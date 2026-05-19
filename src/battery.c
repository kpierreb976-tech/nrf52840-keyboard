#define DT_DRV_COMPAT voltage_divider

#include "battery.h"

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(battery, LOG_LEVEL_INF);

#define VBATT_NODE DT_NODELABEL(vbatt)

static const struct device *const vbatt_dev = DEVICE_DT_GET(VBATT_NODE);

int battery_init(void)
{
	if (!device_is_ready(vbatt_dev)) {
		LOG_ERR("Battery voltage divider device not ready");
		return -ENODEV;
	}

	LOG_INF("Battery measurement initialized");
	return 0;
}

int battery_get_voltage_mv(int32_t *voltage_mv)
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

	*voltage_mv = (int32_t)sensor_value_to_milli(&val);
	LOG_DBG("Battery: %d mV", *voltage_mv);
	return 0;
}
