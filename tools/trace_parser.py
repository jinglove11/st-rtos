#!/usr/bin/env python3
"""
trace_parser.py — Parse my-rtos trace buffer from ELF file or raw binary dump.

Usage:
    python3 trace_parser.py build/stm32f767/my-rtos-stm32f767.elf
    python3 trace_parser.py build/stm32f767/my-rtos-stm32f767.elf --filter FAULT
    python3 trace_parser.py trace_dump.bin --raw --count 128
"""

import struct
import sys
from pathlib import Path

# Add tools directory to path for imports
sys.path.insert(0, str(Path(__file__).parent))
from myrtos_tools import (
    TRACE_ENTRY_SIZE, TRACE_EVENT_NAMES,
    parse_trace_entry, find_symbol_raw,
)


def parse_trace_buffer(data: bytes, count: int | None = None) -> list[dict]:
    """Parse raw trace buffer bytes into list of entry dicts."""
    entries = []
    max_entries = len(data) // TRACE_ENTRY_SIZE
    if count is not None:
        max_entries = min(max_entries, count)

    for i in range(max_entries):
        off = i * TRACE_ENTRY_SIZE
        entry = parse_trace_entry(data, off)
        if entry["tick"] == 0 and entry["event"] == 0 and entry["task_id"] == 0:
            continue  # skip empty entries
        entries.append(entry)

    return entries


def print_trace(entries: list[dict], event_filter: str | None = None):
    """Print trace entries in human-readable format."""
    filtered = entries
    if event_filter:
        event_id = None
        for eid, name in TRACE_EVENT_NAMES.items():
            if name.upper() == event_filter.upper():
                event_id = eid
                break
        if event_id is not None:
            filtered = [e for e in entries if e["event"] == event_id]

    print(f"Trace entries: {len(filtered)} (total: {len(entries)})")
    print(f"  {'TICK':>8}  {'EVENT':<8} {'TASK':>4}  DATA")
    print(f"  {'─'*8}  {'─'*8} {'─'*4}  {'─'*4}")

    for e in filtered:
        name = TRACE_EVENT_NAMES.get(e["event"], f"EV{e['event']}")
        print(f"  {e['tick']:>8}  {name:<8} {e['task_id']:>4}  {e['data']}")


def main():
    import argparse
    parser = argparse.ArgumentParser(description="Parse my-rtos trace buffer")
    parser.add_argument("input", help="ELF file or raw binary dump")
    parser.add_argument("--raw", action="store_true", help="Input is raw binary dump")
    parser.add_argument("--count", type=int, default=None, help="Max entries to parse")
    parser.add_argument("--filter", type=str, default=None, help="Filter by event name (SWITCH, SYSCALL, FAULT, ...)")
    parser.add_argument("--csv", action="store_true", help="Output as CSV")
    args = parser.parse_args()

    if args.raw:
        with open(args.input, "rb") as f:
            data = f.read()
    else:
        sym = find_symbol_raw(args.input, "trace_buf")
        if not sym:
            print("ERROR: trace_buf symbol not found in ELF.", file=sys.stderr)
            sys.exit(1)
        addr, size = sym
        print(f"trace_buf: 0x{addr:08X} ({size} bytes, {size // TRACE_ENTRY_SIZE} entries)")

        # For BSS symbols, read the section data from ELF
        # BSS sections are zero-initialized in the ELF, so we read entries count only
        # Actual runtime data requires a memory dump
        print("\nNOTE: trace_buf is in BSS — ELF contains no runtime data.")
        print("Use --raw with a memory dump from OpenOCD/GDB:\n")
        print("  (gdb) dump binary memory trace_dump.bin &trace_buf &trace_buf + sizeof(trace_buf)")
        print()
        return

    entries = parse_trace_buffer(data, args.count)
    if args.csv:
        print("tick,event,task_id,data")
        for e in entries:
            name = TRACE_EVENT_NAMES.get(e["event"], f"EV{e['event']}")
            print(f"{e['tick']},{name},{e['task_id']},{e['data']}")
    else:
        print_trace(entries, args.filter)


if __name__ == "__main__":
    main()
