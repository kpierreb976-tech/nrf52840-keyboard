#define DT_DRV_COMPAT voltage_divider

#include "power_mgmt.h"

#include <app_event_manager.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/usb/usb_dc.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device_runtime.h>

#include "events/battery_event.h"

#ifndef CONFIG_POWER_MGMT_LOG_LEVEL
#define POWER_MGMT_LOG_LEVEL CONFIG_LOG_DEFAULT_LEVEL
#else
#define POWER_MGMT_LOG_LEVEL CONFIG_POWER_MGMT_LOG_LEVEL
#endif
LOG_MODULE_REGISTER(power_mgmt, POWER_MGMT_LOG_LEVEL);

/* ── Device tree nodes ───────────────────────────────────────────────── */

#define VBATT_NODE DT_NODELABEL(vbatt)
#define IP5305T_NODE DT_NODELABEL(ip5305t)

/* ── IP5305T I2C ─────────────────────────────────────────────────────── */

#define IP5305T_REG_STATUS 0x78
#define IP5305T_REG_CHARGE_STATUS 0x71

/* bits in STATUS register (0x78) */
#define IP5305T_CHARGING BIT(2)
#define IP5305T_FULL BIT(3)
#define IP5305T_LEVEL_MSK GENMASK(7, 4)
#define IP5305T_LEVEL_POS 4

/* ── Battery voltage thresholds (mV) ─────────────────────────────────── */

#define BAT_VOLTAGE_MIN 3200
#define BAT_VOLTAGE_MAX 4200
#define BAT_VOLTAGE_FULL_MV 4150

/* ── WAKEUP pulse timing ─────────────────────────────────────────────── */

#define WAKEUP_PERIOD_SEC 12
#define WAKEUP_PULSE_MS 200

/* ── GPIO pin definitions (from overlay DTS) ─────────────────────────── */

#define WAKEUP_GPIO_PIN 22

/* ── Static resources ────────────────────────────────────────────────── */

static const struct i2c_dt_spec ip5305t =
    I2C_DT_SPEC_GET(IP5305T_NODE);

static const struct device *const vbatt_dev =
    DEVICE_DT_GET(VBATT_NODE);

static const struct gpio_dt_spec wakeup_gpio = {
    .port = DEVICE_DT_GET(DT_NODELABEL(gpio0)),
    .pin = WAKEUP_GPIO_PIN,
    .dt_flags = GPIO_ACTIVE_LOW,
};

static const struct gpio_dt_spec bat_adc_en_spec =
    GPIO_DT_SPEC_GET_BY_IDX(VBATT_NODE, power_gpios, 0);

/* ── VBUS state (updated by USBD hardware event callback) ────────────── */

static bool vbus_present;

static void vbus_status_cb(enum usb_dc_status_code status, const uint8_t *param)
{
    switch (status)
    {
    case USB_DC_CONNECTED:
        vbus_present = true;
        LOG_DBG("VBUS detected via USBD hardware");
        break;
    case USB_DC_DISCONNECTED:
        vbus_present = false;
        LOG_DBG("VBUS removed via USBD hardware");
        break;
    default:
        break;
    }
}

/* ── WAKEUP delayed work ─────────────────────────────────────────────── */

static struct k_work_delayable wakeup_work;

/* ── Helper ──────────────────────────────────────────────────────────── */

static uint8_t voltage_to_battery_pct(int32_t voltage_mv)
{
    if (voltage_mv >= BAT_VOLTAGE_MAX)
    {
        return 100;
    }
    if (voltage_mv <= BAT_VOLTAGE_MIN)
    {
        return 0;
    }
    return (uint8_t)((voltage_mv - BAT_VOLTAGE_MIN) * 100U /
                     (BAT_VOLTAGE_MAX - BAT_VOLTAGE_MIN));
}

static void wakeup_work_handler(struct k_work *work)
{
    int ret;

    /*
     * Drive WAKEUP pin hard to physical 0 V — routing through the raw
     * GPIO API to guarantee the strongest possible low level, bypassing
     * any active-low logical inversion that could weaken the drive.
     */
    ret = gpio_pin_configure(wakeup_gpio.port, wakeup_gpio.pin,
                             GPIO_OUTPUT);
    if (ret < 0)
    {
        LOG_ERR("WAKEUP output config failed: %d", ret);
    }
    ret = gpio_pin_set(wakeup_gpio.port, wakeup_gpio.pin, 0);
    if (ret < 0)
    {
        LOG_ERR("WAKEUP low failed: %d", ret);
    }

    k_sleep(K_MSEC(WAKEUP_PULSE_MS));

    /*
     * Release the pin to high-impedance input so the IP5305T internal
     * pull-up can restore the line cleanly, and the MCU does not leak
     * current through a continuously driven output.
     */
    ret = gpio_pin_configure(wakeup_gpio.port, wakeup_gpio.pin,
                             GPIO_INPUT);
    if (ret < 0)
    {
        LOG_ERR("WAKEUP input config failed: %d", ret);
    }

    k_sleep(K_MSEC(100));

    /* ── 强制 ADC 采样获取真实电压（唯一电量数据源） ──────── */
    int32_t voltage_mv;
    ret = power_mgmt_get_battery_voltage_mv(&voltage_mv);
    if (ret < 0)
    {
        goto skip_battery;
    }
    uint8_t level_pct = voltage_to_battery_pct(voltage_mv);

    /* ── 读取 0x71 寄存器判定充电状态 ──────────────────────── */
    uint8_t reg_71 = 0;
    ret = i2c_reg_read_byte_dt(&ip5305t, IP5305T_REG_CHARGE_STATUS, &reg_71);
    if (ret < 0)
    {
        LOG_ERR("充电状态读取失败: %d", ret);
        goto skip_battery;
    }
    bool charging = (reg_71 != 0x00);

    /* ── 状态判定：充电中 + 电压 >= 4150mV → 满电 ─────────── */
    uint8_t current_state = 0;
    if (charging && voltage_mv >= BAT_VOLTAGE_FULL_MV)
    {
        current_state = 2;
    }
    else if (charging)
    {
        current_state = 1;
    }

    /* ── UI 输出：仅在 wakeup handler 中呈现电池状态 ──────── */
    if (current_state == 2)
    {
        LOG_INF("电池 %u%% %dmV 已满", level_pct, voltage_mv);
    }
    else if (current_state == 1)
    {
        LOG_INF("电池 %u%% %dmV 充电", level_pct, voltage_mv);
    }
    else
    {
        LOG_INF("电池 %u%% %dmV 放电", level_pct, voltage_mv);
    }

    struct battery_event *event = new_battery_event();
    if (event == NULL)
    {
        LOG_ERR("Event alloc failed!");
        goto skip_battery;
    }
    event->level = level_pct;
    event->state = current_state;

    APP_EVENT_SUBMIT(event);

skip_battery:

    ret = k_work_schedule(&wakeup_work, K_SECONDS(WAKEUP_PERIOD_SEC));
    if (ret < 0)
    {
        LOG_ERR("Failed to reschedule wakeup: %d", ret);
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */

int power_mgmt_init(void)
{
    int ret;

    /* VBUS 检测由 USB Next Stack 负责；此模块不再挂旧 usb_dc 回调。 */
    ret = 0;
    LOG_INF("VBUS detection switched to USBD hardware event");

    /* BAT_ADC_EN GPIO — 控制分压电路 N-MOS 导通/关断 */
    if (!device_is_ready(bat_adc_en_spec.port))
    {
        LOG_ERR("BAT_ADC_EN GPIO not ready");
        return -ENODEV;
    }
    ret = gpio_pin_configure_dt(&bat_adc_en_spec, GPIO_OUTPUT_INACTIVE);
    if (ret < 0)
    {
        LOG_ERR("BAT_ADC_EN GPIO config failed: %d", ret);
        return ret;
    }

    /* WAKEUP GPIO */
    if (!gpio_is_ready_dt(&wakeup_gpio))
    {
        LOG_ERR("WAKEUP GPIO not ready");
        return -ENODEV;
    }
    ret = gpio_pin_configure_dt(&wakeup_gpio, GPIO_OUTPUT_INACTIVE);
    if (ret < 0)
    {
        LOG_ERR("WAKEUP GPIO config failed: %d", ret);
        return ret;
    }

    /* IP5305T I2C */
    if (!device_is_ready(ip5305t.bus))
    {
        LOG_ERR("I2C bus not ready");
        return -ENODEV;
    }

    /* Battery voltage divider */
    if (!device_is_ready(vbatt_dev))
    {
        LOG_ERR("Battery voltage divider not ready");
        return -ENODEV;
    }

    /* Initialise and start periodic WAKEUP pulses (first after 5 s) */
    k_work_init_delayable(&wakeup_work, wakeup_work_handler);
    ret = k_work_schedule(&wakeup_work, K_SECONDS(5));
    if (ret < 0)
    {
        LOG_ERR("Failed to schedule first wakeup: %d", ret);
        return ret;
    }

    LOG_DBG("Power management initialized");
    return 0;
}

int power_mgmt_get_battery_voltage_mv(int32_t *voltage_mv)
{
    struct sensor_value val;
    int ret;

    /* 拉高 BAT_ADC_EN，导通 N-MOS 使分压电路生效 */
    ret = gpio_pin_set_dt(&bat_adc_en_spec, 1);
    if (ret < 0)
    {
        LOG_ERR("Failed to assert BAT_ADC_EN: %d", ret);
        return ret;
    }

    /* τ = R6||R8 × C29 = 50kΩ × 100nF = 5ms，等待 6τ = 30ms 确保稳态误差 < 1% */
    k_sleep(K_MSEC(30));

    ret = pm_device_runtime_get(vbatt_dev);
    if (ret < 0)
    {
        LOG_ERR("Failed to enable battery measurement: %d", ret);
        (void)gpio_pin_set_dt(&bat_adc_en_spec, 0);
        return ret;
    }

    ret = sensor_sample_fetch(vbatt_dev);
    if (ret < 0)
    {
        LOG_ERR("Failed to fetch sample: %d", ret);
        (void)pm_device_runtime_put(vbatt_dev);
        (void)gpio_pin_set_dt(&bat_adc_en_spec, 0);
        return ret;
    }

    ret = sensor_channel_get(vbatt_dev, SENSOR_CHAN_VOLTAGE, &val);
    if (ret < 0)
    {
        LOG_ERR("Failed to get voltage: %d", ret);
        (void)pm_device_runtime_put(vbatt_dev);
        (void)gpio_pin_set_dt(&bat_adc_en_spec, 0);
        return ret;
    }

    (void)pm_device_runtime_put(vbatt_dev);

    /* 拉低 BAT_ADC_EN，关断分压电路以降低休眠功耗 */
    (void)gpio_pin_set_dt(&bat_adc_en_spec, 0);

    *voltage_mv = (int32_t)sensor_value_to_milli(&val);
    return 0;
}

int power_mgmt_get_battery_level(uint8_t *level_pct)
{
    uint8_t data;
    int ret;

    ret = i2c_reg_read_byte_dt(&ip5305t, IP5305T_REG_STATUS, &data);
    LOG_DBG("I2C read ret: %d, reg_data: 0x%02x", ret, data);
    if (ret == 0)
    {
        uint8_t segments = (data & IP5305T_LEVEL_MSK) >> IP5305T_LEVEL_POS;

        /*
         * IP5305T uses a thermometer code for the 4-segment battery
         * gauge.  0b0000 → 0%, 0b0001 → 25%, 0b0011 → 50%,
         * 0b0111 → 75%, 0b1111 → 100%
         */
        switch (segments)
        {
        case 0x0:
            *level_pct = 0;
            break;
        case 0x1:
            *level_pct = 25;
            break;
        case 0x3:
            *level_pct = 50;
            break;
        case 0x7:
            *level_pct = 75;
            break;
        case 0xF:
            *level_pct = 100;
            break;
        default:
            /* approximate for any intermediate code */
            *level_pct = (uint8_t)(segments * 100 / 15);
            break;
        }
        return 0;
    }

    /* IP5305T unreachable (e.g. in sleep) — fall back to voltage gauge */
    LOG_DBG("IP5305T I2C failed (%d), using voltage-based level", ret);

    int32_t voltage_mv;
    ret = power_mgmt_get_battery_voltage_mv(&voltage_mv);
    if (ret < 0)
    {
        return ret;
    }

    *level_pct = voltage_to_battery_pct(voltage_mv);
    LOG_DBG("Battery level (voltage): %u%% (%dmV)", *level_pct, voltage_mv);
    return 0;
}

int power_mgmt_is_charging(bool *charging)
{
    uint8_t data;
    int ret;

    ret = i2c_reg_read_byte_dt(&ip5305t, IP5305T_REG_STATUS, &data);
    if (ret < 0)
    {
        LOG_ERR("Failed to read IP5305T status: %d", ret);
        return ret;
    }

    *charging = (data & IP5305T_CHARGING) != 0;
    return 0;
}

int power_mgmt_is_fully_charged(bool *full)
{
    uint8_t data;
    int ret;

    ret = i2c_reg_read_byte_dt(&ip5305t, IP5305T_REG_STATUS, &data);
    if (ret < 0)
    {
        LOG_ERR("Failed to read IP5305T status: %d", ret);
        return ret;
    }

    *full = (data & IP5305T_FULL) != 0;
    return 0;
}

int power_mgmt_is_vbus_present(bool *present)
{
    *present = vbus_present;
    return 0;
}
