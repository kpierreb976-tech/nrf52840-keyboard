#ifndef BLE_CONTROL_EVENT_H_
#define BLE_CONTROL_EVENT_H_

#include <app_event_manager.h>
#include <app_event_manager_profiler_tracer.h>

enum ble_control_cmd {
	BLE_CONTROL_CMD_CLEAR_BONDS = 1,
};

struct ble_control_event {
	struct app_event_header header;
	uint8_t cmd;
};

APP_EVENT_TYPE_DECLARE(ble_control_event);

#endif /* BLE_CONTROL_EVENT_H_ */
