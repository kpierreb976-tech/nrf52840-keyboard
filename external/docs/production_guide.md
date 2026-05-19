# 生产环境指南 — NRF KEYBOARD v0.1-stable

## 1. 日志策略——"平时闭嘴，出事才吭声"

### 1.1 五级日志全景图

Zephyr 的日志系统支持编译期过滤——不是运行时过滤，是**直接在编译阶段把不需要的日志代码删掉**，不浪费 Flash 空间。

```
  CONFIG_LOG_DEFAULT_LEVEL
  ┌─── 0 (NONE)  ────────────────────────────── 完全静默，什么都不输出
  │
  ├─── 1 (ERR)   ─── 只看 LOG_ERR              ◄── 生产环境用这个
  │
  ├─── 2 (WRN)   ─── LOG_ERR + LOG_WRN
  │
  ├─── 3 (INF)   ─── + LOG_INF + APP_EVENT_    ◄── 日常开发用这个
  │                  MANAGER_LOG (事件日志)
  │
  └─── 4 (DBG)   ─── + LOG_DBG                  ◄── 深入调试用这个
```

### 1.2 生产环境 (`CONFIG_LOG_DEFAULT_LEVEL=1`) 的真实输出

正常运行时，你在 RTT 控制台上会看到的：

```
*** Booting Zephyr OS build v3.2.99-ncs2 ***
e:mode_event mode=1
e:battery_event lvl=75 st=0
e:battery_event lvl=75 st=0
e:battery_event lvl=75 st=0
```

每条日志的含义：

- `e:mode_event mode=1` —— 启动时检测到开关在 USB 档位。**只在开关被拨动时**才会出现，平时不刷屏。
- `e:battery_event lvl=75 st=0` —— 每 12 秒一条。lvl=75 表示电量 75%，st=0 表示正在放电。
- 如果一切正常，**看不到任何 LOG_ERR**。只有硬件出问题（ADC 坏了、I2C 断了）才会蹦 ERR。

### 1.3 如何切换日志级别

日志级别是编译期决定的事，改 `prj.conf` 然后重新编译：

```ini
# 生产环境（只保留错误）
CONFIG_LOG_DEFAULT_LEVEL=1

# 开发环境（能看到事件日志）
CONFIG_LOG_DEFAULT_LEVEL=3

# 调试环境（连初始化细节都看得到）
CONFIG_LOG_DEFAULT_LEVEL=4
```

如果想单独控制事件日志（`e:mode_event` 这类）的可见性：

```ini
CONFIG_APP_EVENT_MANAGER_LOG_LEVEL_DBG=y
```

> ⚠️ **重要提醒：** `APP_EVENT_MANAGER_LOG` 内部调用的是 `LOG_INF`。如果 `CONFIG_LOG_DEFAULT_LEVEL=1`，事件日志会**连同 LOG_INF 一起在编译时被剔除**，跟 `APP_EVENT_MANAGER_LOG_LEVEL` 设什么值无关。要想看到事件日志，`CONFIG_LOG_DEFAULT_LEVEL` 至少得是 3。

## 2. prj.conf 维护手册

### 2.1 生产基线配置（含注释）

```ini
# ═══════════════════════════════════════════
# 系统基础
# ═══════════════════════════════════════════
CONFIG_HEAP_MEM_POOL_SIZE=2048    # 事件对象从这里分配，不够会 OOM
CONFIG_ASSERT=y                   # 出现逻辑错误立即 halt，方便定位

# ═══════════════════════════════════════════
# 硬件驱动
# ═══════════════════════════════════════════
CONFIG_GPIO=y                     # 按键、VBUS、WAKEUP 都需要
CONFIG_I2C=y                      # 跟 IP5305T 通信用

# ═══════════════════════════════════════════
# 日志 (生产: 只看错误)
# ═══════════════════════════════════════════
CONFIG_LOG=y
CONFIG_LOG_DEFAULT_LEVEL=1        # ▼ 改这里控制日志量
CONFIG_CONSOLE=y
CONFIG_USE_SEGGER_RTT=y           # 用 J-Link RTT 通道输出
CONFIG_RTT_CONSOLE=y

# ═══════════════════════════════════════════
# CAF 事件管理器
# ═══════════════════════════════════════════
CONFIG_APP_EVENT_MANAGER=y        # 整个架构的核心
CONFIG_CAF=y
CONFIG_CAF_BUTTONS=y              # 按键子系统 (依赖 CAF)

# ═══════════════════════════════════════════
# 电源管理
# ═══════════════════════════════════════════
CONFIG_REBOOT=y
CONFIG_CAF_POWER_MANAGER=y
CONFIG_CAF_POWER_MANAGER_TIMEOUT=120          # 120 秒无操作自动休眠
CONFIG_CAF_POWER_MANAGER_ERROR_TIMEOUT=30     # 出错后 30 秒强制休眠
CONFIG_CAF_POWER_MANAGER_CLEAR_RESET_REASON=y # 启动时清除上次复位原因
CONFIG_CAF_KEEP_ALIVE_EVENTS=y                # 按键等事件可以重置 120s 倒计时
CONFIG_CAF_BUTTONS_PM_KEEP_ALIVE=y            # 按键按下即重置休眠定时器

# ═══════════════════════════════════════════
# ADC 与电池检测
# ═══════════════════════════════════════════
CONFIG_ADC=y
CONFIG_SENSOR=y
CONFIG_VOLTAGE_DIVIDER=y          # Zephyr 标准分压器驱动
CONFIG_PM_DEVICE=y                 # 设备级电源管理
CONFIG_PM_DEVICE_RUNTIME=y         # 运行时动态开关外设
```

### 2.2 开发调试时额外加这几行

```ini
CONFIG_LOG_DEFAULT_LEVEL=4                           # 啥日志都看
CONFIG_APP_EVENT_MANAGER_LOG_LEVEL_DBG=y             # 事件日志也打开
CONFIG_ASSERT=y                                       # 断言保持开启
# CONFIG_CAF_POWER_MANAGER_STAY_ON=y                 # 如果要测休眠，把这行注释掉
```

> ⚠️ `CONFIG_CAF_POWER_MANAGER_STAY_ON=y` 会让设备**永远不睡觉**。测功耗之前一定要注释掉。

## 3. 故障排查

### 3.1 RTT 控制台什么都看不到

| 检查项 | 怎么做 |
|--------|--------|
| RTT 控制台启用了没 | 确认 `CONFIG_RTT_CONSOLE=y` |
| SEGGER 连接正常吗 | 运行 `JLinkRTTClient` 或 `nrfjprog --rtt` |
| 日志级别是不是太高了 | `CONFIG_LOG_DEFAULT_LEVEL` 至少设成 3 |

### 3.2 看不到事件日志 (`e:mode_event` / `e:battery_event`)

这是最容易踩的坑，有四道关卡：

| 关卡 | 怎么过 |
|------|--------|
| `CONFIG_APP_EVENT_MANAGER_LOG_LEVEL` | 必须 ≥ 3 (INFO) —— 设成 `_DBG` 或 `_INF` 变体 |
| `CONFIG_LOG_DEFAULT_LEVEL` | 必须 ≥ 3 —— 因为 `APP_EVENT_MANAGER_LOG` 内部用 `LOG_INF`，级别不够整行编译没了 |
| `APP_EVENT_TYPE_FLAGS_INIT_LOG_ENABLE` | 检查事件 `.c` 文件里的 `APP_EVENT_FLAGS_CREATE()` 必须包含这个标志，否则 `log_event_init()` 不会给它开显示位 |
| `CONFIG_APP_EVENT_MANAGER_SHOW_EVENTS` | 默认就是 y，确认没被人手动关掉 |

### 3.3 IP5305T I2C 读不到数据

症状：串口出现 `LOG_ERR("Failed to read IP5305T status: %d", ret)`

此时固件会自动切换到电压估算备选方案——功能不中断，但电量显示精度会下降。排查方向：

- PMIC 深度休眠了（WAKEUP 脉冲没踢到它）→ 检查 P0.22 的 GPIO 配置和电平
- I2C 总线冲突 → 万用表量一下 SDA/SCL 上有没有上拉电阻，波形是否正常
- I2C 地址写错了 → 确认 overlay 里是 `reg = <0x75>`

### 3.4 档位开关永远报同一个模式

- 电压阈值跟实际分压对不上 → 用 LOG_DBG 级别看看原始 ADC 读数，重新校准阈值
- `power-gpios` 可能在 ADC 采样前没有拉高 → 检查 P0.09 (BAT_ADC_EN) 的电平时序
- 开关本身接触不良 → 硬件问题，万用表直接量分压点电压

### 3.5 设备不睡觉（功耗居高不下）

- `CONFIG_CAF_POWER_MANAGER_STAY_ON=y` —— 这个选项名字就叫 "保持唤醒"，生产环境必须注释掉
- `CONFIG_CAF_KEEP_ALIVE_EVENTS=y` 会在每次按键时重置 120 秒倒计时 —— 这是**正常行为**，不是 bug

## 4. 如何给系统扩展新功能

架构的核心设计原则是 "只跟事件总线打交道"。加新功能只需要三步。

### 4.1 定义一个新事件类型

**头文件** (`inc/events/my_event.h`)：
```c
#include <app_event_manager.h>
#include <app_event_manager_profiler_tracer.h>

struct my_event {
    struct app_event_header header;   // 必须放在第一个字段
    uint8_t data;                     // 你自己要带的数据
};

APP_EVENT_TYPE_DECLARE(my_event);
```

**实现文件** (`src/events/my_event.c`)：
```c
#include "events/my_event.h"

static void log_my_event(const struct app_event_header *aeh)
{
    const struct my_event *event = cast_my_event(aeh);
    APP_EVENT_MANAGER_LOG(aeh, "e:my_event data=%d", event->data);
}

APP_EVENT_TYPE_DEFINE(my_event,
    log_my_event,
    NULL,
    APP_EVENT_FLAGS_CREATE(APP_EVENT_TYPE_FLAGS_INIT_LOG_ENABLE)
    //                      ↑ 别忘了这个标志，否则日志不显示
);
```

然后把 `src/events/my_event.c` 加到 `CMakeLists.txt` 的 `target_sources` 里。

**在别处使用**：
```c
struct my_event *event = new_my_event();
event->data = 42;
APP_EVENT_SUBMIT(event);    // 扔进总线，完事
```

### 4.2 订阅别人的事件

如果你想在收到 `mode_event` 时顺便点亮一颗 LED：

```c
static bool on_mode_event(const struct app_event_header *aeh)
{
    const struct mode_event *event = cast_mode_event(aeh);
    // 根据 event->mode 决定亮哪颗灯
    return false; // false = 不消费事件，允许其他订阅者继续处理
}

APP_EVENT_LISTENER(my_listener, on_mode_event);
APP_EVENT_SUBSCRIBE(my_listener, mode_event);
```

## 5. 编译与烧录

```bash
# 配置工程
west build -b atjialidun/key_board -d build

# 烧录 (通过 SEGGER J-Link)
west flash -d build

# 或者用 nrfjprog 直接烧
nrfjprog --program build/zephyr/zephyr.hex --chiperase --reset

# 打开 RTT 控制台看日志
JLinkRTTClient
```

## 6. 版本记录

| 版本号 | 日期 | 变更内容 |
|--------|------|---------|
| v0.1-stable | 2026-05-19 | CAF 事件总线架构、mode_switch (5s 轮询 + 变化检测)、power_mgmt (12s 保活脉冲 + I2C 电量计 + 电压回退)、生产级日志 (ERR only)、模块化设计文档 |
