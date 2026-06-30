# Kconfig Index

Auto-generated from [`Kconfig`](../../Kconfig) by [`scripts/gen_kconfig_docs.py`](../../scripts/gen_kconfig_docs.py).

**123 options** across **23** menu groups.

To refresh this page after editing Kconfig:

```bash
python3 scripts/gen_kconfig_docs.py
```

## Capability Configuration

| Option | Type | Default | Description |
|---|---|---|---|
| [`CAP_ENABLE`](CAP_ENABLE.md) | `bool` | `y` | Enable capability system |
| [`CAP_MAX_COUNT`](CAP_MAX_COUNT.md) | `int` | `32` | Maximum capability entries |
| [`KERN_TASK_CAP_SLOTS`](KERN_TASK_CAP_SLOTS.md) | `int` | `32` | Capability slots per task |

## Debug Configuration

| Option | Type | Default | Description |
|---|---|---|---|
| [`ASSERT_ENABLE`](ASSERT_ENABLE.md) | `bool` | `y` | Enable assertions |
| [`DEBUG_ENABLE`](DEBUG_ENABLE.md) | `bool` | `y` | Enable debug output |
| [`DEBUG_LEVEL`](DEBUG_LEVEL.md) | `int` | `2` | Debug level |
| [`DEBUG_STACK_CHECK`](DEBUG_STACK_CHECK.md) | `bool` | `y` | Enable stack overflow check |

## Driver Configuration

| Option | Type | Default | Description |
|---|---|---|---|
| [`DRIVER_ENABLE`](DRIVER_ENABLE.md) | `bool` | `y` | Enable device driver framework |
| [`DRIVER_MAX_DEVICES`](DRIVER_MAX_DEVICES.md) | `int` | `8` | Maximum devices |

## Fault Handler Configuration

| Option | Type | Default | Description |
|---|---|---|---|
| [`FAULT_CRASH_DUMP`](FAULT_CRASH_DUMP.md) | `bool` | `y` | Enable crash dump on fault |
| [`FAULT_ENABLE`](FAULT_ENABLE.md) | `bool` | `y` | Enable fault handlers |

## General Setup

| Option | Type | Default | Description |
|---|---|---|---|
| [`PROJECT_NAME`](PROJECT_NAME.md) | `string` | `"my-rtos"` | Project name |
| [`PROJECT_VERSION`](PROJECT_VERSION.md) | `string` | `"1.0.0"` | Project version |

## IPC Configuration

| Option | Type | Default | Description |
|---|---|---|---|
| [`IPC_CHANNEL`](IPC_CHANNEL.md) | `bool` | `y` | Enable channel (P2P messaging) |
| [`IPC_CHANNEL_MAX`](IPC_CHANNEL_MAX.md) | `int` | `4` | Maximum channels |
| [`IPC_CH_MSG_SIZE`](IPC_CH_MSG_SIZE.md) | `int` | `64` | Channel message size (bytes) |
| [`IPC_ENDPOINT`](IPC_ENDPOINT.md) | `bool` | `y` | Enable endpoint (C/S messaging) |
| [`IPC_ENDPOINT_MAX`](IPC_ENDPOINT_MAX.md) | `int` | `4` | Maximum endpoints |
| [`IPC_EP_MAX_PENDING`](IPC_EP_MAX_PENDING.md) | `int` | `4` | Max pending messages per endpoint |
| [`IPC_EP_MSG_SIZE`](IPC_EP_MSG_SIZE.md) | `int` | `64` | Endpoint message size (bytes) |
| [`IPC_EVENT`](IPC_EVENT.md) | `bool` | `y` | Enable event flags |
| [`IPC_EVENT_MAX`](IPC_EVENT_MAX.md) | `int` | `4` | Maximum event groups |
| [`IPC_MQUEUE`](IPC_MQUEUE.md) | `bool` | `y` | Enable message queue |
| [`IPC_MQUEUE_MAX`](IPC_MQUEUE_MAX.md) | `int` | `4` | Maximum message queues |
| [`IPC_MUTEX`](IPC_MUTEX.md) | `bool` | `y` | Enable mutex |
| [`IPC_MUTEX_MAX`](IPC_MUTEX_MAX.md) | `int` | `8` | Maximum mutexes |
| [`IPC_SEMAPHORE`](IPC_SEMAPHORE.md) | `bool` | `y` | Enable semaphore |
| [`IPC_SEMAPHORE_MAX`](IPC_SEMAPHORE_MAX.md) | `int` | `8` | Maximum semaphores |
| [`MUTEX_DEADLOCK_DETECT`](MUTEX_DEADLOCK_DETECT.md) | `bool` | `y` | Enable deadlock detection |

## Interrupt Configuration

| Option | Type | Default | Description |
|---|---|---|---|
| [`IRQ_BH_ENABLE`](IRQ_BH_ENABLE.md) | `bool` | `y` | Enable bottom halves |
| [`IRQ_BH_MAX`](IRQ_BH_MAX.md) | `int` | `8` | Maximum bottom halves |
| [`IRQ_DEFAULT_PRIORITY`](IRQ_DEFAULT_PRIORITY.md) | `int` | `8` | Default ISR priority |
| [`IRQ_ENABLE`](IRQ_ENABLE.md) | `bool` | `y` | Enable interrupt management |
| [`IRQ_MAX_USER`](IRQ_MAX_USER.md) | `int` | `16` | Maximum registered ISRs |
| [`IRQ_THREADED_ENABLE`](IRQ_THREADED_ENABLE.md) | `bool` | `y` | Enable threaded IRQs |
| [`IRQ_THREADED_MAX`](IRQ_THREADED_MAX.md) | `int` | `4` | Maximum threaded IRQs |
| [`IRQ_THREADED_STACK_SIZE`](IRQ_THREADED_STACK_SIZE.md) | `int` | `512` | Threaded IRQ handler stack size (bytes) |

## Kernel Configuration

| Option | Type | Default | Description |
|---|---|---|---|
| [`KERNEL_IDLE_PRIORITY`](KERNEL_IDLE_PRIORITY.md) | `int` | `31` | Idle task priority |
| [`KERNEL_IDLE_SLEEP`](KERNEL_IDLE_SLEEP.md) | `bool` | `y` | CPU sleep in idle task |
| [`KERNEL_IDLE_STACK_SIZE`](KERNEL_IDLE_STACK_SIZE.md) | `int` | `256` | Idle task stack size (bytes) |
| [`KERNEL_MAX_PRIORITIES`](KERNEL_MAX_PRIORITIES.md) | `int` | `32` | Number of priority levels |
| [`KERNEL_MAX_TASKS`](KERNEL_MAX_TASKS.md) | `int` | `16` | Maximum number of tasks |
| [`KERNEL_TASK_STACK_SIZE`](KERNEL_TASK_STACK_SIZE.md) | `int` | `1024` | Default task stack size (bytes) |
| [`KERNEL_TICK_RATE`](KERNEL_TICK_RATE.md) | `int` | `1000` | System tick rate (Hz) |
| [`KERNEL_WATCHDOG`](KERNEL_WATCHDOG.md) | `bool` | `n` | Enable watchdog |
| [`KERNEL_WATCHDOG_TIMEOUT`](KERNEL_WATCHDOG_TIMEOUT.md) | `int` | `1000` | Watchdog timeout (ms) |
| [`KERN_DEFAULT_TIME_SLICE`](KERN_DEFAULT_TIME_SLICE.md) | `int` | `5` | Time slice (ticks) |
| [`KERN_NAME`](KERN_NAME.md) | `string` | `"My-RTOS"` | Kernel name |
| [`KERN_TASK_NAME_LEN`](KERN_TASK_NAME_LEN.md) | `int` | `16` | Task name length |
| [`KERN_TASK_STATS`](KERN_TASK_STATS.md) | `bool` | `y` | Enable task CPU usage statistics |
| [`KERN_VERSION_MAJOR`](KERN_VERSION_MAJOR.md) | `int` | `1` | Version major |
| [`KERN_VERSION_MINOR`](KERN_VERSION_MINOR.md) | `int` | `0` | Version minor |
| [`KERN_VERSION_PATCH`](KERN_VERSION_PATCH.md) | `int` | `0` | Version patch |
| [`TRACE_BUFFER_SIZE`](TRACE_BUFFER_SIZE.md) | `int` | `128` | Trace buffer size |
| [`TRACE_ENABLE`](TRACE_ENABLE.md) | `bool` | `y` | Enable kernel event tracing |

## Memory Configuration

| Option | Type | Default | Description |
|---|---|---|---|
| [`MEM_DYNAMIC`](MEM_DYNAMIC.md) | `bool` | `y` | Enable dynamic memory allocation |
| [`MEM_HEAP_SIZE`](MEM_HEAP_SIZE.md) | `int` | `4096` | Heap size (bytes) |
| [`MEM_POOL_COUNT`](MEM_POOL_COUNT.md) | `int` | `4` | Memory pool count |

## Microkernel Configuration

| Option | Type | Default | Description |
|---|---|---|---|
| [`MPU_ENABLE`](MPU_ENABLE.md) | `bool` | `y` | Enable MPU memory protection |
| [`MPU_REGION_COUNT`](MPU_REGION_COUNT.md) | `int` | `5` | MPU regions per task |
| [`SYSCALL_ENABLE`](SYSCALL_ENABLE.md) | `bool` | `y` | Enable syscall interface |
| [`SYSCALL_TABLE_SIZE`](SYSCALL_TABLE_SIZE.md) | `int` | `71` | Syscall table size |

## Phase 2 — Fault-Tolerant Infrastructure

| Option | Type | Default | Description |
|---|---|---|---|
| [`CAP_RESTART_SUBSET`](CAP_RESTART_SUBSET.md) | `bool` | `n` | Capability subset on restart (drop CAP_GRANT) |
| [`FAULT_ENDPOINT`](FAULT_ENDPOINT.md) | `bool` | `n` | Fault reporting endpoint |
| [`INIT_PROCESS`](INIT_PROCESS.md) | `bool` | `n` | User-mode init task |
| [`SUPERVISOR`](SUPERVISOR.md) | `bool` | `n` | User-mode supervisor task |

## Phase 3 — Driver Servers

| Option | Type | Default | Description |
|---|---|---|---|
| [`BLOCK_DEVICE`](BLOCK_DEVICE.md) | `bool` | `n` | Block device abstraction |
| [`DRIVER_GPIO_SERVER`](DRIVER_GPIO_SERVER.md) | `bool` | `n` | GPIO server (user-mode) |
| [`DRIVER_I2C_SERVER`](DRIVER_I2C_SERVER.md) | `bool` | `n` | I2C server (user-mode) |
| [`DRIVER_RTC`](DRIVER_RTC.md) | `bool` | `n` | RTC + wall clock driver |
| [`DRIVER_SPI`](DRIVER_SPI.md) | `bool` | `n` | SPI bus driver |

## Phase 4 — Persistent Filesystem

| Option | Type | Default | Description |
|---|---|---|---|
| [`FS_LITTLEFS`](FS_LITTLEFS.md) | `bool` | `n` | Build littlefs third-party library |
| [`FS_PERSISTENT`](FS_PERSISTENT.md) | `bool` | `n` | Persistent filesystem (littlefs) |

## Phase 5 — Application Runtime

| Option | Type | Default | Description |
|---|---|---|---|
| [`ELF_LOADER`](ELF_LOADER.md) | `bool` | `n` | ELF loader (sys_proc_exec) |
| [`USER_LIBC`](USER_LIBC.md) | `bool` | `n` | User-mode libc (printf/malloc/string) |

## Phase 6 — SMP (Cortex-M33 Core 1)

| Option | Type | Default | Description |
|---|---|---|---|
| [`CAP_RCU`](CAP_RCU.md) | `bool` | `n` | Capability table RCU reader |
| [`SMP`](SMP.md) | `bool` | `n` | Symmetric multiprocessing (Core 0 + Core 1) |
| [`SMP_MAX_CPUS`](SMP_MAX_CPUS.md) | `int` | `2` | Maximum CPU count |
| [`SMP_WORK_STEALING`](SMP_WORK_STEALING.md) | `bool` | `y` | Idle CPU steals work from busy CPU |

## Phase 7 — Networking

| Option | Type | Default | Description |
|---|---|---|---|
| [`DRIVER_CYW43`](DRIVER_CYW43.md) | `bool` | `n` | CYW43 WiFi bus driver |
| [`MBEDTLS`](MBEDTLS.md) | `bool` | `n` | mbedTLS for HTTPS |
| [`NET`](NET.md) | `bool` | `n` | Network stack (lwIP + socket syscalls) |
| [`NET_DHCP`](NET_DHCP.md) | `bool` | `y` | DHCP client |
| [`NET_DNS`](NET_DNS.md) | `bool` | `y` | DNS resolver |

## Phase 8 — Security & Reliability

| Option | Type | Default | Description |
|---|---|---|---|
| [`OTA`](OTA.md) | `bool` | `n` | Over-the-air update (A/B partition) |
| [`PANIC_LOG`](PANIC_LOG.md) | `bool` | `n` | Panic dump to /flash/panic.log |
| [`PROFILER`](PROFILER.md) | `bool` | `n` | PC sampler + flamegraph export |
| [`SECURE_BOOT`](SECURE_BOOT.md) | `bool` | `n` | Signed boot image (Ed25519) |

## Phase 9 — Comprehensiveness

| Option | Type | Default | Description |
|---|---|---|---|
| [`DYNAMIC_LINKING`](DYNAMIC_LINKING.md) | `bool` | `n` | Dynamic linking (.so + dlopen) |
| [`FB`](FB.md) | `bool` | `n` | Framebuffer / GUI subsystem |
| [`FV`](FV.md) | `bool` | `n` | Formal verification runtime hooks |
| [`PM`](PM.md) | `bool` | `n` | Power management (sleep/deep-sleep/wakeup) |
| [`RT_SCHED`](RT_SCHED.md) | `bool` | `n` | Real-time scheduling classes (FIFO/RR) |

## Shell Configuration

| Option | Type | Default | Description |
|---|---|---|---|
| [`SHELL_ENABLE`](SHELL_ENABLE.md) | `bool` | `y` | Enable interactive shell |
| [`SHELL_PRIORITY`](SHELL_PRIORITY.md) | `int` | `5` | Shell task priority |
| [`SHELL_STACK_SIZE`](SHELL_STACK_SIZE.md) | `int` | `2048` | Shell task stack size (bytes) |

## Target Board

| Option | Type | Default | Description |
|---|---|---|---|
| [`BOARD_DISPLAY_NAME`](BOARD_DISPLAY_NAME.md) | `string` | `"Pico` | Board display name |
| [`BOARD_NAME`](BOARD_NAME.md) | `string` | `"rp2350"` |  |
| [`BOARD_RP2350`](BOARD_RP2350.md) | `bool` | `—` | RP2350 (Raspberry Pi Pico 2) |
| [`BOARD_STM32F767`](BOARD_STM32F767.md) | `bool` | `—` | STM32F767ZI (Nucleo-F767ZI) |

## Test Configuration

| Option | Type | Default | Description |
|---|---|---|---|
| [`TEST_ENABLE`](TEST_ENABLE.md) | `bool` | `y` | Enable test framework |
| [`TEST_MODULE_CAP`](TEST_MODULE_CAP.md) | `bool` | `y` | Capability system tests |
| [`TEST_MODULE_DEADLOCK`](TEST_MODULE_DEADLOCK.md) | `bool` | `y` | Deadlock detection tests |
| [`TEST_MODULE_EXAMPLE`](TEST_MODULE_EXAMPLE.md) | `bool` | `n` | Example tests |
| [`TEST_MODULE_FAULT`](TEST_MODULE_FAULT.md) | `bool` | `y` | Fault handler tests |
| [`TEST_MODULE_IPC_UPGRADE`](TEST_MODULE_IPC_UPGRADE.md) | `bool` | `y` | IPC upgrade tests |
| [`TEST_MODULE_IRQ`](TEST_MODULE_IRQ.md) | `bool` | `y` | Interrupt management tests |
| [`TEST_MODULE_MPU`](TEST_MODULE_MPU.md) | `bool` | `y` | MPU protection tests |
| [`TEST_MODULE_SCHEDULER`](TEST_MODULE_SCHEDULER.md) | `bool` | `y` | Scheduler tests |
| [`TEST_MODULE_STATS`](TEST_MODULE_STATS.md) | `bool` | `y` | CPU statistics tests |
| [`TEST_MODULE_SYSCALL`](TEST_MODULE_SYSCALL.md) | `bool` | `y` | Syscall interface tests |
| [`TEST_MODULE_TIMER`](TEST_MODULE_TIMER.md) | `bool` | `y` | Timer tests |
| [`TEST_MODULE_VFS`](TEST_MODULE_VFS.md) | `bool` | `y` | VFS tests |
| [`TEST_MODULE_WATCHDOG`](TEST_MODULE_WATCHDOG.md) | `bool` | `y` | Watchdog tests |

## Timer Configuration

| Option | Type | Default | Description |
|---|---|---|---|
| [`TIMER_CMD_QUEUE_SIZE`](TIMER_CMD_QUEUE_SIZE.md) | `int` | `8` | Timer command queue size |
| [`TIMER_ENABLE`](TIMER_ENABLE.md) | `bool` | `y` | Enable software timer |
| [`TIMER_MAX`](TIMER_MAX.md) | `int` | `16` | Maximum timers |
| [`TIMER_NAME_LEN`](TIMER_NAME_LEN.md) | `int` | `16` | Timer name length |
| [`TIMER_TASK_PRIORITY`](TIMER_TASK_PRIORITY.md) | `int` | `1` | Timer service task priority |
| [`TIMER_TASK_STACK_SIZE`](TIMER_TASK_STACK_SIZE.md) | `int` | `512` | Timer service task stack size (bytes) |

## VFS Configuration

| Option | Type | Default | Description |
|---|---|---|---|
| [`VFS_ENABLE`](VFS_ENABLE.md) | `bool` | `y` | Enable Virtual File System |
| [`VFS_MAX_FDS`](VFS_MAX_FDS.md) | `int` | `8` | Maximum file descriptors per task |
| [`VFS_MAX_INODES`](VFS_MAX_INODES.md) | `int` | `32` | Maximum inodes |

## Menu presets

| Preset | File | Use case |
|---|---|---|
| Tiny | [`configs/tiny_defconfig`](../../configs/tiny_defconfig) | <32KB flash, single-purpose |
| Default | [`configs/default_defconfig`](../../configs/default_defconfig) | Dev/test baseline (2867/2867 PASS) |
| Release | [`configs/release_defconfig`](../../configs/release_defconfig) | Production (no tests/shell) |
| Full | [`configs/full_defconfig`](../../configs/full_defconfig) | Every subsystem on (compile-test) |
