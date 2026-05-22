#include "events/hid_consumer_report_event.h"

static void log_hid_consumer_report_event(const struct app_event_header *aeh)
{
	const struct hid_consumer_report_event *event =
		cast_hid_consumer_report_event(aeh);

	APP_EVENT_MANAGER_LOG(aeh,
			      "e:hid_consumer_report id=%u count=%u u0=%u u1=%u u2=%u",
			      event->report_id,
			      event->count,
			      event->usages[0],
			      event->usages[1],
			      event->usages[2]);
}

APP_EVENT_TYPE_DEFINE(hid_consumer_report_event,
					  log_hid_consumer_report_event,
					  NULL,
					  APP_EVENT_FLAGS_CREATE(APP_EVENT_TYPE_FLAGS_INIT_LOG_ENABLE));
