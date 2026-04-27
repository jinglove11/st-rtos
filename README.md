# My-RTOS

A simple RTOS for RP2350 (Raspberry Pi Pico 2).

## Project Structure

```
my-rtos/
├── src/
│   ├── start/          # 启动代码
│   │   ├── boot2.S     # Stage2 bootloader
│   │   └── startup.c   # 初始化代码
│   ├── drivers/        # 驱动程序
│   │   ├── uart.c/h    # UART 驱动
│   │   └── gpio.c/h    # GPIO 驱动
│   ├── board/          # 板级定义
│   │   ├── rp2350.h    # 寄存器定义
│   │   └── pico2.h     # 开发板定义
│   ├── kernel/         # 内核代码 (待实现)
│   └── main.c          # 主程序
├── link/
│   └── rp2350.ld       # 链接脚本
├── tools/
│   └── openocd.cfg     # 调试配置
├── Makefile
└── README.md
```

## Requirements

- ARM GCC Toolchain: `arm-none-eabi-gcc`
- OpenOCD or picotool for flashing

## Build

```bash
make          # 编译
make clean    # 清理
make info     # 显示项目信息
```

## Flash

```bash
# Using picotool (推荐)
make flash

# Using OpenOCD
make flash-openocd
```

## Debug

```bash
make debug
```

## Current Status

- [x] Stage 1: Minimal boot system
  - [x] Boot2 bootloader
  - [x] Startup code
  - [x] UART driver
  - [x] GPIO driver
  - [x] Main loop

- [ ] Stage 2: Task scheduling
  - [ ] Task control block (TCB)
  - [ ] Context switching
  - [ ] Scheduler

- [ ] Stage 3: Kernel objects
  - [ ] Semaphore
  - [ ] Mutex
  - [ ] Message queue

## License

MIT
