# My-RTOS Diagnostic Guide

> How to use the diagnostic ecosystem for debugging and analysis.

## Overview

The diagnostic system has three tiers:

| Tier | Component | Location |
|------|-----------|----------|
| On-device | Shell commands | `crash`, `stats`, `mem`, `trace`, `ps` |
| On-device | Trace buffer | Ring buffer, 256 entries × 8 bytes |
| Host-side | Python tools | `tools/trace_parser.py`, `tools/crash_analyzer.py` |

---

## 1. Shell Diagnostic Commands

### `crash` — Last Crash Dump

Displays the decoded crash_dump_t from the last fault.

```
$ crash

=== Crash Dump Analysis ===
Fault Type:  MemManage (1)
Task ID:     5

── Exception Frame ──
PC:     0x08004A32
LR:     0x08004A1C
SP:     0x2000A3E0
xPSR:   0x61000000

── Fault Registers ──
CFSR:  0x00000082  (MMFSR=0x82, BFSR=0x00, UFSR=0x0000)
HFSR:  0x40000000
MMFAR: 0xE000ED34
BFAR:  0x00000000

── Stack Pointers ──
MSP:   0x2000B000
PSP:   0x2000A3E0

── CFSR Decode ──
MMFSR (MemManage):
  [MMARVALID] [DACCVIOL]
BFSR (BusFault):
UFSR (UsageFault):
```

**Common fault scenarios:**

| Symptom | CFSR Bits | Likely Cause |
|---------|-----------|--------------|
| DACCVIOL + MMFAR=MPU region | MMFSR[0] | User task accessed outside MPU region |
| IACCVIOL | MMFSR[1] | User task tried to execute code outside MPU region |
| UNDEFINSTR | UFSR[0] | Corrupted stack or bad function pointer |
| INVSTATE | UFSR[1] | Jump to non-Thumb code (bit 0 of PC = 0) |
| STKERR | BFSR[4] or MMFSR[4] | Stack overflow during exception entry |
| DIVBYZERO | UFSR[9] | Integer division by zero |

### `stats` — Kernel Statistics

Displays global kernel counters.

```
$ stats

=== Kernel Statistics ===
Uptime:           12345 ticks (12.3 s)
Context Switches: 8234
IRQs:             1024
Max IRQ Latency:  12 ticks
Faults:           1
Syscalls:         4096
```

- **Max IRQ Latency**: The longest time from IRQ entry to handler execution. High values indicate interrupt-disabled regions are too long.
- **Faults**: Incremented on every fault (MemManage, BusFault, UsageFault, HardFault).

### `mem` — Memory Layout

Displays heap usage and per-task stack watermarks.

```
$ mem

=== Memory Layout ===

Heap: 1024 / 4096 (25%)

--- Tasks ---
  ID  Name            Stack           Usage  Status
   1  idle            256             15%     READY
   2  shell           2048            42%     RUNNING
   3  timer           512             28%     BLOCKED
   5  user_task       1024            87% *   BLOCKED
```

Tasks marked with `*` have stack usage > 80% — increase their stack size.

### `trace` — Trace Buffer Viewer

```
$ trace              # Show all entries
$ trace fault        # Filter by FAULT events
$ trace syscall      # Filter by SYSCALL events
$ trace isr          # Filter by ISR events
$ trace ipc          # Filter by IPC events
$ trace clear        # Clear the buffer
```

**Event legend:**

| Display | Event | data field |
|---------|-------|------------|
| SW | TASK_SWITCH | 0 |
| ISR+ | ISR_ENTER | irq_num |
| ISR- | ISR_EXIT | irq_num |
| SVC | SYSCALL | syscall_num |
| IPC+ | IPC_SEND | ep/ch id |
| IPC- | IPC_RECV | ep/ch id |
| BH | BH_SCHEDULE | bh_id |
| FLT | FAULT | fault_type |

---

## 2. Host-Side Analysis Tools

### trace_parser.py

Parses the trace buffer from a raw memory dump.

```bash
# From ELF (shows metadata only — trace_buf is in BSS)
python3 tools/trace_parser.py build/stm32f767/my-rtos-stm32f767.elf

# From raw dump (captured via GDB/OpenOCD)
python3 tools/trace_parser.py trace_dump.bin --raw --filter FAULT

# CSV output for spreadsheet analysis
python3 tools/trace_parser.py trace_dump.bin --raw --csv
```

**Capturing the trace buffer:**

In GDB:
```
(gdb) dump binary memory trace_dump.bin &trace_buf &trace_buf + sizeof(trace_buf)
```

In OpenOCD:
```
> dump_image trace_dump.bin 0x20009B7C 2048
```

### crash_analyzer.py

Decodes crash_dump_t with symbol resolution.

```bash
# From ELF (.crash_dump section)
python3 tools/crash_analyzer.py build/stm32f767/my-rtos-stm32f767.elf

# From raw dump with symbol resolution
python3 tools/crash_analyzer.py crash_dump.bin --raw --elf build/stm32f767/my-rtos-stm32f767.elf

# JSON output for automation
python3 tools/crash_analyzer.py crash_dump.bin --raw --json
```

**Capturing the crash dump:**

In GDB:
```
(gdb) dump binary memory crash_dump.bin &crash_dump &crash_dump + 116
```

---

## 3. Common Diagnostic Workflows

### Debugging a MemManage Fault

1. Run `crash` in the shell → note PC, MMFAR, CFSR
2. Run `crash_analyzer.py` with --elf for PC → function name
3. Check MMFAR against MPU region addresses
4. If MMFAR points to MPU peripheral region: task tried to access protected hardware
5. If MMFAR is near stack boundary: stack overflow into guard region

### Mapping a Panic PC to Source

Kernel panic output prints PC/LR and the recent trace tail before entering
`kern_panic()`. Use the ELF from the same build:

```bash
tools/arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-eabi/bin/arm-none-eabi-addr2line \
  -e build/stm32f767/my-rtos-stm32f767.elf -f -C 0x08004A32 0x08004A1C
```

Use PC first. LR is useful when PC points into a common fault/panic path.

### Debugging Performance Issues

1. Run `stats` → note IRQ latency max and syscall count
2. Run `trace clear` then `trace` after a delay → review trace entries
3. Use `trace_parser.py --csv` for timeline analysis
4. High syscall count + high IRQ latency = contention in interrupt-disabled section

### Debugging a Stack Overflow

1. Run `mem` → check for tasks with `*` (over 80%)
2. Increase `KERN_TASK_STACK_SIZE` or `KERNEL_IDLE_STACK_SIZE` in defconfig
3. Rebuild and verify with `mem` again

---

## 4. Configuration Reference

| Kconfig Option | Default | Description |
|----------------|---------|-------------|
| `TRACE_ENABLE` | y | Enable trace buffer |
| `TRACE_BUFFER_SIZE` | 256 | Number of trace entries |
| `KERN_TASK_STATS` | y | Enable CPU usage + IRQ statistics |
| `FAULT_ENABLE` | y | Enable crash dump capture |
| `FAULT_CRASH_DUMP` | y | Store crash_dump_t in .crash_dump |
| `SHELL_ENABLE` | y | Shell with diagnostic commands |
