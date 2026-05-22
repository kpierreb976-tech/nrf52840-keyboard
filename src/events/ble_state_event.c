#include "events/ble_state_event.h"

static const char *ble_state_name(uint8_t state)
{
	switch (state)
	{
	case BLE_TRANSPORT_STATE_OFF:
		return "关闭";
	case BLE_TRANSPORT_STATE_ADVERTISING:
		return "广播";
	case BLE_TRANSPORT_STATE_CONNECTED:
		return "已连接";
	case BLE_TRANSPORT_STATE_ENCRYPTED:
		return "已加密";
	case BLE_TRANSPORT_STATE_READY:
		return "就绪";
	case BLE_TRANSPORT_STATE_ERROR:
		return "错误";
	default:
		return "未知";
	}
}

static void log_ble_state_event(const struct app_event_header *aeh)
{
	const struct ble_state_event *event = cast_ble_state_event(aeh);

	APP_EVENT_MANAGER_LOG(aeh,
			      "状态=%u(%s) 连接=%d bond=%d err=%d",
			      event->state,
			      ble_state_name(event->state),
			      event->connected ? 1 : 0,
			      event->bonded ? 1 : 0,
			      event->err);
}

APP_EVENT_TYPE_DEFINE(ble_state_event,
		      log_ble_state_event,
		      NULL,
		      APP_EVENT_FLAGS_CREATE(APP_EVENT_TYPE_FLAGS_INIT_LOG_ENABLE));
