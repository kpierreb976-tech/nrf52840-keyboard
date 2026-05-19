#include "events/encoder_event.h"
#include <caf/events/button_event.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(encoder_mapper, LOG_LEVEL_DBG);

/* USB HID Consumer Control 键码 */
#define HID_CONSUMER_VOL_UP   0x00E9
#define HID_CONSUMER_VOL_DOWN 0x00EA

static bool handle_encoder_event(const struct app_event_header *aeh)
{
	const struct encoder_event *event = cast_encoder_event(aeh);
	uint16_t key_id;
	uint8_t count = event->steps;

	if (count == 0) {
		return false;
	}

	key_id = (event->dir > 0) ? HID_CONSUMER_VOL_UP : HID_CONSUMER_VOL_DOWN;

	for (uint8_t i = 0; i < count; i++) {
		struct button_event *btn_press = new_button_event();
		if (btn_press == NULL) {
			LOG_ERR("button_event 内存分配失败 (press)");
			return false;
		}
		btn_press->key_id = key_id;
		btn_press->pressed = true;
		APP_EVENT_SUBMIT(btn_press);

		struct button_event *btn_release = new_button_event();
		if (btn_release == NULL) {
			LOG_ERR("button_event 内存分配失败 (release)");
			return false;
		}
		btn_release->key_id = key_id;
		btn_release->pressed = false;
		APP_EVENT_SUBMIT(btn_release);
	}

	LOG_DBG("编码器映射: dir=%d steps=%u → key_id=0x%04X (%s)",
		event->dir, count, key_id,
		(event->dir > 0) ? "音量加" : "音量减");

	return false;
}

APP_EVENT_LISTENER(encoder_mapper_listener, handle_encoder_event);
APP_EVENT_SUBSCRIBE(encoder_mapper_listener, encoder_event);
