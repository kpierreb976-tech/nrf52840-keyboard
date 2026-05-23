# USB 传输层设计方案 (usb_transport.c)

## 1. 需求概述

实现基于 Zephyr USB Next Stack 的高性能传输层，作为 CAF 事件总线的消费者，将 `hid_kbd_report_event` 和 `hid_consumer_report_event` 转换为 USB HID 线缆报文发送至主机。

## 2. 设备树审计

**结论通过**：`key_board.overlay` 已使能 `&usbd { status = "okay"; }`，nRF52840 USBD 硬件可用。

## 3. Kconfig 当前配置

当前 `prj.conf` 已启用并需要保持以下配置项：

```
# --- USB Next Stack (新一代 USB 堆栈) ---
CONFIG_USB_DEVICE_STACK_NEXT=y
CONFIG_USBD_HID_SUPPORT=y

# --- Ring Buffer (环形缓冲区，生产者-消费者桥梁) ---
CONFIG_RING_BUFFER=y
```

当前工程使用 USB Device Stack Next，未启用旧版 `CONFIG_USB_DEVICE_STACK`。USB HID IN 缓冲和 UDC 缓冲池通过 `CONFIG_USBD_HID_IN_BUF_COUNT=8`、`CONFIG_UDC_BUF_COUNT=32`、`CONFIG_UDC_BUF_POOL_SIZE=8192` 放大，以降低高峰按键和 EP0 控制传输时的 net_buf 分配失败风险。

## 4. 架构设计

### 4.1 数据流

```
keyboard_core.c                        usb_transport.c
┌──────────────┐    CAF事件总线         ┌──────────────────────────┐
│ APP_EVENT_   │ ───hid_kbd_report───▶  │ handle_kbd_report()      │
│ SUBMIT()     │                        │   ├─ 检查 usb_configured  │
│              │ ───hid_consumer───▶    │   ├─ 构建线缆格式        │
└──────────────┘                        │   ├─ ring_buf_put()      │
                                        │   ├─ k_sem_give()        │
                                        │   └─ return (非阻塞)     │
                                        │                          │
                                        │ usb_tx_thread (低优先级) │
                                        │   ├─ k_sem_take() 挂起   │
                                        │   ├─ ring_buf_get() 取出 │
                                        │   └─ hid_int_ep_write()  │
                                        └──────────┬───────────────┘
                                                   ▼
                                              USB IN Endpoint
```

### 4.2 生产者-消费者模型

```
[Event Callback] ──push──▶ [Ring Buffer (2048B)] ──pop──▶ [TX Thread (COOP7)]
      ↑                        ↑     ↑                        │
  事件驱动/非阻塞           34B/槽  ~60槽                  阻塞式发送
  返回 false (放行给BLE)                                    hid_int_ep_write
```

### 4.3 线缆格式定义

**键盘报告 — Boot 模式 (8B，无 RID)**：
`[modifier][reserved 0x00][key0..key5]` — 严格 8 字节纯数据，BIOS/Boot 兼容。

**键盘报告 — 6KRO 模式 (9B)**：
`[RID=1][modifier][reserved 0x00][key0..key5]` — 前置 1 字节 Report ID，6 键槽。

**键盘报告 — NKRO 模式 (33B)**：
`[RID=3][modifier][bitmap 31B]` — 前置 1 字节 Report ID，全键位图无冲。

**消费者报告 (7B)**：
`[RID=2][usage0_lo][usage0_hi][usage1_lo][usage1_hi][usage2_lo][usage2_hi]`
空闲槽位填 0，count=0 时发送全零。

### 4.4 环形缓冲区条目格式

```c
#define USB_TX_ITEM_SIZE 35  // 1B type + 1B format + 1B len + 32B data_max

struct usb_tx_item {
    uint8_t type;   // 0=键盘, 1=消费者
    uint8_t format; // 原始事件格式: 0=Boot 8B, 1=NKRO 32B
    uint8_t len;    // 载荷有效字节数
    uint8_t data[32]; // 载荷（原始 raw_report，不做预格式化）
};
```

> **队列解耦原则**：TX 队列只存储原始按键状态，格式化动作推迟至发送前最后一刻，由 `current_proto` / `nkro_enabled` 动态决定。彻底规避协议切换时队列残留旧格式报文的问题。

## 5. 模块分解

### 5.1 HID 报告描述符

- **Report ID 1 (键盘 6KRO)**：9 字节线缆报文，键盘 Array 字段 `LOGICAL_MAXIMUM`/`USAGE_MAXIMUM` 严格限制为 101 (0x65)，确保 BIOS/UEFI 兼容性（详见第 9 节）
- **Report ID 3 (键盘 NKRO)**：33 字节线缆报文，1B 修饰键 + 31B 全键位图 (248bit)，覆盖 Usage 0x00~0xF7，全键无冲（详见第 10 节）
- **Report ID 2 (消费者)**：3 组 16-bit Usage 数组，共计 7 字节

### 5.2 HID 初始化与 USB 状态跟踪

- 定义 HID Report Descriptor 和 `USBD_CONFIGURATION_DEFINE`
- 注册 USB 状态回调，跟踪 `usb_configured` 状态
- 全局状态变量：`current_proto`（0=Boot, 1=Report）和 `nkro_enabled`（true/false）
- 实现 `set_protocol` 回调，同步更新全局状态并提交 `set_protocol_event` 通知 keyboard_core

### 5.3 事件监听回调（在 CAF 事件管理器上下文中执行）

- `handle_kbd_report()`：取 raw_report 原始数据 + format 标记 → ring_buf（队列解耦：不做预格式化）
- `handle_consumer_report()`：构建 7B 线缆报文 → ring_buf → k_sem_give
- 两个回调均检查 `usb_configured`，USB 未就绪返回 false（事件放行给 BLE 层）

### 5.4 传输线程 — 三路分流（current_proto 第一优先级）

```
current_proto == BOOT (0)
  └─ 无条件 8B 纯数据（无 RID），不管 format 是什么

current_proto == REPORT (1)
  ├─ format==NKRO(1) && nkro_enabled → 33B RID=3 NKRO
  └─ 否则                            → 9B  RID=1 6KRO
```

- `K_THREAD_DEFINE` 创建，优先级 `K_PRIO_COOP(7)`（协作式低优先级）
- 无数据时 `k_sem_take` 进入 Blocked 状态（零功耗）
- 被唤醒后 `while` 循环排空 ring_buf，逐条按分流矩阵构建线缆报文并调用 `hid_device_submit_report`

## 6. CMakeLists.txt 修改

```cmake
target_sources(app PRIVATE src/usb_transport.c)
```

## 7. main.c 修改

在 `main()` 中增加 `usb_transport_init()` 调用。

## 8. 预期日志

```
<inf> usb_transport: USB 传输层初始化 — RingBuf=2048B 线程优先级=COOP(7)
<inf> usb_transport: USB HID 已注册 — 键盘 RID=1(6KRO 9B) + RID=3(NKRO 33B), 消费者 RID=2(7B)
<inf> usb_transport: USB 已配置，传输就绪
<inf> usb_transport: 主机请求协议切换: Report → NKRO 通道已启用 (33B)
<dbg> usb_transport: TX kbd[NKRO 33B]: 03 00 00 00 00 04 ...
<inf> usb_transport: 主机请求协议切换: Boot → 降级标准 8B (无 RID)
<dbg> usb_transport: TX kbd[Boot 8B]: 00 00 04 05 00 00 00 00
<dbg> usb_transport: TX consumer[7B]: 02 E9 00 00 00 00 00 00
<wrn> usb_transport: ring_buf 溢出 — 丢弃报告
```

## 10. Hybrid 动态切换模式 (6KRO ↔ NKRO)

### 设计意图

固件同时注册 6KRO (RID=1) 和 NKRO (RID=3) 两个 HID Report ID，根据主机 `Set_Protocol` 请求和全局状态变量 `current_proto` / `nkro_enabled` 动态选择发送通道。

### 分流矩阵

| current_proto | format | nkro_enabled | 发送行为 |
|:--|:--|:--|:--|
| BOOT (0) | 任意 | 任意 | **8B 纯数据，无 RID** |
| REPORT (1) | NKRO (1) | true | 33B RID=3 NKRO |
| REPORT (1) | NKRO (1) | false | 9B RID=1 6KRO (bitmap→6KRO 转换) |
| REPORT (1) | Boot (0) | 任意 | 9B RID=1 6KRO |

### Boot 模式 Report ID 剥离

HID 规范严格限制：当主机通过 `Set_Protocol(0)` 切换到 Boot Protocol 时，主机驱动写死只接收标准 **8 字节** 报文 `[Modifier][Reserved][6×Keycode]`。若此时发送带 Report ID 的 9 字节报文，BIOS 驱动会因数据偏移和长度不匹配而丢弃报文，导致键盘在 BIOS 中完全失效。

因此 `current_proto == BOOT` 时，**无条件剥离 Report ID**，仅发送 8 字节纯数据，无论上游事件格式如何。

### 队列解耦

TX 队列中的 `usb_tx_item` 只存储原始 `raw_report` + `format` 标记，不做预格式化。所有格式化动作（RID 前缀、NKRO↔6KRO 转换）推迟至 TX 线程 `k_sem_take` 唤醒后、`hid_device_submit_report` 调用前执行。这确保了主机协议切换时，队列中残留的旧条目也能以最新协议状态正确发送。

## 9. BIOS/UEFI 兼容性修复：键盘 Array 字段范围限制

### 问题现象

键盘在 Windows/Linux 下所有按键正常，但在 BIOS/UEFI 设置界面（如主板 CMOS 设置、Grub 引导菜单）中，部分按键（如数字键 `6`、部分功能键）无响应。

### 根因分析

USB HID 规范中键盘用途表（Keyboard Usage Page 0x07）的标准键码范围为 `0x04`（Keyboard a/A）至 `0x65`（Keyboard Application，即菜单键）。但许多客制化键盘固件将 HID 报告描述符中 Array 字段的 `LOGICAL_MAXIMUM` 和 `USAGE_MAXIMUM` 声明为 **255 (0xFF)**，而非 **101 (0x65)**。

部分 BIOS/UEFI 的 USB HID 驱动解析器较为简陋，当遇到超出标准范围的 Array 声明时，会错误地分配内部索引表或对中间段的键码做越界判断，导致特定扫描码被静默丢弃。这是 QMK/ZMK 客制化键盘社区中已知的经典兼容性问题。

### 修复方案

将键盘 Array 字段的 `LOGICAL_MAXIMUM` 和 `USAGE_MAXIMUM` 从 `0xFF` 严格限制为 `0x65`，精确匹配 USB HID 键盘用途表的合法上限：

```c
/* 修复前（BIOS 兼容性问题） */
0x25, 0xFF, //   LOGICAL_MAXIMUM (255)
0x29, 0xFF, //   USAGE_MAXIMUM (Keyboard Application)

/* 修复后 */
0x25, 0x65, //   LOGICAL_MAXIMUM (101)
0x29, 0x65, //   USAGE_MAXIMUM (Keyboard Application)
```

### 验证结果

修复后键盘在以下 BIOS/UEFI 环境中所有标准按键均可正常输入：
- AMI Aptio BIOS（主流主板）
- InsydeH2O UEFI（笔记本常见）
- Phoenix SecureCore（部分品牌机）
- Grub2 / systemd-boot 引导菜单
