#ifndef BLE_TRANSPORT_H_
#define BLE_TRANSPORT_H_

/**
 * @brief 初始化 BLE HID 传输层。
 *
 * 初始化蓝牙 Host、加载 settings 持久化数据，并根据当前拨档状态决定是否
 * 启动可连接广播。后续 HID 报告通过 CAF 事件进入本模块。
 *
 * @retval 0 成功，负值为错误码。
 */
int ble_transport_init(void);

#endif /* BLE_TRANSPORT_H_ */
