#ifndef HID_CONSUMER_REPORT_EVENT_H_
#define HID_CONSUMER_REPORT_EVENT_H_

#include <app_event_manager.h>
#include <app_event_manager_profiler_tracer.h>

/** 消费者控制 HID 报告事件。
 *
 * 媒体键（音量、静音等）的状态聚合报告。
 * count=0 表示所有消费者键已释放，传输层必须发送清理报告。
 *
 * 传输层约束：发送前需按 Report Descriptor 定义长度将空闲槽位清零补齐。
 */
struct hid_consumer_report_event {
	struct app_event_header header;
	uint8_t  report_id;     /**< HID Report ID */
	uint16_t usages[3];     /**< 当前按下的消费者 Usage，最多 3 个 */
	uint8_t  count;         /**< 有效 usage 数量，0 = 全部释放 */
};

APP_EVENT_TYPE_DECLARE(hid_consumer_report_event);

#endif /* HID_CONSUMER_REPORT_EVENT_H_ */
