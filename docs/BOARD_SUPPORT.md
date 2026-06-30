# Board Support Matrix

This document captures what works on each supported board, so new contributors
can pick the right target and so the dual-track build system stays honest.

For the strategic rationale (why two boards, why dual-track build), see the
top of [README.md](../README.md) and [MICROKERNEL_OS_ROADMAP.md](../MICROKERNEL_OS_ROADMAP.md).

---

## At a glance

| Feature                     | RP2350 / Pico 2 W        | STM32F767 / Nucleo-F767ZI |
| --------------------------- | ------------------------ | ------------------------- |
| **Core**                    | Cortex-M33 (ARMv8-M NS)  | Cortex-M7 (ARMv7-M)       |
| **Core count used**         | 1 of 2 (Core 1 in Phase 6)| 1                         |
| **Frequency**               | 150 MHz (SDK default)    | 216 MHz max               |
| **Flash / RAM**             | 4 MB external + 520 KB   | 2 MB + 512 KB             |
| **MPU**                     | PMSAv8, 8 regions, MAIR  | PMSAv7, 16 regions        |
| **FPU**                     | FPv5-SP (no FP context)  | FPv5-SP (no FP context)   |
| **Build system**            | CMake + Pico SDK 2.2.0   | classic Make              |
| **defconfig**               | `configs/rp2350_defconfig` | `configs/stm32f767_defconfig` |
| **All 4 presets work?**     | Yes (tiny/default/release/full) | Only default            |
| **Image format**            | ELF + UF2 (NS image)     | ELF + BIN                 |
| **Flash tool**              | OpenOCD + CMSIS-DAP (ELF)| OpenOCD + ST-Link (ELF)   |
| **OpenOCD config**          | `tools/openocd.cfg`      | `board/st_nucleo_f7.cfg`  |
| **Console UART**            | uart0 @ GPIO0/GPIO1      | USART3 @ PD8/PD9          |
| **Test baseline**           | 2867/2867 PASS           | Historical reference      |
| **Active development**      | **Yes**                  | Maintenance               |

---

## RP2350 — primary verification target

### What works

- Full scheduler + task lifecycle + priority bitmap + time-slicing.
- Cortex-M33 context switch (PendSV / SVC / first_switch) with **PSPLIM** saved
  and restored per-task — stack overflow is caught as a MemManage fault.
- **PMSAv8 MPU**: 8-entry MAIR table, region save/restore during live updates
  (MPU_CTRL preserved across updates), `task_create_user` lays down Flash RO+X
  and stack RW regions cleanly.
- SVC / syscall dispatcher; user pointer validation; capability-aware path.
- Capability system: derive / transfer / revoke / ref-counted cleanup.
- IPC: semaphore, mutex (PI + deadlock detect), mqueue, event, endpoint
  (multi-client request/reply), channel (msg + shm).
- VFS / devfs / ramfs; per-task fd table; fd auto-close on task exit.
- Timer (min-heap), threaded IRQ, bottom-half service task.
- Fault handler: MemManage / BusFault / UsageFault / HardFault decoded; CFSR
  bitfields + MMFAR + active TCB dumped.
- Trace ring buffer; per-task CPU stats; counters for syscall/IPC/mem/dev/fault.
- Shell (`ps`, `top`, `free`, `mem`, `ls`, `cat`, `driver`, `fs`, ...).
- User-mode driver + FS server prototypes (shell-managed lifecycle).

### What's deferred (per roadmap)

- Core 1 bring-up (Phase 6: SMP).
- WiFi (CYW43) networking (Phase 7).
- Secure boot / OTA (Phase 8).
- Power management + RT scheduling + dynamic linking (Phase 9).

### Burning the image

```bash
make setup-pico-sdk                   # one-time
cp configs/default_defconfig .config
python3 scripts/menuconfig.py genconfig
make
make flash                            # via tools/openocd.cfg
picocom -b 115200 /dev/ttyACM0
```

Expected boot:

```
My-RTOS boot
...
My-RTOS Test Suite v2.0
...
All tests PASSED! (2867/2867)

My-RTOS Shell v1.0
my-rtos>
```

### GDB post-mortem (when a fault locks the target)

```bash
# Terminal 1
openocd -f tools/openocd.cfg

# Terminal 2
tools/arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-eabi/bin/arm-none-eabi-gdb \
    -q -x tools/gdb_dump_state.gdb \
    build/rp2350-pico-sdk/my-rtos-pico2w.elf
```

The script halts the target, dumps CFSR/HFSR/MMFAR/BFAR/PSPLIM + the active
TCB + backtrace, then drops into interactive gdb.

---

## STM32F767 — maintenance / classic path

### Why it's still here

- Historic primary target; large body of testing and tuning for Cortex-M7.
- Exercises the **classic Make** build path, which catches CMake-specific drift.
- Reference for PMSAv7 MPU code path (under `BOARD_MPU_ARMV8 = n`).
- Useful for users who don't have an RP2350 board.

### What works

Everything in the RP2350 list, modulo PMSAv8-specific bits:
- PMSAv7 MPU (16 regions, no MAIR, attribute encoding by direct TEX/SCB/C bits).
- No PSPLIM (M7 lacks it); stack guard implemented as a separate MPU region.

### Burning the image

```bash
make stm32f767_defconfig
make BOARD=stm32f767
make BOARD=stm32f767 flash
picocom -b 115200 /dev/ttyACM0
```

---

## Dual-track build system

The project deliberately keeps two build systems side-by-side. See
[plan: federated-puzzling-barto.md](../../plans/federated-puzzling-barto.md)
for the strategic decision.

| Path         | Files                                   | Owned by        |
| ------------ | --------------------------------------- | --------------- |
| RP2350       | `CMakeLists.txt`, Pico SDK 2.2.0        | Pico SDK native |
| STM32F767    | `Makefile` (no SDK), hand-written ld    | Project         |

The `Makefile` top-level detects `BOARD` and dispatches. RP2350 path delegates
to CMake; STM32 path is fully self-contained in Make.

---

## CI matrix

[`.github/workflows/build.yml`](../.github/workflows/build.yml) runs:

- `rp2350 × {tiny, default, release, full}` — CMake path, all 4 presets.
- `stm32f767 × default` — classic Make path.
- `docs` — Kconfig doc sync check (catches stale `docs/kconfig/`).

Local mirror: [`scripts/ci_local.sh`](../scripts/ci_local.sh).

No hardware tests in CI (no QEMU for M33, no DAPLink in the runner). Hardware
validation runs locally via [`scripts/regression.sh`](../scripts/regression.sh).
