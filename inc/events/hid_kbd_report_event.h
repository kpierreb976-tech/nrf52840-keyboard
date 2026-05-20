#ifndef HID_KBD_REPORT_EVENT_H_
#define HID_KBD_REPORT_EVENT_H_

#include <app_event_manager.h>
#include <app_event_manager_profiler_tracer.h>

/** 键盘 HID 报告线缆报文事件。
 *
 * raw_report 严格对齐 HID 描述符的内存布局，传输层零拷贝直接发送。
 * Boot 模式: len=8, raw_report 布局为 [modifier][0x00][key1..key6]
 * NKRO 模式: len=32, raw_report 布局为 [modifier][bitmap 31B]
 */
struct hid_kbd_report_event {
	struct app_event_header header;
	uint8_t format;         /**< 0=Boot Protocol, 1=NKRO Report Protocol */
	uint8_t report_id;      /**< HID Report ID */
	uint8_t len;            /**< 有效字节数 */
	uint8_t raw_report[32]; /**< 线缆报文，连续内存，直接可发 */
};

APP_EVENT_TYPE_DECLARE(hid_kbd_report_event);

#endif /* HID_KBD_REPORT_EVENT_H_ */
