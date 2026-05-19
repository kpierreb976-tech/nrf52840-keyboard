#ifndef ENCODER_EVENT_H_
#define ENCODER_EVENT_H_

#include <app_event_manager.h>
#include <app_event_manager_profiler_tracer.h>

struct encoder_event {
	struct app_event_header header;
	/** 旋转方向: 1 = 顺时针 (CW), -1 = 逆时针 (CCW) */
	int8_t  dir;
	/** 旋转偏移量，单位为卡点 (detents)，由驱动完成 steps→detents 转换 */
	uint8_t steps;
};

APP_EVENT_TYPE_DECLARE(encoder_event);

#endif /* ENCODER_EVENT_H_ */
