# BLE 传输层设计方案（ble_transport.c）

## 1. 需求概述

实现一个“模式感知”的蓝牙 HID 键盘传输层。该模块只负责 BLE 连接、配对、广播、HID over GATT 报告发送与持久化恢复，不直接读取矩阵、旋钮或键盘核心状态。

数据来源继续沿用现有 CAF 事件：

- `hid_kbd_report_event`：键盘报告，支持 Boot / NKRO 原始载荷。
- `hid_consumer_report_event`：消费者控制报告，用于音量、静音等多媒体键。
- `mode_event`：拨档位置事件，用于判断当前是否进入 BLE 模式。
- `set_protocol_event`：协议切换事件，供 `keyboard_core` 同步 Boot / Report 状态。

核心目标：

- BLE 档位时自动广播或恢复连接。
- 非 BLE 档位时停止广播并进入低功耗连接策略。
- 使用 Zephyr settings 后端保存蓝牙 bonding 信息，避免断电后每次重新配对。
- 传输层只消费 HID 事件，不反向读取 `keyboard_core` 内部状态，保持单职权边界。

## 2. 设备树审计

当前 `C:/NRF/KEYBOARD/boards/arm/key_board/key_board.overlay` 已归位。

已确认：

- `&usbd { status = "okay"; }` 已存在，USB 传输层继续可用。
- `hid_dev_0` 节点已存在，用于 USB HID 设备。

已确认实际 board DTS `boards/atjialidun/key_board/key_board.dts` 已包含固定分区：

```dts
storage_partition: partition@f8000 {
    label = "storage";
    reg = <0x000f8000 DT_SIZE_K(32)>;
};
```

说明：

- 当前项目已经采用 MCUboot + 双镜像分区：`mcuboot`、`image-0`、`image-1`、`storage`。
- BLE bonding / CCCD / settings 直接复用现有 32KB `storage_partition`。
- overlay 中禁止再次定义 `storage_partition`，否则会触发 devicetree label 重复错误。

## 3. Kconfig 校验

需要新增以下配置项：

```conf
# ==============================================================================
# BLE HID 传输层
# ==============================================================================
CONFIG_BT=y
CONFIG_BT_PERIPHERAL=y
CONFIG_BT_DEVICE_NAME="NRF Keyboard"
CONFIG_BT_DEVICE_APPEARANCE=961

CONFIG_BT_HIDS=y
CONFIG_BT_BAS=y
CONFIG_BT_DIS=y

CONFIG_BT_SMP=y
CONFIG_BT_BONDABLE=y
CONFIG_BT_MAX_PAIRED=1
CONFIG_BT_ID_MAX=2

CONFIG_BT_SETTINGS=y
CONFIG_SETTINGS=y
CONFIG_FLASH=y
CONFIG_FLASH_MAP=y
CONFIG_NVS=y
CONFIG_SETTINGS_NVS=y

CONFIG_BT_GATT_CLIENT=n
```

说明：

- `CONFIG_BT_SETTINGS` + `CONFIG_SETTINGS` 用于恢复配对信息。
- `CONFIG_NVS` + `CONFIG_SETTINGS_NVS` 使用 flash 分区作为 settings 后端。
- `CONFIG_BT_MAX_PAIRED=1` 对应单主机键盘。后续多主机场景可扩展为 3。
- `CONFIG_BT_GATT_CLIENT=n` 保持外设侧 HID 键盘职责，不启用不需要的 GATT Client。

## 4. 架构设计

### 4.1 模块职责

`ble_transport.c` 负责：

- 初始化 BLE Host。
- 初始化 HID Service、Battery Service、Device Information Service。
- 在 BLE 档位启动可连接广播。
- 在非 BLE 档位停止广播，并根据策略断开现有 BLE 连接。
- 监听现有 HID 报告事件，并通过 HIDS 输入报告发送给已连接主机。
- 处理协议切换请求，并提交 `set_protocol_event`。
- 加载 settings，恢复 bonding 信息。

`ble_transport.c` 不负责：

- 不扫描键盘矩阵。
- 不维护按键状态机。
- 不直接读取或修改 `keyboard_core` 内部上下文。
- 不负责 USB 发送路径。

### 4.2 数据流

```text
mode_switch.c
  └─ mode_event(mode=BLE)
       └─ ble_transport: 启动广播 / 恢复连接

keyboard_core.c
  ├─ hid_kbd_report_event
  │    └─ ble_transport: 转换为 HIDS Input Report
  └─ hid_consumer_report_event
       └─ ble_transport: 转换为 Consumer Control Input Report

BLE Host
  └─ HIDS Set Protocol
       └─ ble_transport: 提交 set_protocol_event
```

### 4.3 状态机

```text
BLE_OFF
  └─ 收到 mode_event(BLE) → BLE_ADVERTISING

BLE_ADVERTISING
  ├─ 主机连接成功 → BLE_CONNECTED
  └─ 收到非 BLE 档位 → BLE_OFF

BLE_CONNECTED
  ├─ 连接断开且仍在 BLE 档位 → BLE_ADVERTISING
  ├─ 收到非 BLE 档位 → BLE_OFF
  └─ 收到 HID 报告 → 发送 HIDS Input Report
```

### 4.4 持久化策略

初始化顺序：

1. `bt_enable()` 完成 BLE Host 初始化。
2. 调用 `settings_load()` 加载 NVS 中的 bonding / CCCD 数据。
3. 根据当前档位决定是否启动广播。

保存内容：

- 主机 bonding key。
- CCCD 订阅状态。
- HID Service 相关持久化状态。

不保存内容：

- 当前按键状态。
- 当前连接句柄。
- 运行期临时协议状态。

## 5. 事件规划

新增事件：

- `ble_state_event`

事件头文件：

- `inc/events/ble_state_event.h`

事件注册源文件：

- `src/events/ble_state_event.c`

结构体字段：

```c
struct ble_state_event {
    struct app_event_header header;
    uint8_t state;
    bool connected;
    bool bonded;
    int err;
};
```

日志要求：

```text
e:ble_state state=1 connected=0 bonded=1 err=0
e:ble_state state=2 connected=1 bonded=1 err=0
e:ble_state state=0 connected=0 bonded=1 err=0
```

`APP_EVENT_TYPE_DEFINE` 第四参数固定使用 `0`，并必须提供 `log_event` 回调，禁止 `NULL`。

## 6. 传输策略

### 6.1 键盘报告

BLE HID 通道使用和 USB 层一致的源事件，但 BLE 层独立选择发送路径：

- Boot Protocol：发送 8B Boot Keyboard Input Report。
- Report Protocol：优先发送 NKRO 报告；如主机不订阅 NKRO，则降级发送 6KRO 报告。

为了避免事件回调阻塞，BLE 发送路径采用工作队列隔离：

- CAF 回调只复制报告到模块本地缓冲。
- 使用 `k_work_submit()` 投递到 BLE TX 工作项。
- BLE TX 工作项中调用 HIDS 输入报告发送接口。

### 6.2 消费者报告

消费者报告固定转换为 Consumer Control Input Report：

- Report ID 与 USB 侧保持一致：`2`。
- 最多 3 个 16-bit Usage。
- `count=0` 必须发送全释放报告，避免主机侧按键粘连。

## 7. 文件变更计划

新增文档：

```text
external/docs/ble_transport_design.md
```

新增源码：

```text
inc/ble_transport.h
src/ble_transport.c
inc/events/ble_state_event.h
src/events/ble_state_event.c
```

修改文件：

```text
src/main.c
prj.conf
CMakeLists.txt
boards/arm/key_board/key_board.overlay
```

`CMakeLists.txt` 需要新增：

```cmake
target_sources(app PRIVATE src/ble_transport.c src/events/ble_state_event.c)
```

## 8. 预期日志

```text
<inf> ble_transport: BLE 传输层初始化完成，HIDS/BAS/DIS 已注册
<inf> ble_transport: settings 已加载，bonded=1
e:ble_state state=1 connected=0 bonded=1 err=0
<inf> ble_transport: BLE 档位进入，开始可连接广播
e:ble_state state=2 connected=1 bonded=1 err=0
<inf> ble_transport: BLE 已连接
<dbg> ble_transport: TX kbd[Boot 8B]
<dbg> ble_transport: TX consumer[7B]
<inf> ble_transport: BLE 档位退出，停止广播并断开连接
e:ble_state state=0 connected=0 bonded=1 err=0
```

## 9. 风险与边界

- 当前 USB HID 事件注册文件的 `log_event` 参数仍为 `NULL`，后续建议单独补齐日志，以满足事件总线可观测要求。
- BLE 与 USB 同时订阅 HID 事件时，必须由档位状态决定是否实际发送，避免非当前传输通道误发。
- flash 分区一旦写入 bonding 信息，后续改动分区布局可能需要用户手动清理存储区。
- 本方案暂不实现多主机切换；后续可基于 `CONFIG_BT_MAX_PAIRED=3` 和主机槽位事件扩展。

## 10. 用户主动清理配对后的 Identity 轮换策略

长按触发 `BLE_CONTROL_CMD_CLEAR_BONDS` 时，不能只清理设备端 bonding 数据。
如果设备端仍使用原 BLE identity 地址，Windows、手机等主机会继续把该外设识别为旧设备，并尝试使用主机侧保存的旧密钥重连。
此时设备端已无对应密钥，会进入 `BT_SECURITY_ERR_PIN_OR_KEY_MISSING`，表现为 `err=2 bonded=0` 的循环断连。

Zephyr 默认 identity `BT_ID_DEFAULT` 不能在运行期通过 `bt_id_reset()` 重置。
因此本模块采用官方示例一致的策略：创建或轮换非默认 identity，并使用该 identity 进行广播。
对应 Kconfig 需要设置 `CONFIG_BT_ID_MAX=2`。

启动加载 settings 后必须先审计 identity：

1. 若已存在非默认 identity，则优先使用非默认 identity。
2. 若仅存在默认 identity 且本机无 bond，则立即创建非默认 identity。
3. 首次 BLE 广播必须使用当前 active identity，避免主机继续用默认 identity 的旧密钥重连。

因此，用户主动清理配对必须执行以下闭环：

1. 停止广播。
2. 若当前已连接，先断开连接。
3. 清理设备端全部 bond。
4. 若当前使用默认 identity，则创建新的非默认 identity。
5. 若当前使用非默认 identity，则轮换该 identity。
6. 使用当前 active identity 重新广播，使主机按新设备重新发起配对。
7. 清空本地修复退避状态，并重新开始可连接广播。

该策略只用于 `USER_CLEAR`，不用于普通安全错误自动修复。
普通 `LOCAL_KEY_MISSING` 仍只清理本机 bond，避免无故改变身份地址。

预期日志：

```text
<inf> ble_transport: 蓝牙身份选择 id=0 数量=1
<inf> ble_transport: 首次配对身份已创建 id=1
<inf> ble_transport: 蓝牙就绪 id=1 bond=0 HIDS=1
<wrn> ble_transport: 用户清理蓝牙配对
<inf> ble_transport: 蓝牙已断开 reason=22
<inf> ble_transport: 蓝牙 bond 已删除 bond=0
<wrn> ble_transport: 蓝牙配对已清理 id=0 原因=USER_CLEAR bond=0
<inf> ble_transport: 蓝牙身份已创建 id=1
<inf> ble_transport: 蓝牙广播 id=1 bond=0 修复=0 旧主机=0 延时=500ms
```

## 11. BLE 连接态防休眠与错误状态复位

BLE 已连接期间必须阻止 CAF Power Manager 进入板级 power down。
本模块复用 Nordic CAF 官方 `keep_alive_event` 机制，不新增业务线程，不轮询外设状态。

实现策略：

1. BLE 连接成功后立即提交一次 `keep_alive_event`。
2. BLE 处于连接态且仍在 BLE 档位时，使用 `k_work_delayable` 每 30 秒提交一次 `keep_alive_event`。
3. BLE 断开或离开 BLE 档位时取消该延迟工作。
4. 配对成功、加密成功、新 identity 创建成功、identity 轮换成功、非密钥类安全失败时，统一清理本地修复状态：
   - `bond_repair_required = false`
   - `stale_host_key_backoff = false`
   - `stale_host_key_failures = 0`
   - 广播重启延时恢复为 500ms

预期日志：

```text
<inf> ble_transport: 蓝牙已连接 id=1 bond=0
<inf> app_event_manager: e: keep_alive_event
<inf> ble_transport: 蓝牙加密完成 level=2 bond=1
```
## 12. BLE 调试日志观测修订

本次日志增强只用于 BLE 传输层观测，不改变档位切换、CAF 事件流和 HID 发送架构。新增日志必须遵守以下规则：

1. `CONNECTED id=%u` 必须通过 `bt_conn_get_info()` 读取真实连接 identity，禁止直接打印模块内的 `active_identity`。
2. `ADV_START` 必须同时打印 `active_id` 与 `adv_param.id`，用于判断是否出现“已选择新 identity，但广播仍使用默认 identity”的错误。
3. `HID_DROP` 使用枚举原因并做状态限频，只在原因变化或关键状态复位后打印，避免按键高频事件淹没连接与安全日志。
4. `HID_TX` 成功路径使用 `LOG_DBG`，失败路径使用 `LOG_WRN`，避免正常输入时刷屏。
5. 用户清理配对、断连、安全成功、进出 BLE 档位、CCCD 变化时，必须复位 HID drop 限频状态，确保下一轮异常仍能被观察到。
6. 当前实现尚未真实接入 `bt_id_create()` / `bt_id_reset()`，因此禁止伪造 `ID_CREATE` / `ID_RESET` 成功日志。

标准日志样例：

```text
<inf> ble_transport: ADV_START active_id=1 adv_param.id=1 bonded=0 repair=0 user_clear=0
<err> ble_transport: ADV_START err=-12 active_id=1 adv_param.id=1
<inf> ble_transport: ADV_STOP err=0 user_clear=1 repair=0
<inf> ble_transport: CONNECTED id=1
<inf> ble_transport: SECURITY level=2 err=0 bonded=1
<inf> ble_transport: DISCONNECTED reason=19 user_clear=0 repair=0
<inf> ble_transport: UNPAIR id=0 err=0
<inf> ble_transport: REPAIR_STATE clear reason=SECURITY_OK
<wrn> ble_transport: HID_DROP reason=no_cccd
<dbg> ble_transport: HID_TX err=0 report=kbd
<wrn> ble_transport: HID_TX err=-12 report=consumer
```
