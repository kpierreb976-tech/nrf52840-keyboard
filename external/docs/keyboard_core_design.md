# 键盘核心模块设计方案 — keyboard_core

## 1. 模块定位

`keyboard_core` 是按键事件到 HID 报告的桥梁。订阅 CAF 事件总线上的 `button_event`，维护按键状态并生成传输无关的线缆报文，通过 `hid_kbd_report_event` 和 `hid_consumer_report_event` 广播，供 USB/BLE 承载层零拷贝消费。

```
button_event ──→ keyboard_core ──→ hid_kbd_report_event (键盘)
                   │              → hid_consumer_report_event (媒体键)
                   │
                   ├── 32B bitmap 维护全键状态
                   ├── usages[] 维护消费者键状态
                   ├── Boot Protocol (8B, 默认)
                   └── NKRO Report Protocol (32B, 预留)
```

## 2. 数据流

```
┌──────────────────┐
│   CAF Buttons    │──→ button_event (矩阵扫描)
│   encoder_mapper │──→ button_event (旋转→音量键)
└────────┬─────────┘
         │
   ┌─────▼──────────────────────────────────────────┐
   │              keyboard_core                      │
   │                                                 │
   │  栈上组装: temp_report[32]                       │
   │       ↓                                        │
   │  差分检测: memcmp(temp, prev)                    │
   │       ↓ (有变化才分配)                           │
   │  事件分配: new_hid_xxx_report_event()            │
   │       ↓                                        │
   │  APP_EVENT_SUBMIT()                             │
   └─────────────────────┬──────────────────────────┘
                         │
          ┌──────────────┴──────────────┐
          ▼                              ▼
   hid_kbd_report_event          hid_consumer_report_event
   (传输层零拷贝发送)             (传输层需环形缓冲区排队)
```

## 3. 事件结构

### 3.1 hid_kbd_report_event

```c
struct hid_kbd_report_event {
    struct app_event_header header;
    uint8_t  format;      // 0=Boot, 1=NKRO
    uint8_t  report_id;   // HID Report ID
    uint8_t  len;         // 有效字节数 (Boot=8, NKRO=32)
    uint8_t  raw_report[32]; // 线缆报文，严格对齐 HID 描述符
};
```

**Boot 模式线缆布局** (len=8):
```
[0] = modifier_byte
[1] = 0x00 (保留)
[2..7] = 最多 6 个非修饰键码，不足填 0x00
```

**NKRO 模式线缆布局** (len=32，预留):
```
[0] = modifier_byte
[1..31] = 31 字节 bitmap (覆盖 Usage 0x00~0xF7)
[29] 强制清 0 (消除修饰键幽灵位)
```

### 3.2 hid_consumer_report_event

```c
struct hid_consumer_report_event {
    struct app_event_header header;
    uint8_t  report_id;
    uint16_t usages[3];  // 当前所有按下的消费者键
    uint8_t  count;      // 0 = 全部释放
};
```

- 按键释放时 count=0 的清理报告必须发出
- 传输层约束：按 Report Descriptor 定义长度补齐后发送

### 3.3 set_protocol_event (预留，暂无生产者)

```c
struct set_protocol_event {
    struct app_event_header header;
    uint8_t protocol;  // 0=Boot, 1=Report(NKRO)
};
```

## 4. 内部实现要点

### 4.1 栈上差分检测

先在线程栈上组装 `temp_report[32]`，与 `prev_report[]` 比较。有变化才调用 `new_xxx_event()` 分配堆内存。零无效堆分配。

### 4.2 Boot 报告打包

```
1. modifier = bitmap[28]                     → raw_report[0]
2. raw_report[1] = 0x00
3. 遍历 bitmap[0..31]:
    跳过 byte==28    (修饰键，归 modifier byte)
    跳过 byte==0 bit0 (0x00 Reserved)
    跳过 byte==0 bit1 (0x01 ErrorRollOver)
    提取前 6 个置位 bit → raw_report[2..7]
4. 不足 6 个填 0x00
```

### 4.3 NKRO 报告打包 (预留)

```
raw_report[0]  = bitmap[28]    (modifier byte)
raw_report[1..31] = bitmap[0..30]
raw_report[29]    = 0x00       (清除修饰键在 bitmap 中的幽灵位)
```

### 4.4 消费者键处理

```
收到 button_event (type=消费者):
  pressed=true  → usages[active++] = usage
  pressed=false → usages[] 中移除该 usage
  发布 hid_consumer_report_event (count 可为 0)
```

### 4.5 set_protocol 处理

- 默认 format=0 (Boot)
- 收到 set_protocol_event 后切换 format
- NKRO 报表体暂留空骨架

## 5. 键位布局与 Keymap

```
NUM     /       *       -       ◎旋钮
 7      8       9       + (2U高)
 4      5       6       +
 1      2       3     Enter (2U高)
 0      .     Enter
```

### 5.1 key_id 编码公式

CAF Buttons: `KEY_ID(col_index, row_index) = (col << 7) | row`

### 5.2 完整映射表

| 按键 | 矩阵位置 | key_id | HID Usage | 类型 |
|------|----------|--------|-----------|------|
| NUM | R1, C0 | 0x0001 | 0x53 (NumLk) | 键盘 |
| / | R1, C1 | 0x0081 | 0x54 (KP /) | 键盘 |
| * | R1, C2 | 0x0101 | 0x55 (KP *) | 键盘 |
| - | R1, C3 | 0x0181 | 0x56 (KP -) | 键盘 |
| 7 | R2, C0 | 0x0002 | 0x5F (KP 7) | 键盘 |
| 8 | R2, C1 | 0x0082 | 0x60 (KP 8) | 键盘 |
| 9 | R2, C2 | 0x0102 | 0x61 (KP 9) | 键盘 |
| + | R3, C3 | 0x0183 | 0x57 (KP +) | 键盘 |
| 4 | R3, C0 | 0x0003 | 0x5C (KP 4) | 键盘 |
| 5 | R3, C1 | 0x0083 | 0x5D (KP 5) | 键盘 |
| 6 | R3, C2 | 0x0103 | 0x5E (KP 6) | 键盘 |
| 1 | R4, C0 | 0x0004 | 0x59 (KP 1) | 键盘 |
| 2 | R4, C1 | 0x0084 | 0x5A (KP 2) | 键盘 |
| 3 | R4, C2 | 0x0104 | 0x5B (KP 3) | 键盘 |
| Enter | R5, C3 | 0x0185 | 0x58 (KP Enter) | 键盘 |
| 0 | R5, C0 | 0x0005 | 0x62 (KP 0) | 键盘 |
| . | R5, C1 | 0x0085 | 0x63 (KP .) | 键盘 |
| EC11 SW | R0, C3 | 0x0180 | 0x00E2 (Mute) | 消费者 |
| — | — | 0x00E9 | 0x00E9 (Vol Up) | 消费者 |
| — | — | 0x00EA | 0x00EA (Vol Down) | 消费者 |

> 编码器虚拟键 (0x00E9/0x00EA) 由 encoder_mapper 投递，非矩阵扫描产生。

## 6. 文件清单

| 文件 | 职责 |
|------|------|
| `inc/keyboard_core.h` | 模块初始化声明 |
| `inc/keymap.h` | keymap_entry 结构体与查表接口 |
| `inc/events/hid_kbd_report_event.h` | 键盘 HID 报告事件 |
| `inc/events/hid_consumer_report_event.h` | 消费者 HID 报告事件 |
| `inc/events/set_protocol_event.h` | set_protocol 事件 (预留) |
| `src/keyboard_core.c` | 核心逻辑 |
| `src/keymap.c` | keymap 表与查表实现 |
| `src/events/hid_kbd_report_event.c` | 事件类型注册 |
| `src/events/hid_consumer_report_event.c` | 事件类型注册 |
| `src/events/set_protocol_event.c` | 事件类型注册 (预留) |

## 7. Kconfig 依赖

无需新增。keyboard_core 是纯事件消费者，不依赖新硬件外设。现有配置已满足：
- `CONFIG_APP_EVENT_MANAGER=y`
- `CONFIG_CAF_BUTTONS=y`

## 8. 版本记录

| 日期 | 变更 |
|------|------|
| 2026-05-20 | 初始版本：双报告事件 + Boot/NKRO 双协议 + 栈上差分优化 |
