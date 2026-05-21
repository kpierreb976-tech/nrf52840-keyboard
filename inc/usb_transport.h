#ifndef USB_TRANSPORT_H_
#define USB_TRANSPORT_H_

/**
 * @brief 初始化 USB 传输层。
 *
 * 注册 HID 报告描述符与 USB 设备配置，创建低优先级传输线程与环形缓冲区，
 * 订阅 hid_kbd_report_event 和 hid_consumer_report_event 事件。
 *
 * @retval 0 成功，负值为错误码。
 */
int usb_transport_init(void);

#endif /* USB_TRANSPORT_H_ */
