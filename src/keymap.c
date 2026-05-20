#include <stddef.h>			 /* 修复：size_t 和 NULL */
#include <zephyr/sys/util.h> /* 修复：ARRAY_SIZE 宏 */
#include "keymap.h"

/* 下面是你原有的 keymap 数组定义和 keymap_lookup 函数实现... */
/* ── CAF Buttons key_id 编码 ──────────────────────────────────
 * KEY_ID(col_idx, row_idx) = (col_idx << 7) | row_idx
 * 矩阵布局 (4列 × 6行):
 *
 *     C0      C1      C2      C3
 * R0                          EC11_SW  ◎旋钮按键
 * R1  NUM     /       *       -
 * R2  7       8       9       (旋钮转轴)
 * R3  4       5       6       +
 * R4  1       2       3       (旋钮转轴)
 * R5  0       .       Enter   (旋钮转轴)
 *
 * HID Usage 映射:
 *   键盘键:  Usage Page 0x07 (Keyboard/Keypad)
 *   消费者:  Usage Page 0x0C (Consumer)
 * ───────────────────────────────────────────────────────────── */

static const struct keymap_entry keymap[] = {
	/* ── 矩阵按键 → 键盘 Usage ── */
	{.key_id = 0x0001, .hid_usage = 0x53, .type = KEY_TYPE_KBD}, /* NUM     R1C0 */
	{.key_id = 0x0081, .hid_usage = 0x54, .type = KEY_TYPE_KBD}, /* /       R1C1 */
	{.key_id = 0x0101, .hid_usage = 0x55, .type = KEY_TYPE_KBD}, /* *       R1C2 */
	{.key_id = 0x0181, .hid_usage = 0x56, .type = KEY_TYPE_KBD}, /* -       R1C3 */
	{.key_id = 0x0002, .hid_usage = 0x5F, .type = KEY_TYPE_KBD}, /* 7       R2C0 */
	{.key_id = 0x0082, .hid_usage = 0x60, .type = KEY_TYPE_KBD}, /* 8       R2C1 */
	{.key_id = 0x0102, .hid_usage = 0x61, .type = KEY_TYPE_KBD}, /* 9       R2C2 */
	{.key_id = 0x0183, .hid_usage = 0x57, .type = KEY_TYPE_KBD}, /* +       R3C3 */
	{.key_id = 0x0003, .hid_usage = 0x5C, .type = KEY_TYPE_KBD}, /* 4       R3C0 */
	{.key_id = 0x0083, .hid_usage = 0x5D, .type = KEY_TYPE_KBD}, /* 5       R3C1 */
	{.key_id = 0x0103, .hid_usage = 0x5E, .type = KEY_TYPE_KBD}, /* 6       R3C2 */
	{.key_id = 0x0004, .hid_usage = 0x59, .type = KEY_TYPE_KBD}, /* 1       R4C0 */
	{.key_id = 0x0084, .hid_usage = 0x5A, .type = KEY_TYPE_KBD}, /* 2       R4C1 */
	{.key_id = 0x0104, .hid_usage = 0x5B, .type = KEY_TYPE_KBD}, /* 3       R4C2 */
	{.key_id = 0x0185, .hid_usage = 0x58, .type = KEY_TYPE_KBD}, /* Enter   R5C3 */
	{.key_id = 0x0005, .hid_usage = 0x62, .type = KEY_TYPE_KBD}, /* 0       R5C0 */
	{.key_id = 0x0085, .hid_usage = 0x63, .type = KEY_TYPE_KBD}, /* .       R5C1 */

	/* ── 编码器映射键 → 消费者 Usage ── */
	{.key_id = 0x0180, .hid_usage = 0x00E2, .type = KEY_TYPE_CONSUMER}, /* EC11 SW → Mute  */
	{.key_id = 0x00E9, .hid_usage = 0x00E9, .type = KEY_TYPE_CONSUMER}, /* Vol Up           */
	{.key_id = 0x00EA, .hid_usage = 0x00EA, .type = KEY_TYPE_CONSUMER}, /* Vol Down         */
};

const struct keymap_entry *keymap_lookup(uint16_t key_id)
{
	for (size_t i = 0; i < ARRAY_SIZE(keymap); i++)
	{
		if (keymap[i].key_id == key_id)
		{
			return &keymap[i];
		}
	}
	return NULL;
}
