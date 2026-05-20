#include "keyboard_core.h"
#include "keymap.h"
#include "events/hid_kbd_report_event.h"
#include "events/hid_consumer_report_event.h"
#include "events/set_protocol_event.h"

#include <caf/events/button_event.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(keyboard_core, LOG_LEVEL_DBG);

/* ── 内部状态 ─────────────────────────────────────────────── */

static uint8_t kbd_bitmap[32];           /* 全键位图 256bit 覆盖 Usage 0x00~0xFF */
static uint8_t prev_kbd_report[32];      /* 上次已投递报告（差分基准） */
static uint8_t report_format = 0;        /* 0=Boot, 1=NKRO */

static uint16_t consumer_usages[3];      /* 当前按下的消费者键 */
static uint8_t  consumer_count;
static uint16_t prev_consumer_usages[3]; /* 上次已投递消费者状态 */
static uint8_t  prev_consumer_count;
static bool     kbd_dirty;
static bool     consumer_dirty;

/* ── 报告打包（栈上操作，无堆分配） ─────────────────────────── */

/** 从 bitmap 构建 Boot Protocol 8 字节线缆报文。 */
static void build_boot_report(const uint8_t bitmap[32], uint8_t raw_report[8])
{
	raw_report[0] = bitmap[28];  /* 修饰键: Usage 0xE0~0xE7 → modifier byte */
	raw_report[1] = 0x00;        /* 保留字节 */

	uint8_t idx = 0;
	for (int byt = 0; byt < 32 && idx < 6; byt++) {
		if (byt == 28) continue;   /* 修饰键已归入 raw_report[0]，跳过 */

		uint8_t b = bitmap[byt];
		if (b == 0) continue;

		for (int bit = 0; bit < 8 && idx < 6; bit++) {
			/* 跳过 byte 0 的 Reserved(0x00) 和 ErrorRollOver(0x01) */
			if (byt == 0 && bit < 2) continue;
			if (b & BIT(bit)) {
				raw_report[2 + idx++] = (uint8_t)(byt * 8 + bit);
			}
		}
	}
	while (idx < 6) {
		raw_report[2 + idx++] = 0x00;
	}
}

/** 从 bitmap 构建 NKRO Report 32 字节线缆报文（预留）。 */
static void build_nkro_report(const uint8_t bitmap[32], uint8_t raw_report[32])
{
	raw_report[0] = bitmap[28];
	memcpy(&raw_report[1], &bitmap[0], 31);
	raw_report[1 + 28] = 0x00; /* 清除修饰键在 bitmap 区域的幽灵位 */
}

/* ── 事件提交 ──────────────────────────────────────────────── */

/** 在堆上分配事件并投递键盘报告。仅在差分检测通过后调用。 */
static void submit_kbd_report(void)
{
	struct hid_kbd_report_event *event = new_hid_kbd_report_event();

	if (!event) {
		LOG_ERR("hid_kbd_report_event 内存分配失败");
		return;
	}

	event->format = report_format;
	event->report_id = 1;

	if (report_format == 0) {
		event->len = 8;
		build_boot_report(kbd_bitmap, event->raw_report);
	} else {
		event->len = 32;
		build_nkro_report(kbd_bitmap, event->raw_report);
	}

	APP_EVENT_SUBMIT(event);

	LOG_DBG("键盘报告 fmt=%d: [%02X %02X %02X %02X %02X %02X %02X %02X]",
		report_format,
		event->raw_report[0], event->raw_report[1],
		event->raw_report[2], event->raw_report[3],
		event->raw_report[4], event->raw_report[5],
		event->raw_report[6], event->raw_report[7]);
}

/** 投递消费者控制报告。 */
static void submit_consumer_report(void)
{
	struct hid_consumer_report_event *event = new_hid_consumer_report_event();

	if (!event) {
		LOG_ERR("hid_consumer_report_event 内存分配失败");
		return;
	}

	event->report_id = 2;
	event->count = consumer_count;
	memcpy(event->usages, consumer_usages, sizeof(consumer_usages));

	APP_EVENT_SUBMIT(event);

	LOG_DBG("消费者报告: count=%u usages=[0x%04X 0x%04X 0x%04X]",
		consumer_count,
		consumer_count > 0 ? consumer_usages[0] : 0x0000,
		consumer_count > 1 ? consumer_usages[1] : 0x0000,
		consumer_count > 2 ? consumer_usages[2] : 0x0000);
}

/* ── 差分检测与报告投递 ────────────────────────────────────── */

/**
 * 栈上组装 → 差分比较 → 有变化才分配堆内存投递。
 *
 * 这是关键性能优化：在旋钮快速旋转或按键抖动时，
 * 绝大多数 button_event 不会改变 HID 报告内容（同一键反复按下、消抖期间等），
 * 栈上差分可避免无效的堆内存申请/释放。
 */
static void check_and_submit(void)
{
	/* ── 键盘报告 ── */
	if (kbd_dirty) {
		uint8_t temp_report[32];

		if (report_format == 0) {
			memset(temp_report, 0, sizeof(temp_report));
			build_boot_report(kbd_bitmap, temp_report);
			if (memcmp(temp_report, prev_kbd_report, 8) != 0) {
				memcpy(prev_kbd_report, temp_report, 32);
				submit_kbd_report();
			}
		} else {
			build_nkro_report(kbd_bitmap, temp_report);
			if (memcmp(temp_report, prev_kbd_report, 32) != 0) {
				memcpy(prev_kbd_report, temp_report, 32);
				submit_kbd_report();
			}
		}
		kbd_dirty = false;
	}

	/* ── 消费者报告 ── */
	if (consumer_dirty) {
		submit_consumer_report();
		memcpy(prev_consumer_usages, consumer_usages, sizeof(consumer_usages));
		prev_consumer_count = consumer_count;
		consumer_dirty = false;
	}
}

/* ── 按键处理 ───────────────────────────────────────────────── */

static void process_keyboard_key(const struct keymap_entry *entry, bool pressed)
{
	uint16_t usage = entry->hid_usage;

	if (usage > 0xFF) return;

	int byt = usage >> 3;
	int bit = usage & 0x07;

	if (pressed) {
		kbd_bitmap[byt] |= BIT(bit);
	} else {
		kbd_bitmap[byt] &= ~BIT(bit);
	}
	kbd_dirty = true;
}

static void process_consumer_key(const struct keymap_entry *entry, bool pressed)
{
	uint16_t usage = entry->hid_usage;

	if (pressed) {
		for (uint8_t i = 0; i < consumer_count; i++) {
			if (consumer_usages[i] == usage) return;
		}
		if (consumer_count < 3) {
			consumer_usages[consumer_count++] = usage;
		} else {
			LOG_WRN("消费者键溢出: 0x%04X", usage);
			return;
		}
	} else {
		for (uint8_t i = 0; i < consumer_count; i++) {
			if (consumer_usages[i] == usage) {
				for (uint8_t j = i; j < consumer_count - 1; j++) {
					consumer_usages[j] = consumer_usages[j + 1];
				}
				consumer_count--;
				break;
			}
		}
	}
	consumer_dirty = true;
}

/* ── 事件订阅处理 ──────────────────────────────────────────── */

static bool handle_button_event(const struct app_event_header *aeh)
{
	const struct button_event *event = cast_button_event(aeh);
	const struct keymap_entry *entry = keymap_lookup(event->key_id);

	if (!entry) {
		LOG_DBG("keymap 未匹配: key_id=0x%04X pressed=%d",
			event->key_id, event->pressed);
		return false;
	}

	if (entry->type == KEY_TYPE_KBD) {
		process_keyboard_key(entry, event->pressed);
	} else {
		process_consumer_key(entry, event->pressed);
	}

	check_and_submit();

	return false;
}

static bool handle_set_protocol_event(const struct app_event_header *aeh)
{
	const struct set_protocol_event *event = cast_set_protocol_event(aeh);

	if (event->protocol <= 1) {
		report_format = event->protocol;
		/* 协议切换后强制重发，让主机立刻感知新格式 */
		kbd_dirty = true;
		check_and_submit();
		LOG_INF("协议切换: fmt=%d (%s)",
			report_format,
			report_format == 0 ? "Boot" : "NKRO");
	}
	return false;
}

APP_EVENT_LISTENER(kbd_core_button, handle_button_event);
APP_EVENT_SUBSCRIBE(kbd_core_button, button_event);

APP_EVENT_LISTENER(kbd_core_set_protocol, handle_set_protocol_event);
APP_EVENT_SUBSCRIBE(kbd_core_set_protocol, set_protocol_event);

/* ── 模块初始化 ────────────────────────────────────────────── */

int keyboard_core_init(void)
{
	LOG_INF("键盘核心模块初始化完成 (默认 Boot Protocol)");
	return 0;
}
