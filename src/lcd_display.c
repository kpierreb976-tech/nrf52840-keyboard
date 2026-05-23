#include "lcd_display.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <app_event_manager.h>
#include <lvgl.h>
#include <lvgl_zephyr.h>

#include "events/battery_event.h"
#include "events/ble_state_event.h"
#include "events/mode_event.h"

LOG_MODULE_REGISTER(lcd_display, CONFIG_LOG_DEFAULT_LEVEL);

/* ── 硬件常量 ─────────────────────────────────────────────────── */

#define LCD_BACKLIGHT_NODE DT_NODELABEL(lcd_backlight)
#define LCD_DISPLAY_NODE DT_CHOSEN(zephyr_display)

/* ── LVGL 对象指针（仅 lcd_work_q 内访问） ──────────────────────── */

static lv_obj_t *link_value_label;
static lv_obj_t *mode_value_label;
static lv_obj_t *bat_value_label;

/* ── 工作队列 ──────────────────────────────────────────────────── */

#define LCD_WORK_Q_STACK_SIZE 1024
#define LCD_WORK_Q_PRIO K_PRIO_PREEMPT(8)

static struct k_work_q lcd_work_q;
static K_THREAD_STACK_DEFINE(lcd_work_q_stack, LCD_WORK_Q_STACK_SIZE);

/* ── 状态缓存（仅 lcd_work_q 内读写） ──────────────────────────── */

struct lcd_state_cache {
	uint8_t mode;
	bool    mode_valid;
	uint8_t ble_state;
	bool    link_valid;
	uint8_t battery_level;
	uint8_t battery_state;
	bool    bat_valid;
};

static struct lcd_state_cache lcd_cache;

/* ── 按事件类型拆分的 cache 写入请求 ────────────────────────────── */

static struct {
	uint8_t mode;
} mode_cache_req;
static struct k_work mode_cache_write_work;

static struct {
	uint8_t state;
} ble_cache_req;
static struct k_work ble_cache_write_work;

static struct {
	uint8_t level;
	uint8_t state;
} bat_cache_req;
static struct k_work bat_cache_write_work;

/* ── UI 初始化 work ────────────────────────────────────────────── */

static struct k_work ui_create_work;

/* ── 背光 GPIO ─────────────────────────────────────────────────── */

static const struct gpio_dt_spec lcd_backlight =
	GPIO_DT_SPEC_GET(LCD_BACKLIGHT_NODE, gpios);

static const struct device *const lcd_display =
	DEVICE_DT_GET(LCD_DISPLAY_NODE);

#define LCD_TEST_MAX_WIDTH 320
#define LCD_TEST_CHUNK_WIDTH 64
static uint8_t lcd_test_line[LCD_TEST_MAX_WIDTH * 2] __aligned(4);

static int lcd_display_self_test(void)
{
	struct display_capabilities caps;
	struct display_buffer_descriptor desc;
	int ret;

	display_get_capabilities(lcd_display, &caps);
	LOG_INF("LCD_TEST begin w=%u h=%u", caps.x_resolution,
		caps.y_resolution);
	if (caps.x_resolution > LCD_TEST_MAX_WIDTH) {
		LOG_ERR("LCD test width too large: %u", caps.x_resolution);
		return -EINVAL;
	}

	desc.height = 1;

	for (uint16_t y = 0; y < caps.y_resolution; y++) {
		uint16_t color;
		bool log_row = (y == 0) ||
			       (y == 1) ||
			       (y == caps.y_resolution / 3) ||
			       (y == (caps.y_resolution * 2) / 3);

		if (y < caps.y_resolution / 3) {
			color = 0xf800;
		} else if (y < (caps.y_resolution * 2) / 3) {
			color = 0x07e0;
		} else {
			color = 0x001f;
		}

		for (uint16_t x = 0; x < caps.x_resolution; x++) {
			lcd_test_line[x * 2] = (uint8_t)(color >> 8);
			lcd_test_line[x * 2 + 1] = (uint8_t)(color & 0xff);
		}

		for (uint16_t x = 0; x < caps.x_resolution;
		     x += LCD_TEST_CHUNK_WIDTH) {
			uint16_t chunk_width =
				MIN(LCD_TEST_CHUNK_WIDTH, caps.x_resolution - x);
			bool last_chunk =
				(y + 1 == caps.y_resolution) &&
				(x + chunk_width == caps.x_resolution);

			desc.width = chunk_width;
			desc.pitch = chunk_width;
			desc.buf_size = chunk_width * 2;
			desc.frame_incomplete = !last_chunk;

			if (log_row) {
				LOG_INF("LCD_TEST write y=%u x=%u w=%u",
					y, x, chunk_width);
			}
			ret = display_write(lcd_display, x, y, &desc,
					    &lcd_test_line[x * 2]);
			if (ret < 0) {
				LOG_ERR("LCD test write failed y=%u x=%u err=%d",
					y, x, ret);
				return ret;
			}
			if (log_row) {
				LOG_INF("LCD_TEST wrote y=%u x=%u ret=0",
					y, x);
			}
		}
	}

	LOG_INF("LCD_TEST rgb_bars");
	return 0;
}

/* ── 缓存值 → 显示文本 ────────────────────────────────────────── */

static const char *mode_to_text(uint8_t mode)
{
	switch (mode) {
	case 1: return "USB";
	case 2: return "BLE";
	case 3: return "2G4";
	default: return "--";
	}
}

static const char *ble_state_to_text(uint8_t state)
{
	switch (state) {
	case 0: return "--";
	case 1: return "ADV";
	case 2: return "CON";
	case 3: return "ENC";
	case 4: return "BLE";
	case 5: return "ERR";
	default: return "--";
	}
}

/* ═══════════════════════════════════════════════════════════════════
 * UI 构建（豆沙绿 #C7EDCC 主题，LVGL v9 API）
 * 调用时机：lcd_work_q 内，LVGL 已由 Zephyr auto_init 初始化完毕
 * ═══════════════════════════════════════════════════════════════════ */

static void lcd_create_ui(void)
{
	lv_obj_t *scr = lv_screen_active();
	lv_obj_t *card;
	lv_obj_t *label;

	lv_color_t c_dark_bg = lv_color_hex(0x108210);
	lv_color_t c_douSha = lv_color_hex(0xC7EDCC);
	lv_color_t c_white = lv_color_hex(0xFFFFFF);
	lv_color_t c_black = lv_color_hex(0x000000);

	/* 全屏黑色背景 */
	lv_obj_set_style_bg_color(scr, c_black, LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_STATE_DEFAULT);

	/* ── 标题 ── */
	label = lv_label_create(scr);
	lv_label_set_text(label, "NRF KEYBOARD");
	lv_obj_set_style_text_color(label, c_white, LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(label, &lv_font_montserrat_14,
				   LV_STATE_DEFAULT);
	lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 5);

	/* ── LINK 卡片 ── */
	card = lv_obj_create(scr);
	lv_obj_set_size(card, 146, 44);
	lv_obj_set_pos(card, 8, 30);
	lv_obj_set_style_bg_color(card, c_dark_bg, LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(card, LV_OPA_30, LV_STATE_DEFAULT);
	lv_obj_set_style_border_color(card, c_douSha, LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(card, 2, LV_STATE_DEFAULT);
	lv_obj_set_style_radius(card, 4, LV_STATE_DEFAULT);
	lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);

	label = lv_label_create(card);
	lv_label_set_text(label, "LINK");
	lv_obj_set_style_text_color(label, c_douSha, LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(label, &lv_font_montserrat_14,
				   LV_STATE_DEFAULT);
	lv_obj_align(label, LV_ALIGN_LEFT_MID, 8, 0);

	link_value_label = lv_label_create(card);
	lv_label_set_text(link_value_label, "--");
	lv_obj_set_style_text_color(link_value_label, c_white,
				    LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(link_value_label, &lv_font_montserrat_14,
				   LV_STATE_DEFAULT);
	lv_obj_align(link_value_label, LV_ALIGN_RIGHT_MID, -10, 0);

	/* ── MODE 卡片 ── */
	card = lv_obj_create(scr);
	lv_obj_set_size(card, 146, 44);
	lv_obj_set_pos(card, 166, 30);
	lv_obj_set_style_bg_color(card, c_dark_bg, LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(card, LV_OPA_30, LV_STATE_DEFAULT);
	lv_obj_set_style_border_color(card, c_douSha, LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(card, 2, LV_STATE_DEFAULT);
	lv_obj_set_style_radius(card, 4, LV_STATE_DEFAULT);
	lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);

	label = lv_label_create(card);
	lv_label_set_text(label, "MODE");
	lv_obj_set_style_text_color(label, c_douSha, LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(label, &lv_font_montserrat_14,
				   LV_STATE_DEFAULT);
	lv_obj_align(label, LV_ALIGN_LEFT_MID, 8, 0);

	mode_value_label = lv_label_create(card);
	lv_label_set_text(mode_value_label, "--");
	lv_obj_set_style_text_color(mode_value_label, c_white,
				    LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(mode_value_label, &lv_font_montserrat_14,
				   LV_STATE_DEFAULT);
	lv_obj_align(mode_value_label, LV_ALIGN_RIGHT_MID, -10, 0);

	/* ── BAT 卡片 ── */
	card = lv_obj_create(scr);
	lv_obj_set_size(card, 146, 44);
	lv_obj_set_pos(card, 8, 84);
	lv_obj_set_style_bg_color(card, c_dark_bg, LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(card, LV_OPA_30, LV_STATE_DEFAULT);
	lv_obj_set_style_border_color(card, c_douSha, LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(card, 2, LV_STATE_DEFAULT);
	lv_obj_set_style_radius(card, 4, LV_STATE_DEFAULT);
	lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);

	label = lv_label_create(card);
	lv_label_set_text(label, "BAT");
	lv_obj_set_style_text_color(label, c_douSha, LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(label, &lv_font_montserrat_14,
				   LV_STATE_DEFAULT);
	lv_obj_align(label, LV_ALIGN_LEFT_MID, 8, 0);

	bat_value_label = lv_label_create(card);
	lv_label_set_text(bat_value_label, "--");
	lv_obj_set_style_text_color(bat_value_label, c_white,
				    LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(bat_value_label, &lv_font_montserrat_14,
				   LV_STATE_DEFAULT);
	lv_obj_align(bat_value_label, LV_ALIGN_RIGHT_MID, -10, 0);

	/* ── HID 卡片（静态） ── */
	card = lv_obj_create(scr);
	lv_obj_set_size(card, 146, 44);
	lv_obj_set_pos(card, 166, 84);
	lv_obj_set_style_bg_color(card, c_dark_bg, LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(card, LV_OPA_30, LV_STATE_DEFAULT);
	lv_obj_set_style_border_color(card, c_douSha, LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(card, 2, LV_STATE_DEFAULT);
	lv_obj_set_style_radius(card, 4, LV_STATE_DEFAULT);
	lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);

	label = lv_label_create(card);
	lv_label_set_text(label, "HID");
	lv_obj_set_style_text_color(label, c_douSha, LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(label, &lv_font_montserrat_14,
				   LV_STATE_DEFAULT);
	lv_obj_align(label, LV_ALIGN_LEFT_MID, 8, 0);

	label = lv_label_create(card);
	lv_label_set_text(label, "NKRO");
	lv_obj_set_style_text_color(label, c_white, LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(label, &lv_font_montserrat_14,
				   LV_STATE_DEFAULT);
	lv_obj_align(label, LV_ALIGN_RIGHT_MID, -10, 0);

	/* ── 底部色条（诊断用） ── */
	lv_obj_t *bar;
	lv_color_t bars[] = {
		lv_color_hex(0xF80000),
		lv_color_hex(0x00FF00),
		lv_color_hex(0x0000FF),
		lv_color_hex(0xFFFFFF),
	};
	uint16_t bar_x[] = { 8, 84, 160, 236 };
	uint16_t bar_w[] = { 72, 72, 72, 76 };

	for (int i = 0; i < 4; i++) {
		bar = lv_obj_create(scr);
		lv_obj_set_size(bar, bar_w[i], 10);
		lv_obj_set_pos(bar, bar_x[i], 150);
		lv_obj_set_style_bg_color(bar, bars[i], LV_STATE_DEFAULT);
		lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_STATE_DEFAULT);
		lv_obj_set_style_border_width(bar, 0, LV_STATE_DEFAULT);
		lv_obj_set_style_radius(bar, 0, LV_STATE_DEFAULT);
	}

	LOG_INF("LCD_UI created");
}

/* ── ui_create_work handler ─────────────────────────────────────── */

static void ui_create_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	lvgl_lock();
	lcd_create_ui();
	lvgl_unlock();
}

/* ═══════════════════════════════════════════════════════════════════
 * cache 写入 work handler（lcd_work_q → 写 cache → LVGL label 更新）
 * LVGL timer 在 Zephyr 专用工作队列运行，跨线程访问需持有 lvgl 锁
 * ═══════════════════════════════════════════════════════════════════ */

static void mode_cache_write_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	lcd_cache.mode = mode_cache_req.mode;
	lcd_cache.mode_valid = true;

	LOG_DBG("LCD_EVT mode=%u", lcd_cache.mode);
	lvgl_lock();
	lv_label_set_text(mode_value_label, mode_to_text(lcd_cache.mode));
	lvgl_unlock();
}

static void ble_cache_write_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	lcd_cache.ble_state = ble_cache_req.state;
	lcd_cache.link_valid = true;

	LOG_DBG("LCD_EVT ble=%u", lcd_cache.ble_state);
	lvgl_lock();
	lv_label_set_text(link_value_label,
			  ble_state_to_text(lcd_cache.ble_state));
	lvgl_unlock();
}

static void bat_cache_write_work_handler(struct k_work *work)
{
	char bat_text[8];

	ARG_UNUSED(work);

	lcd_cache.battery_level = bat_cache_req.level;
	lcd_cache.battery_state = bat_cache_req.state;
	lcd_cache.bat_valid = true;

	LOG_DBG("LCD_EVT bat=%u st=%u",
		lcd_cache.battery_level, lcd_cache.battery_state);

	if (lcd_cache.bat_valid) {
		snprintf(bat_text, sizeof(bat_text), "%u%%",
			 lcd_cache.battery_level);
	} else {
		snprintf(bat_text, sizeof(bat_text), "--");
	}

	lvgl_lock();
	lv_label_set_text(bat_value_label, bat_text);
	lvgl_unlock();
}

/* ── CAF 事件 listener（CAF 线程 → 填 req → 投递到 lcd_work_q） ── */

static bool handle_mode_event(const struct app_event_header *aeh)
{
	const struct mode_event *event = cast_mode_event(aeh);

	mode_cache_req.mode = event->mode;
	k_work_submit_to_queue(&lcd_work_q, &mode_cache_write_work);

	return false;
}

static bool handle_battery_event(const struct app_event_header *aeh)
{
	const struct battery_event *event = cast_battery_event(aeh);

	bat_cache_req.level = event->level;
	bat_cache_req.state = event->state;
	k_work_submit_to_queue(&lcd_work_q, &bat_cache_write_work);

	return false;
}

static bool handle_ble_state_event(const struct app_event_header *aeh)
{
	const struct ble_state_event *event = cast_ble_state_event(aeh);

	ble_cache_req.state = event->state;
	k_work_submit_to_queue(&lcd_work_q, &ble_cache_write_work);

	return false;
}

APP_EVENT_LISTENER(lcd_mode_listener, handle_mode_event);
APP_EVENT_SUBSCRIBE(lcd_mode_listener, mode_event);

APP_EVENT_LISTENER(lcd_battery_listener, handle_battery_event);
APP_EVENT_SUBSCRIBE(lcd_battery_listener, battery_event);

APP_EVENT_LISTENER(lcd_ble_state_listener, handle_ble_state_event);
APP_EVENT_SUBSCRIBE(lcd_ble_state_listener, ble_state_event);

/* ── 模块初始化 ────────────────────────────────────────────────── */

int lcd_display_init(void)
{
	LOG_INF("LCD_INIT begin");

	/* 启动专属工作队列 */
	k_work_queue_start(&lcd_work_q,
			   lcd_work_q_stack,
			   K_THREAD_STACK_SIZEOF(lcd_work_q_stack),
			   LCD_WORK_Q_PRIO,
			   NULL);

	/* 初始化 work 项 */
	k_work_init(&mode_cache_write_work, mode_cache_write_work_handler);
	k_work_init(&ble_cache_write_work, ble_cache_write_work_handler);
	k_work_init(&bat_cache_write_work, bat_cache_write_work_handler);
	k_work_init(&ui_create_work, ui_create_work_handler);

	/* 背光 */
	if (!device_is_ready(lcd_backlight.port)) {
		LOG_ERR("LCD 背光 GPIO 未就绪");
		return -ENODEV;
	}

	if (gpio_pin_configure_dt(&lcd_backlight, GPIO_OUTPUT_ACTIVE) < 0) {
		LOG_ERR("LCD 背光配置失败");
		return -EIO;
	}
	LOG_INF("LCD_BL on");

	if (!device_is_ready(lcd_display)) {
		LOG_ERR("LCD display device not ready");
		return -ENODEV;
	}

	if (display_blanking_off(lcd_display) < 0) {
		LOG_ERR("LCD display blanking off failed");
		return -EIO;
	}
	LOG_INF("LCD_DISPLAY on");

	if (lcd_display_self_test() < 0) {
		return -EIO;
	}
	k_sleep(K_SECONDS(2));

	/* 创建 UI widget 树——LVGL 已由 Zephyr auto_init 初始化完毕 */
	k_work_submit_to_queue(&lcd_work_q, &ui_create_work);

	return 0;
}
