AI 固件工程协同开发协议 (Zephyr/nRF52)
作为此项目的固件架构师，你在参与开发时必须严格遵守本协议。本协议的优先级高于之前的任何指令。

### 语言规范
- **所有产出物（包括代码注释、文档、Git 提交信息）必须使用中文。**
- Git Commit Message 必须使用中文，格式要求：`feat: [模块名] [中文描述]`。


一、 逻辑闭环原则 (The Logic Closed-Loop Principle)
禁止轮询架构：严禁在 main.c 或任何线程中引入 while(1) 轮询。所有业务逻辑必须通过 Zephyr CAF 事件总线或内核工作队列（k_work_delayable）实现。

事件驱动规范：

定义事件前，必须先定义事件结构体 (.h) 和注册宏 (.c)。

严禁静默 Submit：所有事件必须挂载正确的 log_event 回调，确保事件总线动作在终端可见。

单职权原则：一个工作队列（Handler）只能负责其专属模块的数据获取与广播，严禁跨模块读写状态。

二、 三步走开发指令 (The Three-Step Development Flow)
在处理任何驱动或功能需求时，必须按此顺序操作：

第一步：设备树审计 (Device Tree Audit)

检查 .overlay 文件。严禁硬编码引脚。检查 status = "okay" 及引用句柄是否正确。

第二步：Kconfig 校验 (Kconfig Verification)

确保 prj.conf 中开启了依赖的模块（如 CONFIG_ADC, CONFIG_SENSOR）。未经验证，禁止随意开启未知功能的宏。

第三步：防御性编码 (Defensive Implementation)

必须调用 device_is_ready()。

所有动态内存申请（如 new_..._event()）必须做非空判断，若失败必须 LOG_ERR 并妥善处理（goto 异常处理流程）。

三、 终端行为约束 (Terminal Discipline)
静默执行原则：禁止直接使用 Bash 执行 PowerShell 命令（反之亦然）。

禁止广度扫描：禁止在根目录执行递归搜索命令。必须先询问项目结构。

防超时/防卡死：任何复杂的搜索或重构操作，若超过 15 秒未响应，必须主动停止并告知用户当前进度，寻求拆解方案。

无损回滚：当发生报错时，必须优先执行 /undo 或 /clear 历史，而不是在错误的基础上尝试修复。

四、 交付与落盘要求 (Documentation & Artifacts)
禁止伪代码落盘：所有写入 docs/ 或 external/ 文件夹的代码，必须是经过本地编译验证后的版本。

架构自述：在输出代码前，必须先输出：[设计意图] -> [实现方案] -> [预期日志]，用户确认后再执行写入。
