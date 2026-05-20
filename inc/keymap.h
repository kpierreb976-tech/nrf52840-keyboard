#ifndef KEYMAP_H_
#define KEYMAP_H_

#include <stdint.h>

/** keymap 条目类型 */
#define KEY_TYPE_KBD      0 /**< 键盘键 (Usage Page 0x07) */
#define KEY_TYPE_CONSUMER 1 /**< 消费者键 (Usage Page 0x0C) */

/** 单个键位映射条目 */
struct keymap_entry {
	uint16_t key_id;   /**< button_event 中的 key_id */
	uint16_t hid_usage; /**< 目标 HID Usage ID */
	uint8_t  type;     /**< KEY_TYPE_KBD 或 KEY_TYPE_CONSUMER */
};

/** 查表：根据 key_id 查找对应的 keymap 条目。
 *
 * @param key_id  CAF Buttons 产生的按键 ID
 * @return 找到返回条目指针，未找到返回 NULL
 */
const struct keymap_entry *keymap_lookup(uint16_t key_id);

#endif /* KEYMAP_H_ */
