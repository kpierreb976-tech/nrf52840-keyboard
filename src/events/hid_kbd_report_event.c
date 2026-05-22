#include "events/hid_kbd_report_event.h"

static void log_hid_kbd_report_event(const struct app_event_header *aeh)
{
	const struct hid_kbd_report_event *event = cast_hid_kbd_report_event(aeh);

	APP_EVENT_MANAGER_LOG(aeh,
			      "e:hid_kbd_report fmt=%u id=%u len=%u",
			      event->format,
			      event->report_id,
			      event->len);
}

APP_EVENT_TYPE_DEFINE(hid_kbd_report_event,
					  log_hid_kbd_report_event,
					  NULL,
					  APP_EVENT_FLAGS_CREATE(APP_EVENT_TYPE_FLAGS_INIT_LOG_ENABLE));
