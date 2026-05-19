#include "events/encoder_event.h"

static void log_encoder_event(const struct app_event_header *aeh)
{
	const struct encoder_event *event = cast_encoder_event(aeh);

	/* 旋转事件已由 encoder_mapper 转发为 button_event，
	 * 此处不再打印，避免与 mapper 日志重复刷屏。
	 */
}

APP_EVENT_TYPE_DEFINE(encoder_event,
	log_encoder_event,
	NULL,
	APP_EVENT_FLAGS_CREATE(APP_EVENT_TYPE_FLAGS_INIT_LOG_ENABLE)
);
