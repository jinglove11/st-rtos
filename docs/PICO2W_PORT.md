# Raspberry Pi Pico 2 W Port

## Status

The default My-RTOS target is Raspberry Pi Pico 2 W (`RP2350`, Arm Cortex-M33
secure mode). The build uses Pico SDK 2.2.0 for the RP2350 boot image, clock
startup, linker layout, and UF2 generation. Kernel scheduling, exceptions,
UART, IPC, VFS, services, and tests remain My-RTOS code.

Implemented:

- RP2350 ARM Secure image definition and boot stage
- Cortex-M33 SVC, PendSV, SysTick, and fault vector bindings
- Runtime-writable RAM vector table for My-RTOS IRQ registration
- UART0 console on GPIO0/GPIO1 at 115200 8N1
- RP2350 IRQ limit and board memory layout
- ELF, BIN, HEX, DIS, and UF2 outputs
- Automatic image verification and UART boot-test capture
- STM32F767 build compatibility retained

Not yet implemented:

- USB CDC console
- CYW43 Wi-Fi/Bluetooth and onboard LED control
- Core 1 scheduling

## Build

```sh
make setup-pico-sdk
make verify
```

The flashable image is:

```text
build/rp2350-pico-sdk/my-rtos-pico2w.uf2
```

`make verify` checks the RP2350 family metadata, ARM Secure image type,
`pico2_w` board tag, exception handlers, runtime UART symbol, and test-module
linker boundaries.

## Flash

1. Hold BOOTSEL while connecting Pico 2 W over USB.
2. Wait for the `RP2350` mass-storage volume.
3. Run `make flash`.

For a nonstandard mount point:

```sh
make flash MOUNT=/path/to/RP2350
```

## UART Wiring

The USB connector is used for power and BOOTSEL flashing, not the shell.
Connect a 3.3 V USB-to-UART adapter:

| Pico 2 W | Physical pin | USB-UART |
|---|---:|---|
| GP0 / UART0 TX | 1 | RX |
| GP1 / UART0 RX | 2 | TX |
| GND | 3 | GND |

Do not connect a 5 V UART signal to GP0 or GP1.

## Tests

Start the receiver, then reset or reconnect the board so the complete boot
output is captured:

```sh
make test-serial-boot PORT=/dev/ttyUSB0
```

After the shell prompt appears, run the Phase 5 service-runtime test:

```sh
make test-serial-svc-runtime PORT=/dev/ttyUSB0
```
