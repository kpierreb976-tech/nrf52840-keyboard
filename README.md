# NRF52840 Tri-Mode Mechanical Keyboard Firmware

这是一个基于 nRF52840 (QFN48) 和 Zephyr RTOS 构建的高性能、纯事件驱动的双模（USB/BLE）机械键盘（数字小键盘，17 键 + EC11 旋转编码器）固件项目。

## ✨ 核心特性

本固件彻底抛弃了传统的“大循环轮询”模式，全面拥抱 Zephyr CAF (Common Application Framework) 事件总线架构，实现了各硬件模块的**绝对零耦合**。

* **⚡ 纯事件驱动架构**：按键扫描、旋钮、电源、档位状态全异步广播，跨模块零直接函数调用。
* **🔄 无缝三模热切换**：通过 ADC 电压分压智能识别物理档位（USB/BLE/2.4G），切换时自动启停蓝牙广播与连接策略。
* **📶 进阶 BLE 状态机**：设计了四级安全门控（Disconnected → Connected → Encrypted → Ready），彻底消除加密握手期的按键积压与卡顿。支持设备 Identity 动态轮换，解决多主机切换的密钥冲突。
* **⌨️ Hybrid 动态 USB HID**：智能兼容，BIOS/UEFI 下自动剥离 Report ID 降级为标准 8 字节 Boot 协议，系统下无缝切换至 33 字节全键无冲 (NKRO) 协议。
* **🔋 智能电源保活**：深度集成 IP5305T 电源管理芯片 (I2C)，内置 12 秒保活脉冲逻辑防休眠，配合 ADC 动态电量估算。
* **🖥️ LVGL 现代图形引擎**：板载 ST7789V 屏幕，接入 Zephyr `mipi-dbi-spi` 驱动链与 LVGL v9.3 图形库。采用首创的“三路 Cache + 单队列串行”零锁并发模型，实现状态实时渲染防撕裂。

## 📂 深入了解系统架构

本项目的所有底层设计决策、状态机图与并发模型均有详尽的中文架构文档。欢迎查阅 `external/docs/` 目录：

* [系统架构总览](external/docs/architecture_overview.md)
* [BLE 传输层与安全策略](external/docs/ble_transport_design.md)
* [USB HID 动态协议分配](external/docs/usb_transport_design.md)
* [LCD 零锁并发模型与 LVGL 集成](external/docs/lcd_display_design.md)
* [电源管理与 IP5305T 状态机](external/docs/power_mgmt_design.md)

## 🛠️ 构建与烧录

本项目依赖 nRF Connect SDK (NCS) v3.2.3 环境。

```bash
# 1. 编译固件
west build -b key_board -d build_release

# 2. 烧录固件 (需连接 J-Link)
west flash -d build_release
