# 双调试环境方案文档

## 1. 设计意图

当前项目同时使用两套调试环境：

- `build_debug`：AI 与开发阶段使用的工程构建目录，用于验证源码改动、Devicetree、Kconfig、链接关系和模块级日志。
- 成品项目 debug：面向实际成品固件调试的环境，用于验证用户实际烧录版本、硬件现象、RTT 运行日志和最终交付行为。

这两套环境必须明确分工，避免出现“源码已经改了，但烧录的不是当前构建产物”或“RTT 看到的是另一套固件日志”的误判。

## 2. 环境职责边界

| 环境 | 主要用途 | 适合观察的问题 | 不建议承担的职责 |
|------|----------|----------------|------------------|
| `build_debug` | 开发验证、编译排错、模块级诊断 | Devicetree 解析、Kconfig 生效、C 编译错误、链接错误、临时诊断日志 | 不作为最终成品行为依据 |
| 成品项目 debug | 实机行为验证、成品固件调试 | 背光、屏幕、USB/BLE、按键、功耗、RTT 真实运行日志 | 不直接用于判断当前源码是否已编译进去 |

核心原则：

- 看到 RTT 日志前，必须先确认该日志来自哪一套固件。
- 修改源码后，必须确认对应构建目录已经重新生成固件，并且烧录的是该目录下的产物。
- 当 `build_debug` 和成品项目 debug 同时打开时，RTT 日志必须带有可区分的启动标识或模块日志。

## 3. `build_debug` 约定

`build_debug` 用于开发阶段快速定位问题，允许保留比成品环境更多的诊断日志。

建议用途：

- 验证 `.overlay` 是否能被 Devicetree 正确解析。
- 验证 `prj.conf` 中新增 Kconfig 是否真实进入 `.config`。
- 验证新增源文件是否已进入 `CMakeLists.txt`。
- 验证模块初始化路径是否按预期执行。
- 临时加入一次性诊断，例如 LCD RGB 色块、BLE 状态日志、USB HID 报告路径日志。

建议日志等级：

```ini
CONFIG_LOG_DEFAULT_LEVEL=3
CONFIG_APP_EVENT_MANAGER_LOG_LEVEL_DBG=y
```

必要时可对单模块提高日志等级，但不建议全局长期使用 `CONFIG_LOG_DEFAULT_LEVEL=4`，避免 RTT 被内核或驱动底层日志淹没。

## 4. 成品项目 debug 约定

成品项目 debug 用于确认最终固件在真实硬件上的表现。

建议用途：

- 验证背光、LCD、按键矩阵、旋钮、USB HID、BLE HID 的真实行为。
- 验证实际烧录固件中的 RTT 日志。
- 验证休眠、唤醒、电池事件、BLE 配对恢复等用户可见行为。
- 验证临时诊断移除后的正常运行状态。

建议日志等级：

```ini
CONFIG_LOG_DEFAULT_LEVEL=3
```

成品调试完成后，若进入低噪声版本，可降为：

```ini
CONFIG_LOG_DEFAULT_LEVEL=1
```

但只要需要观察 CAF 事件日志，`CONFIG_LOG_DEFAULT_LEVEL` 必须至少为 `3`，否则 `APP_EVENT_MANAGER_LOG` 可能在编译期被裁剪。

## 5. 双环境常见误判

### 5.1 RTT 日志不是当前源码

现象：

- 源码中已有 `LCD_TEST rgb_bars`，但 RTT 中只看到旧日志。
- 源码中没有 `LCD_UI render`，但 RTT 中反复出现 `LCD_UI render`。

判断：

- 当前烧录固件不是当前源码对应的构建产物。
- 或 RTT 在启动后较晚连接，错过了启动阶段诊断日志。
- 或连接到了另一块板、另一个 J-Link 会话或另一套 debug 固件。

处理：

- 先打开 RTT，再复位设备，从 `00:00:00` 捕获启动日志。
- 检查是否出现当前源码中新增的唯一日志标识。
- 若没有出现，先确认烧录产物来源，再继续分析硬件现象。

### 5.2 `.config` 与 `prj.conf` 不一致

现象：

- `prj.conf` 中新增了配置，但 `build_debug/KEYBOARD/zephyr/.config` 中没有。

判断：

- 构建目录未重新配置。
- 配置项被依赖关系屏蔽。
- 配置项写在注释行中，没有成为独立 Kconfig 行。

处理：

- 确认配置项在 `prj.conf` 中独立成行。
- 查看 `.config` 中最终是否生效。
- 若 clean build 后仍不生效，再查 Kconfig 依赖。

### 5.3 LCD 背光亮但无画面

判断顺序：

```text
device_is_ready(display) 返回什么？
    |
    +-- false：检查 chosen zephyr,display、mipi-dbi-spi 层级、ST7789V 绑定
    |
    +-- true
          |
          SCREEN_BLK / P1.11 是否拉低？
          |
          +-- 否：检查 lcd_backlight GPIO 与 GPIO_ACTIVE_LOW
          |
          +-- 是
                |
                display_write() 返回什么？
                |
                +-- 非 0：检查 pixel format、窗口参数、buffer descriptor
                |
                +-- 0：检查 INVON、y-offset、DC/CS/RST/SPI 时序和屏幕型号
```

## 6. 推荐启动日志标识

为了区分两套环境，建议在 `build_debug` 和成品项目 debug 中保留不同的启动日志标识。

示例：

```text
<inf> main: 固件环境=build_debug
```

或：

```text
<inf> main: 固件环境=product_debug
```

若不希望修改业务源码，也可以使用不同的模块级诊断日志作为临时标识，例如：

```text
<inf> lcd_display: LCD_TEST rgb_bars
```

只要该日志在两套环境中唯一，就能判断 RTT 当前看到的是哪一套固件。

## 7. AI 协作约束

AI 在协助双环境调试时必须遵守：

- 未经用户明确要求，不主动触发构建。
- 未经用户明确要求，不执行烧录。
- 修改源码前必须先说明设计意图、实现方案和预期日志。
- 排查运行日志时，必须先确认日志来自哪一套 debug 环境。
- 若发现源码与 RTT 日志不一致，应优先提示“固件来源不一致”，而不是继续按旧日志推断源码问题。

## 8. 当前 LCD 调试建议

当前 LCD 背光已经确认可以点亮，说明：

- `SCREEN_BLK / P1.11` 路径有效。
- `GPIO_ACTIVE_LOW` 判断目前成立。
- 问题已经进入显示控制器或像素写入阶段。

下一步应优先使用 `build_debug` 的一次性 RGB 色块诊断确认 `display_write()` 是否真正触达屏幕：

- 若 RTT 出现 `LCD_TEST rgb_bars` 且屏幕出现色块，继续排查 LVGL。
- 若 RTT 出现 `LCD_TEST rgb_bars` 但屏幕仍无画面，继续查 ST7789V 初始化参数、`INVON`、`y-offset`、`DC/CS/RST/SPI`。
- 若 RTT 没有 `LCD_TEST rgb_bars`，先确认当前烧录固件是否来自包含该诊断的构建产物。
