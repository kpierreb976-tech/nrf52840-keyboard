#ifndef MODE_EVENT_H_
#define MODE_EVENT_H_

#include <app_event_manager.h>
#include <app_event_manager_profiler_tracer.h>

struct mode_event {
	struct app_event_header header;
	uint8_t mode;
};

APP_EVENT_TYPE_DECLARE(mode_event);

#endif /* MODE_EVENT_H_ */
