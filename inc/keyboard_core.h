#ifndef KEYBOARD_CORE_H_
#define KEYBOARD_CORE_H_

/**
 * @brief 初始化键盘核心模块。
 *
 * 订阅 button_event 和 set_protocol_event，启动按键状态管理与 HID 报告生成。
 *
 * @retval 0 成功，负值为错误码。
 */
int keyboard_core_init(void);

#endif /* KEYBOARD_CORE_H_ */
