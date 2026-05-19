#ifndef POWER_MGMT_H_
#define POWER_MGMT_H_

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Initialize the power management subsystem.
 *
 * Sets up I2C communication with IP5305T, the battery ADC voltage divider,
 * VBUS detection GPIO, and the WAKEUP pulse timer.
 *
 * @retval 0 on success, negative error code otherwise.
 */
int power_mgmt_init(void);

/**
 * @brief Get the battery voltage.
 *
 * Powers up the measurement circuit, samples the ADC, and powers down.
 *
 * @param[out] voltage_mv Battery voltage in millivolts.
 * @retval 0 on success, negative error code otherwise.
 */
int power_mgmt_get_battery_voltage_mv(int32_t *voltage_mv);

/**
 * @brief Get the battery charge level from the IP5305T fuel gauge.
 *
 * @param[out] level_pct Charge level in percent (0–100).
 * @retval 0 on success, negative error code otherwise.
 */
int power_mgmt_get_battery_level(uint8_t *level_pct);

/**
 * @brief Check whether the battery is currently charging.
 *
 * @param[out] charging true if charging, false otherwise.
 * @retval 0 on success, negative error code otherwise.
 */
int power_mgmt_is_charging(bool *charging);

/**
 * @brief Check whether the battery is fully charged.
 *
 * @param[out] full true if fully charged, false otherwise.
 * @retval 0 on success, negative error code otherwise.
 */
int power_mgmt_is_fully_charged(bool *full);

/**
 * @brief Check whether VBUS (USB 5V) is present.
 *
 * @param[out] present true if VBUS is detected, false otherwise.
 * @retval 0 on success, negative error code otherwise.
 */
int power_mgmt_is_vbus_present(bool *present);

#endif /* POWER_MGMT_H_ */
