#ifndef BATTERY_EVENT_H_
#define BATTERY_EVENT_H_

#include <app_event_manager.h>
#include <app_event_manager_profiler_tracer.h>

struct battery_event {
	struct app_event_header header;
	uint8_t level;
	uint8_t state;
};

APP_EVENT_TYPE_DECLARE(battery_event);

#endif /* BATTERY_EVENT_H_ */
