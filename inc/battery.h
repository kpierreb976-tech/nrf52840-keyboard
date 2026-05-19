#ifndef BATTERY_H_
#define BATTERY_H_

#include <stdint.h>

/**
 * @brief Initialize the battery voltage measurement subsystem.
 *
 * @retval 0 on success, negative error code otherwise.
 */
int battery_init(void);

/**
 * @brief Get the current battery voltage.
 *
 * Powers up the measurement circuit, samples the ADC, converts raw
 * value to millivolts, and powers down the circuit.
 *
 * @param[out] voltage_mv Pointer to store the battery voltage in mV.
 * @retval 0 on success, negative error code otherwise.
 */
int battery_get_voltage_mv(int32_t *voltage_mv);

#endif /* BATTERY_H_ */
