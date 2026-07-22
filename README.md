# My-RTOS

My-RTOS 是一个面向 ARM Cortex-M 的自研微内核实验系统。当前主线平台为
**RP2350 / Pico 2 W（Cortex-M33, ARMv8-M, Non-secure）**，同时保留
**STM32F767 / Nucleo-F767ZI（Cortex-M7）** 作为经典路径并行支持。

目标不是只实现一个最小调度器，而是逐步把 RTOS 推向具备用户/内核隔离、系统调用、
capability 权限、IPC request/reply、VFS、驱动模型、fault 隔离和诊断能力的微内核
风格操作系统。完整路线图见 [`docs/planning/MICROKERNEL_OS_ROADMAP.md`](docs/planning/MICROKERNEL_OS_ROADMAP.md)
（9 个 Phase / ~6 个月）。

当前状态可以概括为：**具备微内核核心机制的 RTOS 原型**。调度、任务生命周期、
MPU 隔离（PMSAv8 完整 MAIR 表）、SVC/syscall、capability、endpoint/channel IPC、
VFS/devfs/ramfs、IRQ/BH、timer、trace/stats、shell 和板级测试框架都已经接入，RP2350
板上测试基线为 **2867/2867 PASS**。

## 当前能力

- 抢占式优先级调度器
  - 32 级优先级
  - bitmap 优先级查找
  - 同优先级时间片轮转
  - task delay / join / delete / fault cleanup

- 任务和用户态隔离
  - kernel task / user task 属性
  - Cortex-M MPU 用户栈隔离
  - Flash 用户只读可执行映射
  - 用户任务 fault 后可终止并清理资源
  - 用户任务返回时通过 SVC 退出

- SVC / syscall
  - 统一 6 参数 syscall 分发
  - user pointer validation / copy_from_user / copy_to_user
  - capability-aware syscall 路径
  - endpoint send/recv 已支持 sleepable syscall continuation

- IPC
  - semaphore
  - mutex，包含死锁检测和优先级继承
  - message queue
  - event flags
  - endpoint，多客户端到服务端 request/reply
  - channel，包含消息和共享内存接口雏形
  - IPC cap transfer 基础结构

- Capability 权限系统
  - cap id 使用 slot + generation
  - cap type / rights 检查
  - derive / transfer / revoke
  - revoke tree
  - 对象引用计数和 cleanup hook
  - 每任务 cap set

- VFS / 文件描述符
  - inode tree
  - per-task fd table
  - ramfs
  - devfs
  - readdir
  - path normalization
  - mount / unmount
  - fd 生命周期和 task exit 自动关闭

- 设备和驱动
  - device registry
  - devfs 设备节点绑定
  - UART 字符设备
  - GPIO/LED 设备示例
  - open/read/write/ioctl 统一入口

- Timer / IRQ / Bottom Half
  - min-heap software timer
  - timer service task
  - threaded IRQ 基础框架
  - bottom-half service task
  - 队列满、删除、等待者唤醒等工程化处理

- Fault / Trace / Stats
  - MemManage / BusFault / UsageFault / HardFault 诊断
  - crash dump
  - 用户 fault 隔离
  - trace ring buffer
  - per-task CPU usage
  - syscall / IPC / mem / device / fault 统计

- Shell
  - UART shell
  - `ps`, `top`, `free`, `mem`
  - `ls`, `cat`, `echo`
  - `trace`, `stats`, `crash`, `hexdump`
  - `version`, `reset`, `clear`

- 测试框架
  - 板上自举测试
  - 模块化 test registry
  - 覆盖 scheduler、task、timer、IRQ、MPU、syscall、usercopy、capability、
    fault、VFS、driver、IPC upgrade、watchdog、stats、trace、memory、diagnostics

## 与普通 RTOS 的区别

FreeRTOS 这类传统 RTOS 通常提供小而稳定的调度、队列、信号量和软件定时器，应用任
务大多运行在同一信任域。My-RTOS 的目标更接近微内核：

- 用户任务不应直接访问内核对象
- 系统资源通过 capability 授权
- 用户态通过 SVC 进入内核
- 服务之间通过 endpoint/channel IPC 通信
- fault 的用户任务应被隔离清理，而不是拖垮整个系统
- 长期目标是 FS、driver、device manager、name server 等都运行在用户态

因此 My-RTOS 当前更适合作为 RTOS 内核、微内核架构、安全隔离和嵌入式 OS 机制的研
究项目，而不是直接替代成熟工业 RTOS。

## 当前微内核化进度

已完成的核心方向：

- P0: 任务生命周期、上下文切换、调度稳定性和基础边界修复
- P1: endpoint/channel IPC、capability、VFS/fd 生命周期
- P2: timer/IRQ/BH、device、memory、shell、trace/stats 工程化
- P3: usercopy、fault cleanup、request/reply IPC、最小用户态服务路径、
  sleepable endpoint syscall
- P4: name server、service registry、IRQ notification、MMIO/SHM capability、
  用户态 driver server、用户态 FS server、shell-managed 服务生命周期与恢复原型

仍待完成的微内核方向：

- root/init 用户态任务
- 完整 CSpace/CNode
- reply cap 一等对象
- 全 syscall sleepable continuation
- root/init 统一 supervisor 和服务崩溃恢复策略

## 支持平台

双轨构建系统：**RP2350 走 CMake + Pico SDK**，**STM32F767 走经典 Make**。两个板子
各自维护一份默认 defconfig；feature matrix 见 [`docs/BOARD_SUPPORT.md`](docs/BOARD_SUPPORT.md)。

| 平台 | 构建系统 | 状态 | 说明 |
| --- | --- | --- | --- |
| **RP2350 / Pico 2 W** | CMake + Pico SDK 2.2.0 | **主验证平台** | Cortex-M33 NS / ARMv8-M / PMSAv8 / 2×M33 + 2×Hazard3 RV (本期仅用 Core 0) |
| STM32 Nucleo-F767ZI | 经典 Make | 维护中 | Cortex-M7 / PMSAv7 / 历史主线 |

每板各自的串口：

| 平台 | UART | Pins | Baudrate | Format |
| --- | --- | --- | --- | --- |
| RP2350 | uart0 | GPIO0 (TX) / GPIO1 (RX) | 115200 | 8N1 |
| STM32F767 | USART3 | PD8 / PD9 | 115200 | 8N1 |

## 配置预设

仓库内置 4 个通用 defconfig 预设和 1 个 RP2350 SMP 验收预设：

| 预设 | 文件 | 目标 | 估计 footprint |
| --- | --- | --- | --- |
| **tiny** | `configs/tiny_defconfig` | 最小可用镜像；单任务、无 shell/test/trace | ~24-32KB flash / ~4-8KB RAM |
| **default** | `configs/default_defconfig` | 开发 + 测试基线（2867/2867 PASS） | ~120-180KB flash / ~32-48KB RAM |
| **release** | `configs/release_defconfig` | 生产：去 test/shell/trace，保留 IPC/cap/fault | ~60-90KB flash / ~16-24KB RAM |
| **full** | `configs/full_defconfig` | 所有子系统 ON（包括未实装的 Phase 2-9 占位） | ~300-800KB flash / ~64-128KB RAM |
| **rp2350_smp** | `configs/rp2350_smp_defconfig` | M1 双核验收；sem/endpoint 跨核各 100 万轮 | 实验配置，不用于 release |

切换：

```bash
cp configs/<preset>_defconfig .config
python3 scripts/menuconfig.py genconfig
```

双核 M1 验收也可直接使用 `make rp2350_smp_defconfig`。生产
`release_defconfig` 仍保持 `SMP=n`。

双核镜像烧录后，可让串口门禁先确认 100 万轮压力模块全部通过，再持续做
shell 活性探测并监控 panic/fault/reset。30 分钟门禁示例：

```bash
make test-smp-soak PORT=/dev/ttyACM0 DURATION=1800
```

8 小时和 24 小时分别使用 `DURATION=28800`、`DURATION=86400`；日志默认写到
`/tmp/my-rtos-smp-soak.log`。

每个 `CONFIG_*` 的细节见 [`docs/kconfig/INDEX.md`](docs/kconfig/INDEX.md)（共 123 个
配置项，每个都有独立页面）。

## 目录结构

```text
my-rtos/
├── configs/                    # defconfig
├── docs/                       # 设计文档、调试记录、阶段资料
├── link/                       # linker scripts
├── scripts/                    # menuconfig / genconfig
├── src/
│   ├── app/                    # shell 和入口应用
│   ├── arch/arm/cortex-m/      # PendSV, SVC, first switch, HAL (M7/M33 共享)
│   ├── board/stm32f767/        # STM32F767 board support
│   ├── drivers/                # UART/GPIO 和设备驱动适配
│   ├── hal/                    # HAL interface
│   ├── kernel/
│   │   ├── cap/                # capability system
│   │   ├── core/               # scheduler
│   │   ├── dev/                # device registry
│   │   ├── fault/              # fault handling and crash dump
│   │   ├── include/            # kernel config and common types
│   │   ├── ipc/                # sem/mutex/mqueue/event/endpoint/channel
│   │   ├── irq/                # IRQ and bottom-half
│   │   ├── mem/                # heap and mempool
│   │   ├── mpu/                # MPU protection
│   │   ├── stats/              # counters and CPU usage
│   │   ├── syscall/            # syscall dispatcher and user API
│   │   ├── task/               # task lifecycle
│   │   ├── timer/              # software timers
│   │   ├── trace/              # trace buffer
│   │   ├── usercopy/           # user pointer validation
│   │   └── vfs/                # VFS, inode, ramfs, devfs
│   ├── startup/arm/cortex-m7/   # vectors, reset, fault assembly
│   └── tests/                  # board test suite
├── tools/                      # local ARM GNU toolchain and helper tools
├── Kconfig
├── Makefile
└── README.md
```

## 构建环境

推荐在 Linux 环境下构建。仓库默认使用本地工具链路径：

```text
tools/arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-eabi/bin
```

常用外部工具：

- `make`
- `python3`
- `openocd`，用于 STM32F767 烧录
- 串口终端，例如 `minicom`, `picocom`, `screen`

## 快速开始

### 路径 A：RP2350 / Pico 2 W（CMake，主验证平台）

首次需要拉取 Pico SDK 与 picotool（一次性，~50MB 下载 + 编译）：

```bash
make setup-pico-sdk
```

应用默认配置并构建：

```bash
cp configs/default_defconfig .config
python3 scripts/menuconfig.py genconfig
make            # 等价于 cmake --build build/rp2350-pico-sdk -j4
```

烧录（DAPLink / CMSIS-DAP，经 OpenOCD，ELF 优先）：

```bash
make flash
# 或手动：
openocd -f tools/openocd.cfg \
    -c "program build/rp2350-pico-sdk/my-rtos-pico2w.elf verify reset exit"
```

串口（DAPLink CDC 通常枚举为 `/dev/ttyACM0`）：

```bash
picocom -b 115200 /dev/ttyACM0
```

### 路径 B：STM32F767 / Nucleo-F767ZI（经典 Make）

```bash
make stm32f767_defconfig
make BOARD=stm32f767
make BOARD=stm32f767 flash
```

### 通用

清理：

```bash
make clean
```

查看构建配置：

```bash
make info
```

交互式配置：

```bash
make menuconfig
make genconfig
```

### 一键回归（烧录 + 串口捕获 + 检查 PASS 行）

```bash
scripts/regression.sh                 # 默认 BOARD=rp2350, PORT=/dev/ttyACM0
BOARD=stm32f767 scripts/regression.sh # 切到 STM32
SKIP_FLASH=1 scripts/regression.sh    # 只验 build,不烧录
```

### 本地 CI 镜像（与 `.github/workflows/build.yml` 同款）

```bash
scripts/ci_local.sh                   # 全矩阵: 4 preset × rp2350 + stm32 + docs
scripts/ci_local.sh rp2350 default    # 单 preset
scripts/ci_local.sh docs              # 只验 Kconfig doc 同步
```

## 串口使用

烧录后连接板上 UART（RP2350 GPIO0/1 或 STM32 USART3），参数为 `115200 8N1`：

```bash
picocom -b 115200 /dev/ttyACM0
```

启动后会先运行测试框架，然后进入 shell：

```text
Test Framework Starting...
Starting scheduler...

My-RTOS Test Suite v2.0
...
All tests PASSED!

========================================
  My-RTOS Shell v1.0
  Type 'help' for available commands.
========================================

my-rtos>
```

## 常用 shell 命令

| 命令 | 说明 |
| --- | --- |
| `help` | 查看命令列表 |
| `ps` | 查看任务列表和状态 |
| `top` | 查看任务 CPU 使用率 |
| `free` | 查看 heap 使用情况 |
| `mem` | 查看内存/任务布局 |
| `ls [path]` | 查看 VFS 目录 |
| `cat <path>` | 读取文件 |
| `driver up/down/restart/health/recover` | 一键启动、停止、重启、健康检查或恢复用户态 driver 栈，并在 status 中显示健康统计 |
| `driver abi/status/lookup/registered/probe/probe-mmio` | 用户态 driver ABI、注册表、服务发现、注册状态和探测 |
| `fs up/down/restart/health/recover` | 一键启动、停止、重启、健康检查或恢复用户态 FS 栈，并在 status 中显示健康统计 |
| `fs abi/status/start/stop/probe/registered` | 用户态 FS 服务管理、注册状态和探测 |
| `fs ls [path]` | 通过用户态 FS 服务遍历目录 |
| `fs cat <path>` | 通过用户态 FS 服务读取文件 |
| `fs write <path> <text>` | 通过用户态 FS 服务创建/覆盖文件 |
| `fs rm <path>` | 通过用户态 FS 服务删除文件 |
| `fs mkdir <path>` | 通过用户态 FS 服务创建目录 |
| `fs stat <path>` | 通过用户态 FS 服务查询 inode 元信息 |
| `echo ...` | 输出文本 |
| `trace` | 查看 trace buffer |
| `stats` | 查看内核统计 |
| `crash` | 查看 crash dump |
| `hexdump` | 查看内存十六进制内容 |
| `version` | 查看内核版本 |
| `reset` | 复位系统 |
| `clear` | 清屏 |

示例：

```text
my-rtos> ls /
my-rtos> ls /dev
my-rtos> ps
my-rtos> top
my-rtos> stats
```

## 配置

主要配置文件：

- `.config`（本地状态，不入 git）
- `configs/<preset>_defconfig` — 4 个预设：tiny / default / release / full
- `configs/rp2350_defconfig`、`configs/stm32f767_defconfig` — 各板硬件默认
- `src/kernel/include/kernel_config.h`（生成产物）
- `docs/kconfig/INDEX.md` — 全部 123 个 `CONFIG_*` 的索引与每项详情

推荐通过下面命令生成配置头：

```bash
cp configs/<preset>_defconfig .config
make menuconfig    # 交互式
make genconfig     # 仅生成头文件
```

常见配置项包括：

| 配置 | 说明 |
| --- | --- |
| `KERNEL_MAX_TASKS` | 最大任务数 |
| `KERNEL_MAX_PRIORITIES` | 优先级数量 |
| `KERNEL_TICK_RATE` | tick 频率 |
| `MPU_ENABLE` | 启用 MPU 隔离 |
| `SYSCALL_ENABLE` | 启用 SVC/syscall |
| `CAP_ENABLE` | 启用 capability |
| `VFS_ENABLE` | 启用 VFS |
| `DRIVER_ENABLE` | 启用 device/driver |
| `TRACE_ENABLE` | 启用 trace |
| `KERN_TASK_STATS` | 启用任务统计 |

## 开发和测试

当前测试随固件一起编译，启动后在板上自动运行。测试通过后进入 shell。开发流程通常
是：

1. 修改内核、驱动或测试代码
2. `make BOARD=stm32f767`
3. `make BOARD=stm32f767 flash`
4. 通过串口查看测试结果
5. 进入 shell 做手工验证

测试模块位于 `src/tests/`，每个模块通过测试框架注册。新增测试时优先把边界条件、
失败路径和资源清理行为写成自动测试。

## 设计文档

仓库中保留了阶段设计和演进文档：

- `docs/planning/P0_OPTIMIZATION_PLAN.md`
- `docs/planning/P1_MICROKERNEL_CORE_PLAN.md`
- `docs/planning/P2_SERVICE_ENGINEERING_PLAN.md`
- `docs/reports/P2_COMPLETION_REPORT.md`
- `docs/planning/P3_MICROKERNEL_SERVICE_PLAN.md`
- `docs/planning/MICROKERNEL_MIGRATION_PLAN.md`
- `docs/MICROKERNEL_ROADMAP.md`
- `docs/IRQ_DESIGN.md`
- `docs/TIMER_DESIGN.md`
- `docs/DIAGNOSTIC_GUIDE.md`

这些文档记录了从 RTOS 内核到微内核风格系统的阶段性目标、已完成内容和后续迁移路
线。

## 当前限制

需要明确的是，My-RTOS 目前还不是完整意义上的用户态服务微内核：

- 内核 VFS/devfs、debug UART、timer、BH 仍保留兼容路径
- 已有 shell-managed FS/driver 服务生命周期原型，但还没有 root/init 统一 supervisor
- capability 还不是完整 CSpace/CNode 模型
- cap-bearing blocking IPC 还没有完整 continuation
- channel blocking syscall 仍有保守限制
- 不是所有阻塞 syscall 都已经 sleepable continuation 化
- 用户态服务已有手动 restart/recover 原型，崩溃后的统一 supervisor/revoke/restart 策略还未完成
- MMIO capability、IRQ notification、shared memory manager 已有基础路径，仍需产品化收口

这些限制是后续服务化阶段的主要工作。

## 路线图

短期重点：

- 收口所有 syscall 的 capability 检查
- 完成统一 sleepable syscall continuation
- 修复非法 SVC 号处理
- 禁止用户 callback 在内核服务任务中执行
- 扩展 endpoint/channel IPC 的 cap transfer continuation

中期重点：

- root/init task
- service supervisor
- 完整 CSpace/CNode
- 完整 sleepable syscall continuation
- 用户态 driver server 产品化
- 用户态 FS server 产品化，并把 shell-managed lifecycle 下沉到 root/init supervisor

长期目标：

- 内核只保留调度、IPC、capability、MPU/address-space、IRQ dispatch、fault handling
- 文件系统、驱动、设备管理、系统策略全部迁到用户态服务
- 形成清晰的 syscall ABI、IPC ABI 和服务协议

## License

MIT
