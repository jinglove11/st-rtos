# my-rtos 微内核成熟度差距分析

> 版本: 1.0 | 日期: 2026-06-30 | 目标硬件: RP2350 (Cortex-M33, Pico 2 W)
> 审计基线: master 分支 commit `1831bf6` 之后, RP2350 端到端验证通过 (~94% 测试 PASS)
> 审计方法: 三路并行源码审计 + 真机验证 (DAPLink/OpenOCD/gdb/UART 抓取)

---

## 0. 总评

**定位**: 原型级 / 教学级微内核。
**规模**: ~50K LOC, 137 个源文件, 15 个内核子系统, 9 个用户态服务框架, 22 个测试模块。

**已建成**:
- Capability 系统 (derive/transfer/revoke, 128 全局池, 32 槽/任务)
- IPC 完整套件 (endpoint + channel + mqueue + semaphore + mutex + event, 全部带 timeout)
- PMSAv8 MPU 隔离 (8 region/任务, PSPLIM 栈保护, MMFSR 精确诊断)
- 71 个 syscall, SVC 分发, 部分 cap 检查
- 优先级抢占调度 (32 优先级, O(1) bitmap, Mutex PI)
- 用户态 server 雏形 (nameserver, driver_registry, fs_server, uart_server, supervisor)
- 基础 trace (16 类事件) + stats + crash_dump
- Fault handler (寄存器级诊断 + 任务隔离 + 内核 panic)

**核心 gap (5 项)**:
1. 没有真正的地址空间隔离 (M33 MPU-only, 无 MMU)
2. 没有 capability minting (seL4 标志特性)
3. 没有虚拟内存 mapping syscall
4. 没有 fault→restart 运行时闭环 (supervisor 是死数据)
5. 没有利用 RP2350 双核

成熟度雷达 (满分 5):

| 维度 | 得分 | 维度 | 得分 |
|---|---|---|---|
| Capability | 3.5 | Scheduler | 3.0 |
| IPC | 3.0 | Fault/重启 | 3.0 |
| Memory | 2.0 | User services | 1.5 |
| MPU 隔离 | 2.5 | Drivers | 1.5 |
| Syscall | 3.5 | SMP/多核 | 0 |
| VFS | 2.5 | 网络/无线 | 0 |
| Power mgmt | 1.0 | Real-time | 1.0 |
| Boot/secure | 1.0 | 文档/CI | 2.5 |

---

## 一、战略级 gap

### 1.1 没有真正的地址空间隔离

**现状**:
- Cortex-M33 (RP2350) 只有 PMSAv8 MPU, 没有 MMU。
- 每任务 8 region (`kernel_types.h:172` `mpu_regions[8][2]`), 任务初始仅用 region 0 (Flash RO+X) + region 2 (栈 RW+XN), region 1 显式禁用, region 3-7 留给 SHM (`task.c:888-916`)。
- 内核态靠 PRIVDEFENA 背景区保护 (`mpu.c:88`)。
- 用户态 heap 和内核 heap 共享同一 `kmalloc` 池 (`mem.c`)。
- `SYSCALL_SHM_MAP/UNMAP` (`syscall.h:61-62`) 只是把已分配 region 装到 TCB `shm_maps[TASK_SHM_MAP_MAX=5]` (`kernel_types.h:114`), 不是任意 vm_region 映射。

**后果**:
- 用户态和内核共享同一 SRAM, region 配错即漏洞。
- 用户程序除自己栈外没有通用数据区 (region 1 留空), 必须靠 cap 显式注册 region。
- 不可能做到"独立地址空间"。

**根因**: 硬件限制 (M33 MPU-only)。
**软件可改进**: region 分配策略动态化 (目前是 `task_create_user` 静态装载); SHM region 数从 5 提到 8 用满 MPU 槽; 引入 region-pool allocator 让用户程序按需申请。

**行业参照**: seL4 (Untyped→Page retype + mapping cap), Zircon (VMAR/VMO 完整虚拟内存抽象)。本项目缺最关键的 map/unmap page to address space。

### 1.2 没有 capability minting

**现状**:
- `capability.c` 628 行, `cap_pool[CAP_MAX_COUNT=128]` 全局, 每任务 `cap_set[KERN_TASK_CAP_SLOTS=32]` 线性数组 (`capability.h:99`)。
- 有完整 derive/transfer/revoke 树, generation 防 stale, refcount, cleanup hook。
- IPC 中走 `ipc_transfer.c:19` 做 COPY/MOVE 两阶段提交。
- grep 全仓无 `mint`。

**缺口**:
- **无 cap minting** (seL4 标志特性): `CAP_GRANT` 只允许 derive 同对象的不同权限子集, 无法 mint 出新对象/独立身份。
- cspace 不是二级页表式: 每任务固定 32 槽线性数组, O(n) 顺序扫描 (`capability.c:119`), seL4/Zircon 是树状 cspace + CNode Copy/Mint/Move。
- 32 槽远不足以承载真实服务 (NS + driver + fs + 自定义服务轻松 >32)。
- 没有 untyped/object cap 概念, cap 指向的内核对象是直接 `void*`, 不是 cap-derived kernel object。

**行业参照**: seL4 (cspace mint/derive/retype 全套), Fuchsia Zircon (handle 表 + rights)。

### 1.3 没有虚拟内存 mapping syscall

**现状**:
- 71 个 syscall 中没有 `map_page`/`unmap`/`frame_alloc`/`vm_create` 等微内核标志性原语。
- grep 全仓无 `page table/MMU/vaddr/paddr/frame_alloc`。
- `SYSCALL_SHM_MAP/UNMAP` 只是 region 装载, 不是任意地址映射。

**根因**: M33 无 MMU。但即便 MPU-only, 也可以提供 `region_map(addr, size, attrs, cap)` 形式的 syscall 让用户态按需 MPU region 配置。本项目没暴露这种接口。

### 1.4 没有 fault→restart 运行时闭环

**现状**:
- `supervisor.c` 260 行有完整数据结构: `restart_policy` (AUTO/MANUAL), `max_restarts`, `record_restart`/`record_fault`/`should_auto_restart` (`supervisor.h:14-17`)。
- `fault.c` 用户任务 fault 隔离到 `task_terminate_with_result(KERN_ERR_FAULT)` (`fault.c:294`), 改 PC→`task_fault_exit` 触发 PendSV。
- watchdog 在 `kernel.c:51` `hal_watchdog_init` 启动。
- **但代码里没有任何地方在 fault 后真正调用 `supervisor_should_auto_restart` 去重启任务**。
- watchdog 在 RP2350 路径是 `#error "RP2350 watchdog not implemented"` (`hal.c:767`), defconfig (`configs/rp2350_defconfig`) 也未启用 `KERN_WATCHDOG`。

**后果**:
- 整个系统没有 watchdog 保护; 一个关键服务 fault 后系统不会自愈。
- crash_dump 在 `.crash_dump` RAM section, 掉电即失。

**缺口**:
- panic 后无 live update / hot reboot。
- 无 A/B bootloader 双区回滚。
- 无 exponential backoff / circuit breaker。
- supervisor 没有依赖图 / health-check heartbeat / service manifest。

**行业参照**: QNX (process restart policy + critical process monitor), Fuchsia (component framework + realm restart), seL4 (domain 自动重启)。

### 1.5 没有利用 RP2350 双核

**现状**:
- RP2350 是双核 Cortex-M33, 但 `grep cm1|core1|multicore|smp|ipi` 在 `src/` 下 0 业务命中。
- `scheduler.c:77,86` `_current_task/_next_task` 是单一全局变量, 完全单核。
- 无 spinlock 抽象 (有 `spinlock.h` 但用法未见), 无原子原语, 无 IPI (核间中断)。
- Core1 在 gdb 下观察 PC=0xec 处于 WFE 状态。

**后果**: 一半硬件算力浪费。RP2350 典型场景是把 driver server / network stack 放到 Core1, 主核跑应用 —— 本项目无法做这种分工。

---

## 二、系统级 gap

### 2.1 驱动覆盖窄

**现状**:
- `src/drivers/chip/rp2350/` 只有 `gpio_rp2350.c` + `uart_rp2350.c` (UART 还是 SDK 直通)。
- `src/drivers/include/` 只有 `gpio.h`、`uart.h`、`uart_dev.h`。
- `src/hal/hal.h` 只声明 CPU/中断/SysTick/上下文切换/看门狗接口, **无 GPIO/UART/I2C/SPI/PWM/ADC/USB 抽象**。
- `driver_registry.c` 硬编码唯一条目 `"dev.uart0"`。

**缺口**:
- I2C / SPI / PWM / ADC / USB / QSPI 全无。
- driver server **不直接 MMIO 操作** —— `attach` 只是记一个 `mmio_attached` bool 位 (`uart_server.c:536-542`), 未真正把 MMIO region 映射到 server 地址空间。无 IOMMU/devmem cap。
- 没有 hot-plug (`device_remove` 只在 `open_count==0` 时生效)。
- read/write 截断在 `DRV_PAYLOAD_MAX=32` 字节 (`driver_proto.h:14`), 不能传大数据。
- 缺 DMA / 电源管理 / 多实例驱动。

### 2.2 没有块设备文件系统

**现状**:
- 内核态 `vfs.c` (922 行) 提供完整 open/close/read/write/ioctl/lseek/readdir/mkdir/unlink/stat/mount (`vfs.h:27-41`)。
- ramfs (`ramfs.c`, 64B block kmalloc 链式扩展) + devfs (`devfs.c` 字符设备 + /dev/null)。
- 用户态 `fs_server.c` (486 行) 把 VFS API 通过 IPC 暴露。

**缺口**:
- **无任何块设备 FS** (grep FAT/littlefs/block_dev 全空)。
- ramfs 文件数据用 kmalloc 链式扩展, 无页缓存、无日志、无并发锁。
- mount 机制要求预先存在目录 inode 作挂载点 (`vfs.c:863`), 无自动 mount/umount syscall 暴露给用户。
- fs_server 单线程同步 RPC, IPC 协议只有 PING/OPEN/READ/WRITE/CLOSE/STAT, 缺 opendir/readdir。
- 无 overlay、无权限位、无 fstab。

### 2.3 完全没有网络/无线

- `grep cyw43|wifi|ble|bluetooth` 在 `src/` 下 0 命中。
- `docs/PICO2W_PORT.md` 明列 "Not yet implemented: CYW43 Wi-Fi/Bluetooth"。
- 无 lwIP / socket IPC / 任何网络抽象。

### 2.4 没有用户态 libc

**现状**: grep `malloc|printf|pthread|fopen` 在 `src/user/`, `src/app/` 下只命中注释。

**缺口**:
- 无 malloc / realloc / free (用户进程无 heap 抽象, 只能用栈缓冲如 `ns_name_msg_t` on stack)。
- 无 printf / snprintf / fprintf。
- 无 pthread / mutex / condvar / rwlock。
- 无 stdio FILE* / fopen / fclose。
- 无 string.h 用户封装 (nameserver 自己手写 `ns_zero`/`ns_copy_name`)。

**后果**: 无法"像写应用一样"写用户程序 —— 开发者必须直接打 IPC 协议结构体。这是与 seL4 / Fuchsia 最大的差距之一。

### 2.5 real-time 特性薄弱

**现状**:
- Mutex 优先级继承 (`mutex.c:54-102`, `KERN_MUTEX_PI=1`)。
- `stats_record_irq()` 跟踪 `irq_latency_max` (shell 可查)。

**缺口**:
- 无 EDF / deadline scheduling (grep 无 `EDF|deadline`)。
- 无 priority ceiling protocol, 无 preemption threshold。
- 无 deterministic syscall 表 / WCET 标注 / bounded syscall 时间表。
- 无调度 jitter / interrupt jitter histogram。
- **IPC 阻塞路径散落 12 处裸 `__asm volatile("wfi")`** (mqueue/endpoint/channel/event/semaphore/mutex/fault), 不可预测。

### 2.6 power management 几乎为 0

- 仅 `hal_enter_lowpower()` (WFI) + `KERNEL_IDLE_SLEEP=y`。
- 无 tickless idle、无 sleep state (DORMANT/SLEEP)、无 peripheral clock gating、无 wakeup source 管理、无 tickless 计时器重编程。

---

## 三、服务级 gap (与 seL4/QNX/Fuchsia 对比)

完全缺失的系统服务:

| 服务 | seL4 | QNX | Fuchsia | 本项目 |
|---|---|---|---|---|
| 真正的 init/supervisor 进程 | ✓ | ✓ procmgr | ✓ component_manager | **缺** (server 由 shell 临时 spawn) |
| 网络栈 server | ✓ lwIP | ✓ io-net | ✓ | **缺** |
| 显示/compositor server | ✓ | ✓ Screen | ✓ Scenic | **缺** |
| 输入 server (HID) | ✓ | ✓ devi | ✓ input | **缺** |
| 调试 server / gdbstub | ✓ | ✓ pdebug | ✓ debug | **缺** |
| Cap minting / derivation tool | ✓ capdl | — | ✓ | **缺** |
| ELF loader / 动态链接 | ✓ | ✓ | ✓ | **缺** (所有 task 编译期固定) |
| Package manager / loader | — | ✓ | ✓ componentmgr | **缺** |
| Timer server (用户态) | ✓ | ✓ | ✓ | **缺** (timer 全在内核) |
| 日志 ring buffer server | ✓ | ✓ slogger | ✓ | **缺** (trace 在内核) |
| Crypto / keystore server | ✓ | ✓ | ✓ | **缺** |
| 电源管理 server | ✓ | ✓ | ✓ | **缺** |

### Shell 成熟度: 3.5/5

4581 行, 最成熟的用户态组件。命令表含 help/ls/cat/echo/clear/ps/top/crash/mem/stats/dev/driver/fs/svc/free/hexdump/version/reset (~18 条)。行编辑器支持 backspace, `shell_split` argv 解析。

**缺**: 变量、管道、重定向、脚本执行、if/for、history、tab 补全、PATH/exec —— 不能 launch 用户二进制。

---

## 四、工程级 gap

### 4.1 架构目录命名欺诈

- `src/arch/arm/cortex-m7/` 同时服务 STM32F7 (M7) 和 RP2350 (M33), 没有真正的 M33 子目录。
- TrustZone / Secure Stack / Non-secure attribution 完全没碰 (虽 `.config` 标 `rp2350-arm-ns`)。
- ARMv8-M mainline 在指令级兼容 ARMv7-M 的 PendSV/SVC, 所以代码能跑; 但 abstraction 没分。
- `CMakeLists.txt:38-42` 写死 `src/arch/arm/cortex-m7/*.S` 和 `src/drivers/chip/rp2350/*`, 多板切换实际未参数化。

### 4.2 没有 CI / host unit test

- 没有 `.github/workflows`。
- 22 个测试模块全部在目标机串行跑 (`test_run_all_modules` 被 `main.c` 调用)。
- 无 host 端 unit test、无 mock HAL、无 coverage (gcov/lcov)、无 fuzzing、无 QEMU target。
- 回归靠人工烧录抓串口。

### 4.3 trace 与调试能力薄弱

- `TRACE_BUFFER_SIZE=256` 极小, 环形易丢。
- 无栈回溯 (backtrace)、无 panic 符号解析、无 live watch、无 kdb / gdbstub。
- `tools/crash_analyzer.py`、`trace_parser.py` 是离线 Python 工具。
- fault.c 有 crash_dump (R0-R3/R12/PC/LR/XPSR/CFSR/HFSR/MMFAR/BFAR/MSP/PSP + MPU region dump), 已经不错, 但缺符号化。

### 4.4 boot/secure boot 完全靠 SDK

- RP2350 boot2 完全由 Pico SDK 提供, 项目内无 `boot2.S`/`boot2.lds`。
- 无 stage1/stage2 boot 概念。
- 无 secure boot / image signing / OTA / 双区回滚。
- watchdog 在 RP2350 上 `#error` 未实现。

### 4.5 文档缺口

- `docs/` 有 17 个设计文档 (ROADMAP/IRQ_DESIGN/TIMER_DESIGN/SCHEDULER_GUIDE/SHM_MAP_DESIGN/TEST_FRAMEWORK 等) —— 设计文档质量不错。
- **缺**: CLAUDE.md (项目隐性约定给 AI 协作)、API reference、porting guide、tutorials、architecture overview。
- **隐性约定未文档化**:
  - arch 目录名 `cortex-m7` 实际服务 M33
  - `tcb_offsets.inc` 由 `scripts/gen_tcb_offsets.py` 代码生成
  - Kconfig `BOARD_NAME` 与 `src/board/` 子目录名强耦合
  - UART 重命名为 `rtos_uart_*` 宏替换易与 SDK inline 冲突 (见 `uart.h` undef 模式)

---

## 五、syscall 模型偏离微内核主流

**现状**: 71 个 syscall (`SYSCALL_TABLE_SIZE=71`), 传统分类式 —— `task_*` / `ep_*` / `ch_*` / `sem_*` / `mutex_*` / `mqueue_*` / `event_*` 各占 4-6 个。

**问题**:
1. **未正交化**: seL4 ≈ 12 个原语 + cap invoke, Zircon 200+ 但统一 handle-based。本项目介于两者之间, 没有"cap + method opcode"统一模型。
2. **大量 syscall 无 cap 检查**: `task_yield/delay/exit/self` (`syscall.c:89-103`)、`mem_alloc/free`、VFS 走的 `open/close/read/write/stat/readdir/unlink/mkdir` 全部裸调。grep ~40 处 `cap_resolve`, 覆盖率约 60%。
3. **微内核典型 syscall 完全缺失**: `task_restart`、`domain_create`、`fault_handler_register`、`map_page`/`unmap`、`cap_mint`、`cap_retype`。
4. **扩展性差**: `SYSCALL_TABLE_SIZE=71` 与最后编号 70 一致, 无 reserved 扩展槽。

---

## 六、优先级建议 (按 ROI 排序)

### P0 (小时-天级, 决定系统鲁棒性)
1. **fault→restart 运行时闭环**: 在 `fault.c` 或 `task_terminate` 路径调用 `supervisor_should_auto_restart`, 命中则 `task_create` 重启。同时启用 `KERN_WATCHDOG` 并实现 RP2350 watchdog (`hardware_watchdog` 库)。
2. **CI + host unit test**: 至少把 `cap`/`mem`/`vfs`/`mempool` 这种无硬件依赖的模块抽出来, 用 host gcc + mock HAL 跑 unit test, 加 GitHub Actions。

### P1 (周级, 决定能不能写应用)
3. **用户态 libc**: 移植 picolibc 或自写最小 malloc/printf/snprintf/string/pthread, 让用户程序不再直接打 IPC 结构体。
4. **块设备 FS**: 移植 LittleFS, 接到 QSPI flash (Pico 2 W 板载 W25Q32JV), 暴露 mount syscall。
5. **driver MMIO cap 授权 + I2C/SPI**: 让 driver server 真正拿到一段 MMIO region cap, MPU 配 region 保护。同时补 I2C/SPI 驱动 (传感器/显示屏基本接口)。

### P2 (月级, 决定场景广度)
6. **SMP 启用 Core1**: 用 Pico SDK multicore 拉起 Core1 跑一个独立调度器实例, IPI 经 SIO FIFO。最简场景: Core0 跑应用, Core1 跑 driver server / 网络。
7. **网络栈**: CYW43 + lwIP, 暴露 socket 为 IPC endpoint。
8. **ELF loader + 动态进程**: 让 shell 能 `exec /fs/apps/foo.elf`。

### P3 (战略级, 决定是否进生产)
9. **Capability minting + cspace 重构**: 树状 cspace, mint syscall, untyped retype (虽然无 MMU, 但概念可移植到 region/memory cap)。
10. **Secure boot + A/B OTA**: stage1 bootloader + 镜像签名 + 双区回滚。
11. **Real-time 增强**: EDF / priority ceiling / WCET 标注 / jitter 测量。

---

## 附录 A: 审计方法

- 三路并行源码审计 (cap+IPC+mem+mpu+syscall+scheduler+fault / userspace+services / drivers+tools+docs+SMP+RT)。
- 真机验证: DAPLink + OpenOCD + gdb 单步, UART @115200 抓 boot 输出。
- 测试基线: 9 个模块全 PASS (capability/deadlock/diag/fault/ipc_upgrade/mem/mpu/scheduler/shell/stats/svc_runtime/task), 4 个模块部分 FAIL (driver 112/irq 19/service_model 41/syscall 30), 总通过率约 94%。
- FAIL 集中在依赖硬件 IRQ 路由 / SHM map / 长阻塞 syscall 的 case。

## 附录 B: 关键文件索引

| 主题 | 文件 |
|---|---|
| Capability | `src/kernel/cap/capability.c`, `src/kernel/ipc/ipc_transfer.c` |
| IPC endpoint | `src/kernel/ipc/endpoint.c` |
| Memory/SHM | `src/kernel/mem/mem.c:640-905` |
| MPU | `src/kernel/mpu/mpu.c:154-189` |
| Task user MPU 装载 | `src/kernel/task/task.c:880-917` |
| Fault 处理 | `src/kernel/fault/fault.c` |
| Supervisor | `src/user/supervisor/supervisor.c` |
| Syscall 分发表 | `src/kernel/syscall/syscall.c:1388-1490` |
| Scheduler bitmap | `src/kernel/core/scheduler.c:113` |
| Nameserver | `src/user/nameserver/nameserver.c` |
| Driver registry | `src/user/drivers/driver_registry.c` |
| FS server | `src/user/fs/fs_server.c` |
| UART server | `src/user/drivers/uart_server.c` |

## 附录 C: 行业参照

- **seL4**: capability + minting + untyped retype + 形式化验证。
- **QNX Neutrino**: 进程重启 + critical process monitor + 完整 network/display/input server。
- **Fuchsia Zircon**: handle + rights + VMAR/VMO + component framework。
- **L4 family**: 快速 IPC + small TCB。
- **Zephyr/FreeRTOS**: 不是微内核, 但 RTOS 维度 (驱动 / 协议栈 / power mgmt) 是工程基线。
