# Project Context: NRF-KEYBOARD

## 0. 协同规则 (AI 必须强制执行)
- **初始化指令**: 每次对话开始时，请务必以此文件定义的路径和规范为唯一基准。
- **环境约束**: **严禁执行任何 Shell 脚本 (Bash/PowerShell) 进行全局路径扫描或搜索 (如 Get-ChildItem, find 等)**。
- **路径获取**: 必须通过读取此文件获取路径信息，若发现路径缺失，请直接询问用户，禁止自行探测。
- **编码准则**: 严格遵守《AI 固件工程协同开发协议》（参见第 3 节）。

## 1. 系统概览
- **芯片平台**: nRF52840 (Nordic)
- **SDK 版本**: nRF Connect SDK v3.2.3 (支持 Sysbuild 编译体系)
- **构建系统**: Zephyr RTOS / West
- **项目架构**: 事件驱动架构 (CAF - Common Application Framework)

## 2. 关键物理路径 (核心约束)
*严禁执行 `Get-ChildItem` 或 `find` 搜索以下路径，请直接使用这些变量：*
- **PROJECT_ROOT**: `C:/NRF/KEYBOARD`
- **BOARD_DIR**: `C:/NRF/KEYBOARD/boards/arm/key_board`
- **DEVICE_TREE**: `C:/NRF/KEYBOARD/boards/arm/key_board/key_board.overlay`
- **EVENT_HEADERS**: `C:/NRF/KEYBOARD/inc/events/`
- **EVENT_SOURCES**: `C:/NRF/KEYBOARD/src/events/`
- **SCHEME_DOCS**: `C:/NRF/KEYBOARD/external/`

## 3. 开发规范 (必须强制执行)

### 3.1 编码准则
- **绝对禁止轮询**: 所有业务逻辑必须基于 `k_work` 队列或 `app_event_manager`。禁止在 `main.c` 中出现 `while(1)`。
- **事件日志**: 必须注册 `log_event` 回调，日志格式固定为 `e:module_name field=value`。
- **事件宏版本对齐**: 注册事件调用 `APP_EVENT_TYPE_DEFINE` 时，第四个参数（Flags）必须固定填 `0`，禁止使用废弃的 `APP_EVENT_FLAGS_NONE`，防止触发 v3.2.3 SDK 的编译期静态断言 (BUILD_ASSERT) 卡死。
- **防御性编程**: 调用 `new_..._event()` 后必须检查 `NULL`。必须调用 `device_is_ready()` 审计外设硬件状态。
- **外设管理**: 严禁在代码中硬编码引脚，必须从设备树 (Device Tree) 读取。

### 3.2 全局开发守则
- **方案先行 (Docs First)**: 实现任何重要的新功能、驱动或模块之前，**必须**先在 `C:/NRF/KEYBOARD/external/` 目录下编写或更新对应的 `.md` 设计方案文档。
- **确认机制**: 方案文档落盘并向用户汇报核心逻辑后，必须遵循：`[设计意图] -> [实现方案] -> [预期日志]` 架构自述。必须等待用户明确同意，才能开始实际编写 `.c` 或 `.h` 等源码文件。
- **编译职责划分**: 编译与烧录由用户手工自行在终端触发。**AI 严禁在对话或代码注释中输出任何 `west build` / `flash` 等终端编译调试指令**。但若需求涉及新文件创建，AI 必须给出对应的 `CMakeLists.txt` 修改片段（使用 `target_sources` 宏）以保证构建依赖完整。
- **日志规范**: 除非用户特别要求，调试通过后自动精简底层循环日志，保持串口终端清爽。

## 4. 当前开发进度
- [x] 电源管理模块 (IP5305T 保活机制与 ADC 低功耗采样)
- [x] 拨档开关切换逻辑
- [x] CAF 事件总线初步搭建
- [x] 键盘核心层状态机 (keyboard_core 全键 Bitmap 差分管理与修饰键过滤)
- [ ] 待优化：事件回调的日志对齐与性能监控
