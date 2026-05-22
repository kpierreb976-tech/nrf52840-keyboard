#include "events/set_protocol_event.h"

static void log_set_protocol_event(const struct app_event_header *aeh)
{
	const struct set_protocol_event *event = cast_set_protocol_event(aeh);

	APP_EVENT_MANAGER_LOG(aeh, "e:set_protocol protocol=%u", event->protocol);
}

APP_EVENT_TYPE_DEFINE(set_protocol_event,
					  log_set_protocol_event,
					  NULL,
					  APP_EVENT_FLAGS_CREATE(APP_EVENT_TYPE_FLAGS_INIT_LOG_ENABLE));
