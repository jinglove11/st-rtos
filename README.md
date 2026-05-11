# My-RTOS

A lightweight real-time operating system for ARM Cortex-M7 (STM32F767 Nucleo-144).

## Features

- **Preemptive priority scheduler** with O(1) bitmap-based priority lookup (32 levels)
- **Time-slice round-robin** for same-priority tasks
- **MPU memory protection** — user/kernel task isolation, stack guard regions
- **Syscall interface** — user tasks access kernel services via SVC
- **Fault handling** — MemManage/BusFault/UsageFault/HardFault with crash dump and task termination
- **IPC**: semaphore, mutex (with deadlock detection + priority inheritance), message queue, event flags, endpoint, channel
- **Software timers** — one-shot and periodic, min-heap based
- **Threaded IRQ + bottom-half** deferred processing
- **VFS layer** — ramfs, devfs, inode cache
- **Capability system** — fine-grained access control
- **Trace buffer** — event logging with filtering
- **CPU usage statistics** — per-task tick accounting
- **Stack overflow detection** — magic-byte guard
- **Interactive shell** over UART: `ps`, `top`, `free`, `mem`, `ls`, `cat`, `crash`, `trace`, `hexdump`, `stats`
- **659-test suite** with modular registration

## Hardware

| Component    | Detail                        |
|-------------|-------------------------------|
| MCU         | STM32F767ZI                   |
| Board       | Nucleo-F767ZI (Nucleo-144)    |
| Core        | ARM Cortex-M7                 |
| Clock       | 48 MHz (PLL from HSI 16 MHz) |
| UART        | USART3 (PD8/PD9, 115200 8N1) |
| Toolchain   | arm-none-eabi-gcc 14.3        |

## Project Structure

```
my-rtos/
├── src/
│   ├── app/                  # Application (shell)
│   ├── arch/arm/cortex-m7/   # Arch-specific (PendSV, SVC, first_switch, HAL)
│   ├── board/stm32f767/      # Board definitions & drivers
│   ├── drivers/              # UART, GPIO, device model
│   ├── hal/                  # HAL interface
│   ├── kernel/
│   │   ├── core/             # Scheduler
│   │   ├── task/             # Task management
│   │   ├── ipc/              # Semaphore, mutex, mqueue, event, endpoint, channel
│   │   ├── timer/            # Software timers
│   │   ├── irq/              # IRQ, bottom-half
│   │   ├── mpu/              # MPU memory protection
│   │   ├── syscall/          # Syscall dispatch
│   │   ├── cap/              # Capability tokens
│   │   ├── fault/            # Fault handlers & crash dump
│   │   ├── mem/              # Heap allocator & mempool
│   │   ├── vfs/              # Virtual filesystem (ramfs, devfs)
│   │   ├── trace/            # Event tracing
│   │   ├── stats/            # CPU usage & kernel statistics
│   │   ├── dev/              # Device framework
│   │   └── include/          # Kernel config & types
│   ├── startup/arm/cortex-m7/ # Vectors, reset handler, fault handlers, SystemInit
│   └── tests/                # 659-test suite (17 modules)
├── configs/                  # Board defconfigs
├── link/                     # Linker script
├── tools/                    # ARM GCC toolchain
├── Makefile
└── README.md
```

## Build & Flash

```bash
make BOARD=stm32f767          # Build
make BOARD=stm32f767 flash    # Build & flash via OpenOCD
make BOARD=stm32f767 clean    # Clean
```

## Shell Commands

| Command    | Description              |
|-----------|--------------------------|
| `help`    | List all commands        |
| `ps`      | List tasks & stack usage |
| `top`     | Task CPU usage           |
| `free`    | Heap memory usage        |
| `mem`     | Per-task memory layout   |
| `ls`      | List VFS directory       |
| `cat`     | Read VFS file            |
| `echo`    | Print text               |
| `crash`   | Show last crash dump     |
| `stats`   | Kernel statistics        |
| `trace`   | Event trace buffer       |
| `hexdump` | Hex dump memory          |
| `version` | Kernel version           |
| `reset`   | System reset             |
| `clear`   | Clear screen             |

## Configuration

Edit `configs/stm32f767_defconfig` then rebuild. Key options:

- `KERNEL_MAX_TASKS` (default 16)
- `KERNEL_MAX_PRIORITIES` (default 32)
- `KERNEL_TICK_RATE` (default 1000 Hz)
- `MPU_ENABLE` — memory protection
- `SYSCALL_ENABLE` — user-mode syscalls
- `KERN_TASK_STATS` — CPU usage tracking

## License

MIT
