# 编码器驱动方案 — EC11 旋转编码器

## 1. 硬件概览

| 项目 | 详情 |
|------|------|
| 型号 | EC11 旋转编码器（带按键） |
| A 相 | P0.10（QDEC_A） |
| B 相 | P1.06（QDEC_B） |
| 按键 | COL3 / ROW0（矩阵扫描） |
| 公共端 C | GND |
| 物理卡点 | 20 个/圈 |
| QDEC steps | 80 steps/圈（4 steps = 1 detent） |

### 1.1 硬件踩坑：浮空引脚

EC11 C 脚接 GND，旋转时 A/B 交替接触 C 被拉到低电平，但断开时引脚浮空。nRF52840 QDEC 外设内置数字滤波器，浮空导致的电平不确定会被识别为毛刺并全部丢弃，ACC 寄存器始终为 0。

**修复**：在 `qdec_default` pinctrl 中添加 `bias-pull-up`，让 A/B 在未接触 GND 时保持高电平。

```dts
qdec_default: qdec_default {
    group1 {
        psels = <NRF_PSEL(QDEC_A, 0, 10)>,
                <NRF_PSEL(QDEC_B, 1, 6)>;
        bias-pull-up;  /* 关键：防止引脚浮空 */
    };
};
```

## 2. 软件分层架构

```
┌─────────────────────────────────────────────────────────┐
│                    USB/BLE HID 发送层                       │
│   key_id: 0x180 → MUTE (0x00E2)                         │
│   key_id: 0x00E9 → Volume Up                            │
│   key_id: 0x00EA → Volume Down                          │
└──────────────────────────┬──────────────────────────────┘
                           │ button_event
┌──────────────────────────▼──────────────────────────────┐
│                   CAF 事件总线                            │
│                                                         │
│   ┌─────────────────┐    ┌──────────────────────┐       │
│   │ encoder_event   │    │ button_event         │       │
│   │ dir: ±1         │    │ key_id: uint16_t     │       │
│   │ steps: detents  │    │ pressed: bool        │       │
│   └────────┬────────┘    └──────────▲───────────┘       │
│            │                        │                    │
│   encoder.c 产出          encoder_mapper.c 桥接          │
│   (QDEC 硬件驱动)         (encoder → button)             │
└──────────────────────────────────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────┐
│                     硬件抽象层                             │
│   QDEC 外设 (nRF52840)  +  键盘矩阵扫描 (CAF Buttons)     │
└──────────────────────────────────────────────────────────┘
```

## 3. 模块职责

| 文件 | 职责 |
|------|------|
| `src/encoder.c` | QDEC 硬件驱动：初始化、触发注册、steps→detents 转换 |
| `src/events/encoder_event.c` | 定义 encoder_event 事件类型及日志格式 |
| `src/events/encoder_mapper.c` | 旋转→音量键桥接：监听 encoder_event，投递 button_event |
| `boards/.../key_board-pinctrl.dtsi` | QDEC 引脚配置（含 bias-pull-up 修复） |
| `key_board.overlay` | QDEC 设备树节点（led-pre=4, steps=80） |

## 4. 按键映射（按下静音）

### 4.1 当前策略：无全局 Keymap，直接复用矩阵 key_id

项目当前不建立全局键盘矩阵→HID 键码映射表。编码器按键（EC11 的 SW 脚）接入键盘矩阵的 **COL3 / ROW0**，由 CAF Buttons 模块扫描并直接产生 `button_event`，携带 `key_id = 0x180`。

```
EC11 按下 → 矩阵 COL3/ROW0 导通 → CAF Buttons 扫描
         → button_event { key_id=0x180, pressed=true/false }
```

### 4.2 静音翻译：在 HID 发送端完成

`0x180` 到 HID Consumer Control 静音码 `0x00E2` 的映射不在编码器事件层做，而是在当前 `keymap.c` 中统一声明，由 `keyboard_core` 生成 `hid_consumer_report_event`，再分别交给 USB/BLE 传输层发送。

| 来源 | key_id | HID Usage | 含义 |
|------|--------|-----------|------|
| 编码器按下 | `0x180` | `0x00E2` | Mute（静音） |

## 5. 旋转映射（音量加/减）

### 5.1 encoder_mapper.c 事件桥接逻辑

```
encoder_event (dir=±1, steps=N)
        │
        ▼
encoder_mapper 监听器
        │
        ├── dir > 0 → key_id = 0x00E9 (Volume Up)
        ├── dir < 0 → key_id = 0x00EA (Volume Down)
        │
        └── for i in 0..steps-1:
                APP_EVENT_SUBMIT(button_event{pressed=true,  key_id})
                APP_EVENT_SUBMIT(button_event{pressed=false, key_id})
```

### 5.2 成对提交的原因

USB HID 协议要求每个按键必须有对应的按下（pressed）和释放（released）事件。如果只发按下不发释放，HID 主机端会认为该键一直被按住（"卡键"），导致音量持续变化。

### 5.3 键码约定

| 旋转方向 | HID Consumer Code | 宏定义 | 含义 |
|----------|-------------------|--------|------|
| 顺时针 (CW) | `0x00E9` | `HID_CONSUMER_VOL_UP` | 音量加 |
| 逆时针 (CCW) | `0x00EA` | `HID_CONSUMER_VOL_DOWN` | 音量减 |

## 6. 数据流举例

用户顺时针旋转 2 个卡点：

```
QDEC 硬件 ACC 累加 8 steps → 触发 DATA_READY 中断
→ encoder_work_handler
  → steps_raw = 8, detents = 2
  → encoder_event { dir=1, steps=2 }
  → APP_EVENT_SUBMIT

→ encoder_mapper 收到 encoder_event
  → loop x2:
      button_event { key_id=0x00E9, pressed=true  }
      button_event { key_id=0x00E9, pressed=false }
```

用户按下旋钮：

```
矩阵 COL3/ROW0 导通 → CAF Buttons 扫描
→ button_event { key_id=0x180, pressed=true  }
→ button_event { key_id=0x180, pressed=false }
```

## 7. Kconfig 依赖

```ini
CONFIG_QDEC_NRFX=y        # nRF QDEC 外设驱动
CONFIG_SENSOR=y            # Zephyr 传感器框架
CONFIG_APP_EVENT_MANAGER=y # CAF 事件总线
CONFIG_CAF_BUTTONS=y       # CAF 按键子系统（提供 button_event）
```

## 8. 版本记录

| 日期 | 变更 |
|------|------|
| 2026-05-20 | 初始版本：QDEC 硬件驱动 + encoder_mapper 桥接 + pinctrl bias-pull-up 修复 |
