#!/usr/bin/env python3
"""
crash_analyzer.py — Decode my-rtos crash_dump_t from ELF or raw binary dump.

Usage:
    python3 crash_analyzer.py build/stm32f767/my-rtos-stm32f767.elf
    python3 crash_analyzer.py crash_dump.bin --raw
    python3 crash_analyzer.py crash_dump.bin --raw --elf build/stm32f767/my-rtos-stm32f767.elf
"""

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from myrtos_tools import (
    CRASH_DUMP_SIZE, FAULT_TYPE_NAMES,
    parse_crash_dump, format_crash_dump, find_symbol_raw, find_section_raw,
)


def resolve_symbol(elf_path: str, addr: int) -> str | None:
    """Resolve an address to the nearest symbol name in the ELF."""
    with open(elf_path, "rb") as f:
        data = f.read()

    if data[:4] != b"\x7fELF":
        return None

    from myrtos_tools import _get_shstr, _section_by_name

    shoff, shentsize, shnum, shstr_offset = _get_shstr(data)
    symtab = _section_by_name(data, shoff, shentsize, shnum, shstr_offset, ".symtab")
    strtab = _section_by_name(data, shoff, shentsize, shnum, shstr_offset, ".strtab")

    if not symtab or not strtab:
        return None

    entsize = symtab["entsize"]
    best_name, best_addr = None, 0

    for i in range(0, symtab["size"], entsize):
        st_info = struct.unpack_from("<B", data, symtab["offset"] + i + 12)[0]
        if st_info & 0xF != 2:  # STT_FUNC only
            continue
        st_value = struct.unpack_from("<I", data, symtab["offset"] + i + 4)[0]
        st_size = struct.unpack_from("<I", data, symtab["offset"] + i + 8)[0]
        st_name = struct.unpack_from("<I", data, symtab["offset"] + i)[0]
        name = data[strtab["offset"] + st_name:].split(b"\x00")[0].decode("ascii", errors="replace")

        if st_value <= addr < st_value + st_size:
            offset = addr - st_value
            return f"{name}+0x{offset:X}" if offset else name

        if st_value <= addr and st_value > best_addr:
            best_addr = st_value
            best_name = name

    if best_name:
        return f"{best_name}+0x{addr - best_addr:X}"
    return None


def main():
    import argparse
    parser = argparse.ArgumentParser(description="Decode my-rtos crash dump")
    parser.add_argument("input", help="ELF file or raw binary dump")
    parser.add_argument("--raw", action="store_true", help="Input is raw binary dump")
    parser.add_argument("--elf", type=str, default=None, help="ELF for symbol resolution (with --raw)")
    parser.add_argument("--json", action="store_true", help="Output as JSON")
    args = parser.parse_args()

    if args.raw:
        with open(args.input, "rb") as f:
            data = f.read()
        if len(data) < CRASH_DUMP_SIZE:
            print(f"ERROR: Need {CRASH_DUMP_SIZE} bytes, got {len(data)}", file=sys.stderr)
            sys.exit(1)
    else:
        section = find_section_raw(args.input, ".crash_dump")
        if not section:
            print("ERROR: .crash_dump section not found in ELF.", file=sys.stderr)
            sys.exit(1)
        data = section
        if len(data) < CRASH_DUMP_SIZE:
            print(f"ERROR: .crash_dump too small ({len(data)} < {CRASH_DUMP_SIZE})", file=sys.stderr)
            sys.exit(1)

    dump = parse_crash_dump(data, 0)

    # All zeros means no crash recorded
    if all(v == 0 for v in dump.values()):
        print("No crash recorded (all zeros).")
        return

    if args.json:
        import json
        dump["fault_name"] = FAULT_TYPE_NAMES.get(dump["fault_type"], f"UNKNOWN({dump['fault_type']})")
        print(json.dumps(dump, indent=2))
    else:
        print(format_crash_dump(dump))

        # Symbol resolution
        elf_path = args.elf if args.raw else args.input
        pc = dump["pc"]
        lr = dump["lr"]

        sym_pc = resolve_symbol(elf_path, pc)
        sym_lr = resolve_symbol(elf_path, lr)

        if sym_pc or sym_lr:
            print("\n── Symbol Resolution ──")
            if sym_pc:
                print(f"PC: {sym_pc}")
            if sym_lr:
                print(f"LR: {sym_lr}")

        # Sanity check
        ft = dump["fault_type"]
        if ft == 0:
            print("\nWARNING: fault_type=0 (HardFault) — if CFSR shows MemManage/BusFault/UsageFault bits,"
                  " this may indicate the FAULT handler itself faulted again (double fault).")
        if dump["xpsr"] & 0x1FF:
            exc_num = dump["xpsr"] & 0x1FF
            print(f"\nNOTE: xPSR exception number = {exc_num} — task was in exception handler when fault occurred.")


if __name__ == "__main__":
    main()
