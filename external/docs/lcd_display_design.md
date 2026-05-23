# LCD 动态显示设计方案 — lcd_display

## 1. 设计意图

当前 `lcd_display.c` 只在上电时渲染一次静态状态页。本次改造分三阶段：

- **v1.0（已实施）**：事件驱动动态刷新，LINK / MODE / BAT 三张信息卡片实时响应系统状态变化
- **v2.0（已实施）**：集成 LVGL v9.3.0，用 widget 替代手绘代码，以豆沙绿 `#C7EDCC` 为主题色
- **v2.1（本次）**：Zephyr ST7789V display driver 接管硬件初始化，LVGL 走 Zephyr auto_init，`lcd_display.c` 只保留 UI 逻辑和 CAF 事件响应

**v2.1 核心变更**：从 "手动控制一切" 变为 "Zephyr 管理硬件，我们管理 UI"。

| 层面 | v2.0 | v2.1 |
|------|------|------|
| 面板初始化 | 手动 `lcd_cmd_sequence()` 发 ST7789 命令 | Zephyr `sitronix,st7789v` 驱动 |
| SPI 传输 | 手动 `lcd_spi_write()` / `lcd_write_cmd()` | Zephyr `mipi-dbi-spi` 传输层 |
| LVGL 初始化 | 手动 `lv_init()` + `lv_display_create()` | `CONFIG_LV_Z_AUTO_INIT=y` 自动 |
| flush_cb | 自定义 `lcd_flush_cb()` → 手动 SPI | Zephyr `lvgl.c` 内置 → `display_write()` |
| LVGL 心跳 | 自定义 5ms delayable work | `CONFIG_LV_Z_RUN_LVGL_ON_WORKQUEUE=y` |
| 线程安全 | 全部串行在 `lcd_work_q`（零锁） | `lvgl_lock()` / `lvgl_unlock()` 互斥锁 |

## 2. 事件审计

三个所需 CAF 事件均已存在并实际投递，无需新增或修改任何其他模块：

| 事件 | 生产者 | 关键字段 |
|------|--------|---------|
| `mode_event` | `mode_switch.c` | `mode`: 1=USB, 2=BLE, 3=2.4G |
| `battery_event` | `power_mgmt.c` | `level`: 0~100, `state`: 0/1/2 |
| `ble_state_event` | `ble_transport.c` (`publish_ble_state`) | `state`: 0~5, `connected`, `bonded`, `err` |

## 3. 线程安全：CAF 线程 → lcd_work_q + LVGL mutex

v2.1 中 LVGL 在 Zephyr 专用工作队列（`lvgl workqueue`）运行 `lv_timer_handler()`，而我们的 cache 写入 handler 在 `lcd_work_q`。两个队列都可以调用 LVGL API，需要 mutex 保护。

```
mode_event handler (CAF 线程)
    │  写入 mode_cache_req.mode
    │  k_work_submit_to_queue(&lcd_work_q, &mode_cache_write_work) ──┐
    │                                                                 │
ble_state_event handler (CAF 线程)                                    │
    │  写入 ble_cache_req.state                                        │
    │  k_work_submit_to_queue(&lcd_work_q, &ble_cache_write_work) ──┐ │
    │                                                                │ │
battery_event handler (CAF 线程)                                     │ │
    │  写入 bat_cache_req.level + state                              │ │
    │  k_work_submit_to_queue(&lcd_work_q, &bat_cache_write_work) ──┐│ │
    │                                                               ││ │
    ▼                                                               ▼▼ ▼
┌───────────────────────────────────────────────────────────────────────┐
│  lcd_work_q (单线程串行消费)                                            │
│                                                                       │
│  mode_cache_write_work_handler ─► 写 cache ─► lvgl_lock()             │
│                                              ─► lv_label_set_text()   │
│                                              ─► lvgl_unlock()         │
│  (ble/bat handler 同理)                                                │
└───────────────────────────────────────────────────────────────────────┘

┌───────────────────────────────────────────────────────────────────────┐
│  lvgl workqueue (Zephyr 自动管理, CONFIG_LV_Z_RUN_LVGL_ON_WORKQUEUE) │
│                                                                       │
│  lv_timer_handler() ─► 处理脏区域 ─► flush_cb ─► display_write()     │
│                        └── 内部持有 lvgl 锁                            │
└───────────────────────────────────────────────────────────────────────┘
```

**覆盖语义**不变：
- 同类型：后续事件覆盖前一个未处理的 req
- 跨类型：三个独立的 req + work，互不踩踏

**互斥锁**：`CONFIG_LV_Z_LVGL_MUTEX=y`（由 `CONFIG_LV_Z_RUN_LVGL_ON_WORKQUEUE` 隐含），提供 `lvgl_lock()` / `lvgl_unlock()`。

## 4. 数据结构

```c
/* ── 状态缓存（仅 lcd_work_q 内读写）── */

struct lcd_state_cache {
    uint8_t mode;           /* 1=USB, 2=BLE, 3=2.4G */
    bool    mode_valid;
    uint8_t ble_state;      /* 0=OFF~5=ERROR */
    bool    link_valid;
    uint8_t battery_level;  /* 0~100 */
    uint8_t battery_state;  /* 0=放电, 1=充电, 2=满电 */
    bool    bat_valid;
};

/* ── 按事件类型拆分的 cache 写入请求 ── */

static struct { uint8_t mode; } mode_cache_req;
static struct k_work mode_cache_write_work;

static struct { uint8_t state; } ble_cache_req;
static struct k_work ble_cache_write_work;

static struct { uint8_t level; uint8_t state; } bat_cache_req;
static struct k_work bat_cache_write_work;

/* ── LVGL 对象指针（仅 lcd_work_q 内访问）── */

static lv_obj_t *link_value_label;
static lv_obj_t *mode_value_label;
static lv_obj_t *bat_value_label;
```

## 5. 卡片渲染映射（不变）

**LINK 卡片**：

| ble_state | 显示 |
|-----------|------|
| 0 (OFF) | `--` |
| 1 (ADVERTISING) | `ADV` |
| 2 (CONNECTED) | `CON` |
| 3 (ENCRYPTED) | `ENC` |
| 4 (READY) | `BLE` |
| 5 (ERROR) | `ERR` |

**MODE 卡片**：

| mode | 显示 |
|------|------|
| 1 | `USB` |
| 2 | `BLE` |
| 3 | `2G4` |

BAT 卡片：`level` + `%`。HID 卡片：`NKRO`（静态）。

## 6. 渲染调度（v2.1）

- **事件驱动**：cache_write_work_handler → 写 cache → `lvgl_lock()` → `lv_label_set_text()` → `lvgl_unlock()`。LVGL 标记脏区域后在下一个 `lv_timer_handler()` 自动刷屏。
- **LVGL 心跳**：由 `CONFIG_LV_Z_RUN_LVGL_ON_WORKQUEUE=y` 自动管理，我们无需手动调度。`lv_timer_handler()` 在 Zephyr 专用 workqueue 以约 5ms 间隔运行。

## 7. 设备树与驱动架构

### 7.1 Devicetree（`key_board.overlay`）

```
zephyr,mipi-dbi-spi (mipi_dbi)
├── spi-dev = <&spi3>        → SPI3 控制器 (SCK=P1.13, MOSI=P0.28, CS=P0.02)
├── dc-gpios = P0.03         → 数据/命令选择
├── reset-gpios = P1.10      → 硬件复位
└── sitronix,st7789v (display)
    ├── width=320, height=172
    ├── x-offset=0, y-offset=35
    ├── mdac=0xa0, colmod=0x55
    ├── inversion-on
    └── (gamma/VCOM/porch 等初始化参数)
```

### 7.2 驱动调用链

```
lv_timer_handler()
  └── lv_display_flush()
        └── display_write(display_dev, x, y, w, h, px_map)  ← Zephyr display API
              └── st7789v_write()                            ← sitronix,st7789v 驱动
                    ├── st7789v_set_mem_area()               ← CASET / RASET
                    ├── st7789v_transmit(RAMWR)
                    └── mipi_dbi_write_display()             ← mipi-dbi-spi 传输层
                          └── spi_write(&spi3, ...)           ← Zephyr SPI API
```

面板初始化（编译时 `SYS_INIT`）：
```
st7789v_lcd_init()
  ├── st7789v_transmit(CMD2EN)    ← 进入扩展命令模式
  ├── st7789v_transmit(PORCTRL)   ← Porch 参数
  ├── st7789v_transmit(GCTRL)     ← Gate Control
  ├── st7789v_transmit(VCOMS)     ← VCOM
  ├── st7789v_transmit(GAMSET)    ← Gamma
  ├── st7789v_transmit(PVGAMCTRL) ← 正极性 Gamma
  ├── st7789v_transmit(NVGAMCTRL) ← 负极性 Gamma
  ├── st7789v_transmit(COLMOD)    ← RGB565
  ├── st7789v_transmit(MADCTL)    ← 0xa0
  ├── st7789v_transmit(INVON)     ← 反转开
  └── st7789v_transmit(DISPON)    ← 显示开
```

### 7.3 Kconfig（`prj.conf` 新增）

```ini
# Zephyr display 驱动链
CONFIG_DISPLAY=y
CONFIG_ST7789V=y
CONFIG_ST7789V_RGB565=y

# LVGL（由 Zephyr 自动初始化 + 专用 workqueue 驱动）
CONFIG_LVGL=y
CONFIG_LV_COLOR_DEPTH_16=y
CONFIG_LV_Z_MEM_POOL_SIZE=8192
CONFIG_LV_USE_LABEL=y
CONFIG_LV_FONT_MONTSERRAT_14=y
CONFIG_LV_Z_RUN_LVGL_ON_WORKQUEUE=y
```

## 8. 文件变更

| 文件 | 操作 | 说明 |
|------|------|------|
| `key_board.overlay` | 修改 | 新增 `mipi-dbi-spi` + `st7789v` DT 节点；移除 `lcd_ctrl` 节点（DC/RST 并入 MIPI DBI） |
| `prj.conf` | 修改 | 新增 LVGL + DISPLAY + ST7789V Kconfig；`LV_Z_RUN_LVGL_ON_WORKQUEUE=y` |
| `src/lcd_display.c` | 重写 | 删除所有 SPI/ST7789/LVGL 初始化代码；仅保留 UI 逻辑 |

### 8.1 lcd_display.c 变更清单

| 操作 | 内容 |
|------|------|
| **保留** | 工作队列 `lcd_work_q`、`lcd_state_cache`、三路 `cache_req` + `work`、CAF event listener |
| **保留** | `mode_to_text()`、`ble_state_to_text()` 映射函数 |
| **保留** | `lcd_create_ui()` widget 树构建（豆沙绿主题） |
| **保留** | 背光 GPIO 控制（独立于 display driver） |
| **删除** | SPI 常量/句柄/函数（`lcd_spi_write`、`lcd_write_cmd`、`lcd_write_data`、`lcd_write_cmd_data`） |
| **删除** | ST7789 命令序列（`lcd_cmd_sequence`、`lcd_set_window`） |
| **删除** | GPIO 控制函数（`lcd_reset_panel`、`lcd_configure_bus_gpio` — 改由 MIPI DBI 管理） |
| **删除** | 颜色常量（`LCD_COLOR_*`）、字体常量（`LCD_FONT_*`）、`lcd_line_buf` |
| **删除** | 所有手绘函数（5x7 字体、`lcd_fill_rect`、`lcd_draw_*`、`lcd_render_status_screen`） |
| **删除** | `lcd_flush_cb`、`lvgl_timer_work`、LVGL 初始化代码 |
| **新增** | `#include <lvgl_zephyr.h>` — 提供 `lvgl_lock()` / `lvgl_unlock()` |
| **新增** | `ui_create_work` — 在 `lcd_work_q` 执行 widget 创建 |
| **修改** | 三个 `cache_write_work_handler` — 用 `lvgl_lock()/unlock()` 包裹 LVGL API 调用 |
| **修改** | `lcd_display_init()` — 只做：启工作队列 → 初始化 work → 背光 → 提交 UI 创建 |

### 8.2 key_board.overlay 变更

**移除**：`lcd_ctrl` 节点（`lcd_dc` P0.03 和 `lcd_reset` P1.10 的 `gpio-leds` 包装）。这两个引脚改由 `mipi-dbi-spi` 节点的 `dc-gpios` 和 `reset-gpios` 属性管理。

**新增**：`mipi_dbi` 节点（`zephyr,mipi-dbi-spi` 兼容），包含 `display: st7789v@0` 子节点。

## 9. 预期日志

```
<inf> lcd_display: LCD_INIT begin
<inf> lcd_display: LCD_BL on
<inf> display_st7789v: Display initialized      ← Zephyr driver（SYS_INIT 阶段）
<inf> lcd_display: LCD_UI created
<dbg> lcd_display: LCD_EVT mode=2
<dbg> lcd_display: LCD_EVT ble=4
<dbg> lcd_display: LCD_EVT bat=75 st=0
```

注：`display_st7789v: Display initialized` 在 POST_KERNEL 阶段输出，早于 `main()`。`LCD_INIT begin` 在 `main()` 中输出。

## 10. 版本记录

| 日期 | 变更 |
|------|------|
| 2026-05-23 | v1.0：三路拆分 cache 写入请求 + 单队列串行 + 5 秒心跳 + LVGL 迁移路径预留 |
| 2026-05-23 | v2.0：LVGL v9.3.0 集成，豆沙绿 `#C7EDCC` 主题，删除手绘代码，5ms LVGL 心跳替代 5s 全量重绘 |
| 2026-05-23 | v2.1：Zephyr ST7789V display driver + MIPI DBI SPI 接管硬件层；LVGL 走 `LV_Z_AUTO_INIT` + `LV_Z_RUN_LVGL_ON_WORKQUEUE`；`lcd_display.c` 精简为纯 UI 逻辑 |

## 11. v2.1 构建修正记录

本次构建修正不改变 LCD 架构，只补齐 Zephyr Devicetree 与 LVGL 绑定要求：

- `mipi_dbi: mipi-dbi-spi` 必须放在根节点 `/ { ... };` 内部，不能裸露在根节点之外。否则 Devicetree 解析会在 `st7789v@0` 的 `reg = <0>;` 附近报 `expected label reference (&foo)`。
- 根节点必须增加 `chosen { zephyr,display = &display; };`，用于告诉 `lvgl_zephyr.h` 默认显示控制器是哪一个。否则 LVGL 编译期会报 `Could not find "zephyr,display" chosen property, or "zephyr,displays" compatible node in DT`。
- `sitronix,st7789v` 当前绑定只提供 `inversion-off` 属性。驱动默认行为是开启反色，因此不需要写 `inversion-on`；保留该未绑定属性可能触发后续 Devicetree 绑定校验问题。

修正后的关键结构如下：

```dts
/ {
    chosen {
        zephyr,display = &display;
    };

    mipi_dbi: mipi-dbi-spi {
        compatible = "zephyr,mipi-dbi-spi";
        spi-dev = <&spi3>;
        dc-gpios = <&gpio0 3 GPIO_ACTIVE_HIGH>;
        reset-gpios = <&gpio1 10 GPIO_ACTIVE_LOW>;
        write-only;
        #address-cells = <1>;
        #size-cells = <0>;

        display: st7789v@0 {
            compatible = "sitronix,st7789v";
            reg = <0>;
            mipi-max-frequency = <8000000>;
            mipi-mode = "MIPI_DBI_MODE_SPI_4WIRE";
            width = <320>;
            height = <172>;
            x-offset = <0>;
            y-offset = <35>;
        };
    };
};
```

| 2026-05-23 | v2.1-fix：修正 `mipi_dbi` 节点层级，新增 `zephyr,display` chosen，移除未绑定的 `inversion-on` 属性 |

## 12. LCD 直写色块诊断

背光已确认可以点亮后，新增一次性 RGB565 色块诊断路径，用于绕过 LVGL，直接验证 Zephyr display driver 到 ST7789V 面板的像素写入链路。

诊断位置：`lcd_display_init()` 中 `display_blanking_off()` 成功之后、提交 `ui_create_work` 之前。

诊断逻辑：

- 通过 `DT_CHOSEN(zephyr_display)` 获取当前显示设备。
- 调用 `display_get_capabilities()` 获取分辨率。
- 使用一行 `RGB565` 缓冲，通过 `display_write()` 按行写入整屏。
- 屏幕上 1/3 区域写红色 `0xf800`，中间 1/3 写绿色 `0x07e0`，底部 1/3 写蓝色 `0x001f`。
- 色块保留约 2 秒后，再提交 LVGL UI 创建工作项。

判断方式：

- 若上电后出现红/绿/蓝色块，说明 SPI、DC、CS、RST、ST7789V 初始化和 `display_write()` 基本可用，后续应排查 LVGL flush 或 UI 时序。
- 若仍然只有背光无画面，说明问题在 ST7789V 初始化参数、SPI 时序、屏幕驱动型号或屏幕连线方向。

预期日志：

```text
<inf> lcd_display: LCD_DISPLAY on
<inf> lcd_display: LCD_TEST rgb_bars
<inf> lcd_display: LCD_UI created
```
