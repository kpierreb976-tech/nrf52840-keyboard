#ifndef BLE_STATE_EVENT_H_
#define BLE_STATE_EVENT_H_

#include <app_event_manager.h>
#include <app_event_manager_profiler_tracer.h>

enum ble_transport_state {
	BLE_TRANSPORT_STATE_OFF = 0,
	BLE_TRANSPORT_STATE_ADVERTISING = 1,
	BLE_TRANSPORT_STATE_CONNECTED = 2,
	BLE_TRANSPORT_STATE_ENCRYPTED = 3,
	BLE_TRANSPORT_STATE_READY = 4,
	BLE_TRANSPORT_STATE_ERROR = 5,
};

struct ble_state_event {
	struct app_event_header header;
	uint8_t state;
	bool connected;
	bool bonded;
	int err;
};

APP_EVENT_TYPE_DECLARE(ble_state_event);

#endif /* BLE_STATE_EVENT_H_ */
