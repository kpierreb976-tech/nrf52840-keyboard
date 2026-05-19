#include "events/battery_event.h"

static void log_battery_event(const struct app_event_header *aeh)
{
	const struct battery_event *event = cast_battery_event(aeh);

	APP_EVENT_MANAGER_LOG(aeh, "e:battery_event lvl=%d st=%d", event->level, event->state);
}

APP_EVENT_TYPE_DEFINE(battery_event,
	log_battery_event,
	NULL,
	APP_EVENT_FLAGS_CREATE(APP_EVENT_TYPE_FLAGS_INIT_LOG_ENABLE)
);
