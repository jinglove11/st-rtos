# My-RTOS Diagnostic Guide

This guide covers the on-device shell diagnostics and host-side tools used to
debug P1/P2 kernel behavior.

## On-Device Commands

### `crash`

Shows the last captured fault frame and system fault registers.

Use this first for HardFault, MemManage, BusFault, and UsageFault issues. The
most useful fields are `PC`, `LR`, `CFSR`, `MMFAR`, `BFAR`, and `Task ID`.

### `stats [clear]`

Shows global kernel counters and P2 subsystem event counters.

The subsystem table includes:

- `ok`: successful operations
- `err`: generic errors
- `full`: queue/resource exhaustion
- `timeout`: timeout paths
- `del`: delete/remove paths
- `cancel`: cancel paths
- `busy`: busy objects
- `noexist`: deleted or missing objects

Use `stats clear` to clear only the subsystem event counters.

### `mem`

Shows heap usage, peak usage, live allocation count, OOM failure count, invalid
free count, and per-task stack usage. Stack entries marked with `*` are over
80% used.

### `free`

Compact heap summary: total, used, free, peak, alloc/free/fail counts, and live
allocation count.

### `trace [n] [event]`

Shows recent trace entries. Output is bounded: without `n`, the shell prints the
latest 20 entries. `n` is capped to the trace buffer size.

Examples:

```text
trace
trace 50
trace mem
trace 40 timer
trace clear
```

Supported filters:

```text
switch isr syscall ipc bh fault timer irq dev mem cap vfs
```

P2 event classes use the trace `data` field as:

```text
high byte = object id
low byte  = action/result packed by typed trace helper
```

### `dev`

Lists registered devices:

```text
ID  NAME  TYPE  OPEN  IRQ
```

Use this to confirm devfs/device binding and open reference counts before
testing `device_remove()`.

### `ps` / `top`

`ps` lists task state and stack usage. `top` adds CPU usage when
`KERN_TASK_STATS` is enabled.

## Host Tools

### Trace Parser

```bash
python3 tools/trace_parser.py build/stm32f767/my-rtos-stm32f767.elf
python3 tools/trace_parser.py trace_dump.bin --raw --filter MEM
python3 tools/trace_parser.py trace_dump.bin --raw --csv
```

### Crash Analyzer

```bash
python3 tools/crash_analyzer.py build/stm32f767/my-rtos-stm32f767.elf
python3 tools/crash_analyzer.py crash_dump.bin --raw --elf build/stm32f767/my-rtos-stm32f767.elf
```

## Common Workflows

### Queue or Resource Exhaustion

1. Run `stats`.
2. Check `full` under `timer`, `bh`, `dev`, or `mem`.
3. Run `trace 50 timer`, `trace 50 dev`, or `trace 50 mem` to inspect the
   recent object-level sequence.

### Device Removal Problems

1. Run `dev` and check the `OPEN` count.
2. If remove returns busy, close the fd owner or let task cleanup run.
3. Run `stats` and check `dev busy/del/noexist`.

### Memory Leaks or OOM

1. Run `mem`.
2. Check `Live`, `OOM`, and `BadFree`.
3. Run `trace 50 mem` for recent alloc/free/fail events.

### Fault or Random Crash

1. Run `crash`.
2. Run `trace 50` for the recent system sequence.
3. Use `tools/crash_analyzer.py` with the matching ELF.
4. Use `addr2line` for PC/LR if needed:

```bash
tools/arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-eabi/bin/arm-none-eabi-addr2line \
  -e build/stm32f767/my-rtos-stm32f767.elf -f -C 0x08004A32 0x08004A1C
```

## Configuration

| Option | Purpose |
|--------|---------|
| `TRACE_ENABLE` | Enables trace buffer and shell trace command |
| `TRACE_BUFFER_SIZE` | Trace entry count, default 256 |
| `KERN_TASK_STATS` | Enables CPU/global/subsystem stats |
| `FAULT_ENABLE` | Enables crash dump capture |
| `SHELL_ENABLE` | Enables diagnostic shell |
