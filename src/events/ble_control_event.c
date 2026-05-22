#include "events/ble_control_event.h"

static void log_ble_control_event(const struct app_event_header *aeh)
{
	const struct ble_control_event *event = cast_ble_control_event(aeh);

	APP_EVENT_MANAGER_LOG(aeh, "命令=%u", event->cmd);
}

APP_EVENT_TYPE_DEFINE(ble_control_event,
		      log_ble_control_event,
		      NULL,
		      APP_EVENT_FLAGS_CREATE(APP_EVENT_TYPE_FLAGS_INIT_LOG_ENABLE));
