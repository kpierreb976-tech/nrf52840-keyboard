# USB/BLE HID 传输修复方案

## 设计意图

本次修复收敛三个传输层问题：

- USB 传输层必须服从拨档状态，非 USB 档位不应消费键盘或消费者控制事件生成实际 USB TX。
- BLE HID Notify 在连续按键下需要更大的底层 TX 缓冲资源，降低断连和发送失败概率。
- USB HID 类请求需要显式支持 `Set Idle` / `Get Idle`，避免主机枚举或协议初始化阶段收到未实现响应。

## 实现方案

- `usb_transport` 订阅 `mode_event`，维护 `usb_mode_active` 状态；事件入口和发送线程双重检查该状态，切出 USB 档位时清空 `usb_tx_ring`。
- `prj.conf` 成组提高 BLE ACL、连接、L2CAP、ATT TX 缓冲计数。
- `ble_transport` 使用模块专属工作队列执行 GATT Notify，避免从系统工作队列调用带完成回调的 `bt_gatt_notify_cb()` 时触发资源等待失败。
- BLE 键盘报告采用最新状态优先策略；底层拥塞或本地队列满时保留最新键盘状态，优先保证松开包不会被旧按下包挤掉。
- `usb_transport` 增加 HID Idle 状态表，注册 `set_idle` 与 `get_idle` 回调；`id=0` 时同步应用到所有已声明 Report ID。

## 预期日志

```text
<inf> usb_transport: USB 档位激活=1
<dbg> usb_transport: 非 USB 档位，丢弃键盘 TX
<dbg> usb_transport: HID SetIdle id=1 duration=0ms
<dbg> usb_transport: HID GetIdle id=1 duration=0ms
```
