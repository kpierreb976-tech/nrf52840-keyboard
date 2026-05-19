#define DT_DRV_COMPAT voltage_divider

#include "power_mgmt.h"

#include <app_event_manager.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device_runtime.h>

#include "events/battery_event.h"

LOG_MODULE_REGISTER(power_mgmt, LOG_LEVEL_DBG);

/* ── Device tree nodes ───────────────────────────────────────────────── */

#define VBATT_NODE   DT_NODELABEL(vbatt)
#define IP5305T_NODE DT_NODELABEL(ip5305t)

/* ── IP5305T I2C ─────────────────────────────────────────────────────── */

#define IP5305T_REG_STATUS 0x78

/* bits in STATUS register (0x78) */
#define IP5305T_CHARGING  BIT(2)
#define IP5305T_FULL      BIT(3)
#define IP5305T_LEVEL_MSK GENMASK(7, 4)
#define IP5305T_LEVEL_POS 4

/* ── Battery voltage thresholds (mV) ─────────────────────────────────── */

#define BAT_VOLTAGE_MIN 3200
#define BAT_VOLTAGE_MAX 4200

/* ── WAKEUP pulse timing ─────────────────────────────────────────────── */

#define WAKEUP_PERIOD_SEC 12
#define WAKEUP_PULSE_MS   200

/* ── GPIO pin definitions (from overlay DTS) ─────────────────────────── */

#define VBUS_GPIO_PIN   10
#define WAKEUP_GPIO_PIN 22

/* ── Static resources ────────────────────────────────────────────────── */

static const struct i2c_dt_spec ip5305t =
	I2C_DT_SPEC_GET(IP5305T_NODE);

static const struct device *const vbatt_dev =
	DEVICE_DT_GET(VBATT_NODE);

static const struct gpio_dt_spec vbus_gpio = {
	.port = DEVICE_DT_GET(DT_NODELABEL(gpio0)),
	.pin  = VBUS_GPIO_PIN,
	.dt_flags = GPIO_PULL_DOWN,
};

static const struct gpio_dt_spec wakeup_gpio = {
	.port = DEVICE_DT_GET(DT_NODELABEL(gpio0)),
	.pin  = WAKEUP_GPIO_PIN,
	.dt_flags = GPIO_ACTIVE_LOW,
};

/* ── WAKEUP delayed work ─────────────────────────────────────────────── */

static struct k_work_delayable wakeup_work;

static void wakeup_work_handler(struct k_work *work)
{
	int ret;

	LOG_DBG("Sending Keep-Alive pulse (low %ums)", WAKEUP_PULSE_MS);

	/*
	 * Drive WAKEUP pin hard to physical 0 V — routing through the raw
	 * GPIO API to guarantee the strongest possible low level, bypassing
	 * any active-low logical inversion that could weaken the drive.
	 */
	ret = gpio_pin_configure(wakeup_gpio.port, wakeup_gpio.pin,
				 GPIO_OUTPUT);
	if (ret < 0) {
		LOG_ERR("WAKEUP output config failed: %d", ret);
	}
	ret = gpio_pin_set(wakeup_gpio.port, wakeup_gpio.pin, 0);
	if (ret < 0) {
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
	if (ret < 0) {
		LOG_ERR("WAKEUP input config failed: %d", ret);
	}

	/* ── Battery: read & publish to event bus ─────────────────── */
	uint8_t level_pct;
	power_mgmt_get_battery_level(&level_pct);

	bool charging = false;
	bool full = false;
	power_mgmt_is_charging(&charging);
	power_mgmt_is_fully_charged(&full);

	uint8_t current_state = 0;
	if (full) {
		current_state = 2;
	} else if (charging) {
		current_state = 1;
	}

	struct battery_event *event = new_battery_event();
	if (event == NULL) {
		LOG_ERR("Event alloc failed!");
		goto skip_battery;
	}
	event->level = level_pct;
	event->state = current_state;

	APP_EVENT_SUBMIT(event);

skip_battery:

	/* Reschedule for next keep-alive pulse */
	ret = k_work_schedule(&wakeup_work, K_SECONDS(WAKEUP_PERIOD_SEC));
	if (ret < 0) {
		LOG_ERR("Failed to reschedule wakeup: %d", ret);
	}
}

/* ── Helper ──────────────────────────────────────────────────────────── */

static uint8_t voltage_to_battery_pct(int32_t voltage_mv)
{
	if (voltage_mv >= BAT_VOLTAGE_MAX) {
		return 100;
	}
	if (voltage_mv <= BAT_VOLTAGE_MIN) {
		return 0;
	}
	return (uint8_t)((voltage_mv - BAT_VOLTAGE_MIN) * 100U /
			 (BAT_VOLTAGE_MAX - BAT_VOLTAGE_MIN));
}

/* ── Public API ──────────────────────────────────────────────────────── */

int power_mgmt_init(void)
{
	int ret;

	/* VBUS detect GPIO */
	if (!gpio_is_ready_dt(&vbus_gpio)) {
		LOG_ERR("VBUS GPIO not ready");
		return -ENODEV;
	}
	ret = gpio_pin_configure_dt(&vbus_gpio, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("VBUS GPIO config failed: %d", ret);
		return ret;
	}

	/* WAKEUP GPIO */
	if (!gpio_is_ready_dt(&wakeup_gpio)) {
		LOG_ERR("WAKEUP GPIO not ready");
		return -ENODEV;
	}
	ret = gpio_pin_configure_dt(&wakeup_gpio, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("WAKEUP GPIO config failed: %d", ret);
		return ret;
	}

	/* IP5305T I2C */
	if (!device_is_ready(ip5305t.bus)) {
		LOG_ERR("I2C bus not ready");
		return -ENODEV;
	}

	/* Battery voltage divider */
	if (!device_is_ready(vbatt_dev)) {
		LOG_ERR("Battery voltage divider not ready");
		return -ENODEV;
	}

	/* Initialise and start periodic WAKEUP pulses (first after 5 s) */
	k_work_init_delayable(&wakeup_work, wakeup_work_handler);
	ret = k_work_schedule(&wakeup_work, K_SECONDS(5));
	if (ret < 0) {
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

	ret = pm_device_runtime_get(vbatt_dev);
	if (ret < 0) {
		LOG_ERR("Failed to enable battery measurement: %d", ret);
		return ret;
	}

	ret = sensor_sample_fetch(vbatt_dev);
	if (ret < 0) {
		LOG_ERR("Failed to fetch sample: %d", ret);
		(void)pm_device_runtime_put(vbatt_dev);
		return ret;
	}

	ret = sensor_channel_get(vbatt_dev, SENSOR_CHAN_VOLTAGE, &val);
	if (ret < 0) {
		LOG_ERR("Failed to get voltage: %d", ret);
		(void)pm_device_runtime_put(vbatt_dev);
		return ret;
	}

	(void)pm_device_runtime_put(vbatt_dev);

	/*
	 * Zephyr voltage_divider driver already applies the full-ohms /
	 * output-ohms ratio (2×) from the device tree. The sensor
	 * subsystem additionally reports an already-multiplied value,
	 * so we divide by 2 here to obtain the real battery voltage.
	 */
	*voltage_mv = (int32_t)sensor_value_to_milli(&val) / 2;
	return 0;
}

int power_mgmt_get_battery_level(uint8_t *level_pct)
{
	uint8_t data;
	int ret;

	ret = i2c_reg_read_byte_dt(&ip5305t, IP5305T_REG_STATUS, &data);
	if (ret == 0) {
		uint8_t segments = (data & IP5305T_LEVEL_MSK) >> IP5305T_LEVEL_POS;

		/*
		 * IP5305T uses a thermometer code for the 4-segment battery
		 * gauge.  0b0000 → 0%, 0b0001 → 25%, 0b0011 → 50%,
		 * 0b0111 → 75%, 0b1111 → 100%
		 */
		switch (segments) {
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
	if (ret < 0) {
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
	if (ret < 0) {
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
	if (ret < 0) {
		LOG_ERR("Failed to read IP5305T status: %d", ret);
		return ret;
	}

	*full = (data & IP5305T_FULL) != 0;
	return 0;
}

int power_mgmt_is_vbus_present(bool *present)
{
	int ret;

	ret = gpio_pin_get_dt(&vbus_gpio);
	if (ret < 0) {
		LOG_ERR("Failed to read VBUS GPIO: %d", ret);
		return ret;
	}

	*present = (ret != 0);
	return 0;
}
