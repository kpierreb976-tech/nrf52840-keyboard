#include "events/encoder_event.h"

static void log_encoder_event(const struct app_event_header *aeh)
{
	const struct encoder_event *event = cast_encoder_event(aeh);

	APP_EVENT_MANAGER_LOG(aeh,
			      "e:encoder_event dir=%d steps=%u",
			      event->dir,
			      event->steps);
}

APP_EVENT_TYPE_DEFINE(encoder_event,
	log_encoder_event,
	NULL,
	APP_EVENT_FLAGS_CREATE(APP_EVENT_TYPE_FLAGS_INIT_LOG_ENABLE)
);
