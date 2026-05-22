#include "events/mode_event.h"

static void log_mode_event(const struct app_event_header *aeh)
{
	const struct mode_event *event = cast_mode_event(aeh);

	APP_EVENT_MANAGER_LOG(aeh, "档位=%d", event->mode);
}

APP_EVENT_TYPE_DEFINE(mode_event,
	log_mode_event,
	NULL,
	APP_EVENT_FLAGS_CREATE(APP_EVENT_TYPE_FLAGS_INIT_LOG_ENABLE)
);
