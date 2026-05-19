#define DT_DRV_COMPAT voltage_divider

#include "mode_switch.h"

#include <app_event_manager.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "events/mode_event.h"

LOG_MODULE_REGISTER(mode_switch, LOG_LEVEL_DBG);

#define MODE_SENSE_NODE DT_NODELABEL(mode_sense)

/* 电压阈值 (mV)，根据实际硬件电阻分压调整 */
#define THRESHOLD_USB_BLE  800
#define THRESHOLD_BLE_2G4 2500

/* 轮询间隔 — 5 秒足矣，档位切换无需亚秒级响应 */
#define MODE_POLL_INTERVAL_MS 5000

static const struct device *const mode_dev = DEVICE_DT_GET(MODE_SENSE_NODE);

static struct k_work_delayable mode_poll_work;
static enum mode_switch_position last_pos = MODE_SWITCH_POS_UNKNOWN;

static void mode_poll_handler(struct k_work *work)
{
	struct sensor_value val;
	int32_t voltage_mv;
	enum mode_switch_position pos;
	int ret;

	ret = sensor_sample_fetch(mode_dev);
	if (ret < 0) {
		LOG_ERR("Hardware read failed: sample_fetch=%d", ret);
		goto reschedule;
	}

	ret = sensor_channel_get(mode_dev, SENSOR_CHAN_VOLTAGE, &val);
	if (ret < 0) {
		LOG_ERR("Hardware read failed: channel_get=%d", ret);
		goto reschedule;
	}

	voltage_mv = (int32_t)sensor_value_to_milli(&val);

	if (voltage_mv < THRESHOLD_USB_BLE) {
		pos = MODE_SWITCH_POS_USB;
	} else if (voltage_mv < THRESHOLD_BLE_2G4) {
		pos = MODE_SWITCH_POS_BLE;
	} else {
		pos = MODE_SWITCH_POS_2G4;
	}

	/* 只有档位发生实际变化时才提交事件 */
	if (pos != last_pos) {
		last_pos = pos;

		struct mode_event *event = new_mode_event();
		if (event == NULL) {
			LOG_ERR("Event alloc failed!");
			goto reschedule;
		}
		event->mode = (uint8_t)pos;
		APP_EVENT_SUBMIT(event);
	}

reschedule:
	ret = k_work_schedule(&mode_poll_work, K_MSEC(MODE_POLL_INTERVAL_MS));
	if (ret < 0) {
		LOG_ERR("Failed to reschedule mode poll: %d", ret);
	}
}

int mode_switch_init(void)
{
	if (!device_is_ready(mode_dev)) {
		LOG_ERR("Mode sense device not ready");
		return -ENODEV;
	}

	k_work_init_delayable(&mode_poll_work, mode_poll_handler);
	int ret = k_work_schedule(&mode_poll_work, K_MSEC(MODE_POLL_INTERVAL_MS));
	if (ret < 0) {
		LOG_ERR("Failed to schedule first mode poll: %d", ret);
		return ret;
	}

	LOG_DBG("Mode switch detection initialized");
	return 0;
}

int mode_switch_get_position(enum mode_switch_position *pos)
{
	struct sensor_value val;
	int32_t voltage_mv;
	int ret;

	ret = sensor_sample_fetch(mode_dev);
	if (ret < 0) {
		LOG_ERR("Failed to fetch mode sample: %d", ret);
		return ret;
	}

	ret = sensor_channel_get(mode_dev, SENSOR_CHAN_VOLTAGE, &val);
	if (ret < 0) {
		LOG_ERR("Failed to get mode voltage: %d", ret);
		return ret;
	}

	voltage_mv = (int32_t)sensor_value_to_milli(&val);

	if (voltage_mv < THRESHOLD_USB_BLE) {
		*pos = MODE_SWITCH_POS_USB;
	} else if (voltage_mv < THRESHOLD_BLE_2G4) {
		*pos = MODE_SWITCH_POS_BLE;
	} else {
		*pos = MODE_SWITCH_POS_2G4;
	}

	return 0;
}
