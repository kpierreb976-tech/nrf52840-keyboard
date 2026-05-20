#ifndef SET_PROTOCOL_EVENT_H_
#define SET_PROTOCOL_EVENT_H_

#include <app_event_manager.h>
#include <app_event_manager_profiler_tracer.h>

/** set_protocol 事件，由 USB/BLE 承载层在收到主机 Set_Protocol 请求时投递。
 *
 * 当前暂无生产者，预留此事件类型供 keyboard_core 订阅。
 */
struct set_protocol_event {
	struct app_event_header header;
	uint8_t protocol; /**< 0=Boot Protocol, 1=Report Protocol (NKRO) */
};

APP_EVENT_TYPE_DECLARE(set_protocol_event);

#endif /* SET_PROTOCOL_EVENT_H_ */
