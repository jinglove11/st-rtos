# My-RTOS

My-RTOS 是一个面向 ARM Cortex-M 的自研 RTOS / 微内核实验系统。当前主线以
STM32 Nucleo-F767ZI 为主要验证平台，目标不是只实现一个最小调度器，而是逐步把
RTOS 推向具备用户/内核隔离、系统调用、capability 权限、IPC request/reply、VFS、
驱动模型、fault 隔离和诊断能力的微内核风格操作系统。

当前状态可以概括为：**具备微内核核心机制的 RTOS 原型**。调度、任务生命周期、
MPU 隔离、SVC/syscall、capability、endpoint/channel IPC、VFS/devfs/ramfs、IRQ/BH、
timer、trace/stats、shell 和板级测试框架都已经接入。后续真正微内核化的重点是把
FS、driver、device manager、name server 等服务迁到用户态。

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

仍待完成的微内核方向：

- root/init 用户态任务
- name server / service registry
- 用户态 FS server
- 用户态 driver server
- IRQ capability 和 IRQ-to-endpoint notification
- MMIO capability
- shared memory object
- 完整 CSpace/CNode
- reply cap 一等对象
- 全 syscall sleepable continuation
- 完整服务崩溃恢复和 supervisor

## 支持平台

| 平台 | 状态 | 说明 |
| --- | --- | --- |
| STM32 Nucleo-F767ZI | 主验证平台 | Cortex-M7，当前主要开发和板测目标 |
| RP2350 / Pico 2 | 构建入口存在 | 仍处于次要平台，主线验证以 STM32F767 为准 |

STM32F767 默认串口：

| 项目 | 配置 |
| --- | --- |
| UART | USART3 |
| Pins | PD8 / PD9 |
| Baudrate | 115200 |
| Format | 8N1 |

## 目录结构

```text
my-rtos/
├── configs/                    # defconfig
├── docs/                       # 设计文档、调试记录、阶段资料
├── link/                       # linker scripts
├── scripts/                    # menuconfig / genconfig
├── src/
│   ├── app/                    # shell 和入口应用
│   ├── arch/arm/cortex-m7/     # PendSV, SVC, first switch, HAL
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

加载 STM32F767 默认配置：

```bash
make stm32f767_defconfig
```

构建：

```bash
make BOARD=stm32f767
```

烧录：

```bash
make BOARD=stm32f767 flash
```

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

## 串口使用

烧录后连接 USART3，串口参数为 `115200 8N1`。例如：

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

- `.config`
- `configs/stm32f767_defconfig`
- `configs/rp2350_defconfig`
- `src/kernel/include/kernel_config.h`

推荐通过下面命令生成配置头：

```bash
make stm32f767_defconfig
make menuconfig
make genconfig
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

- `P0_OPTIMIZATION_PLAN.md`
- `P1_MICROKERNEL_CORE_PLAN.md`
- `P2_SERVICE_ENGINEERING_PLAN.md`
- `P2_COMPLETION_REPORT.md`
- `P3_MICROKERNEL_SERVICE_PLAN.md`
- `MICROKERNEL_MIGRATION_PLAN.md`
- `docs/MICROKERNEL_ROADMAP.md`
- `docs/IRQ_DESIGN.md`
- `docs/TIMER_DESIGN.md`
- `docs/DIAGNOSTIC_GUIDE.md`

这些文档记录了从 RTOS 内核到微内核风格系统的阶段性目标、已完成内容和后续迁移路
线。

## 当前限制

需要明确的是，My-RTOS 目前还不是完整意义上的用户态服务微内核：

- VFS、devfs、driver、timer、BH 仍主要运行在内核侧
- 没有 root/init 和 name server
- capability 还不是完整 CSpace/CNode 模型
- cap-bearing blocking IPC 还没有完整 continuation
- channel blocking syscall 仍有保守限制
- 不是所有阻塞 syscall 都已经 sleepable continuation 化
- 用户态服务崩溃后的 supervisor/restart 机制还未完成
- MMIO capability、IRQ notification、shared memory manager 仍待建设

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
- name server
- IRQ-to-endpoint notification
- MMIO capability
- shared memory object
- 用户态 driver server
- 用户态 FS server

长期目标：

- 内核只保留调度、IPC、capability、MPU/address-space、IRQ dispatch、fault handling
- 文件系统、驱动、设备管理、系统策略全部迁到用户态服务
- 形成清晰的 syscall ABI、IPC ABI 和服务协议

## License

MIT
