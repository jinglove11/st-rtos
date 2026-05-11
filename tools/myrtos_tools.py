#!/usr/bin/env python3
"""
myrtos_tools.py — ELF reader and shared utilities for my-rtos diagnostic tools.

Reads my-rtos ELF files to locate sections and symbols.
Falls back to raw struct parsing if pyelftools is not installed.
"""

import struct
import sys
from pathlib import Path

# ── crash_dump_t layout (29 × uint32_t = 116 bytes) ──────────────────────
CRASH_DUMP_FIELDS = [
    "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
    "r8", "r9", "r10", "r11", "r12",
    "sp", "lr", "pc", "xpsr",
    "cfsr", "hfsr", "mmfar", "bfar",
    "msp", "psp",
    "fault_type", "task_id",
    "reserved0", "reserved1", "reserved2", "reserved3",
]

CRASH_DUMP_SIZE = 116

# ── trace_entry_t layout (8 bytes) ────────────────────────────────────────
# typedef struct {
#     uint32_t tick;      // offset 0
#     uint8_t  event;     // offset 4
#     uint8_t  task_id;   // offset 5
#     uint16_t data;      // offset 6
# } trace_entry_t;

TRACE_ENTRY_SIZE = 8
TRACE_BUFFER_SIZE = 256  # default, may be overridden

TRACE_EVENT_NAMES = {
    0: "SWITCH",
    1: "ISR_IN",
    2: "ISR_OUT",
    3: "SYSCALL",
    4: "IPC_SND",
    5: "IPC_RCV",
    6: "BH",
    7: "FAULT",
}

FAULT_TYPE_NAMES = {
    0: "HardFault",
    1: "MemManage",
    2: "BusFault",
    3: "UsageFault",
}


def parse_crash_dump(data: bytes, offset: int = 0) -> dict:
    """Parse crash_dump_t from raw bytes."""
    if len(data) < offset + CRASH_DUMP_SIZE:
        raise ValueError(f"Need {CRASH_DUMP_SIZE} bytes, got {len(data) - offset}")
    values = struct.unpack_from("<29I", data, offset)
    return dict(zip(CRASH_DUMP_FIELDS, values))


def parse_trace_entry(data: bytes, offset: int = 0) -> dict:
    """Parse a single trace_entry_t from raw bytes."""
    tick, event, task_id, d = struct.unpack_from("<IBBH", data, offset)
    return {"tick": tick, "event": event, "task_id": task_id, "data": d}


def format_crash_dump(dump: dict) -> str:
    """Format crash_dump_t as human-readable report."""
    ft = dump["fault_type"]
    fault_name = FAULT_TYPE_NAMES.get(ft, f"UNKNOWN({ft})")

    # Decode CFSR bytes
    cfsr = dump["cfsr"]
    mmfsr = cfsr & 0xFF
    bfsr = (cfsr >> 8) & 0xFF
    ufsr = (cfsr >> 16) & 0xFFFF

    lines = [
        "=== Crash Dump Analysis ===",
        f"Fault Type:  {fault_name} ({ft})",
        f"Task ID:     {dump['task_id']}",
        "",
        "── Exception Frame ──",
        f"PC:     0x{dump['pc']:08X}",
        f"LR:     0x{dump['lr']:08X}",
        f"SP:     0x{dump['sp']:08X}",
        f"xPSR:   0x{dump['xpsr']:08X}",
        "",
        f"R0:  0x{dump['r0']:08X}  R1:  0x{dump['r1']:08X}",
        f"R2:  0x{dump['r2']:08X}  R3:  0x{dump['r3']:08X}",
        f"R4:  0x{dump['r4']:08X}  R5:  0x{dump['r5']:08X}",
        f"R6:  0x{dump['r6']:08X}  R7:  0x{dump['r7']:08X}",
        f"R8:  0x{dump['r8']:08X}  R9:  0x{dump['r9']:08X}",
        f"R10: 0x{dump['r10']:08X}  R11: 0x{dump['r11']:08X}",
        f"R12: 0x{dump['r12']:08X}",
        "",
        "── Fault Registers ──",
        f"CFSR:  0x{cfsr:08X}  (MMFSR=0x{mmfsr:02X}, BFSR=0x{bfsr:02X}, UFSR=0x{ufsr:04X})",
        f"HFSR:  0x{dump['hfsr']:08X}",
        f"MMFAR: 0x{dump['mmfar']:08X}",
        f"BFAR:  0x{dump['bfar']:08X}",
        "",
        "── Stack Pointers ──",
        f"MSP:   0x{dump['msp']:08X}",
        f"PSP:   0x{dump['psp']:08X}",
        "",
        "── CFSR Decode ──",
    ]

    # MMFSR decode
    mmfsr_bits = {
        7: "MMARVALID",
        4: "MSTKERR",
        3: "MUNSTKERR",
        1: "DACCVIOL",
        0: "IACCVIOL",
    }
    lines.append("MMFSR (MemManage):")
    for bit, name in mmfsr_bits.items():
        if mmfsr & (1 << bit):
            lines.append(f"  [{name}] ", end="")
    lines.append("")

    # BFSR decode
    bfsr_bits = {
        7: "BFARVALID",
        4: "STKERR",
        3: "UNSTKERR",
        2: "IMPRECISERR",
        1: "PRECISERR",
        0: "IBUSERR",
    }
    lines.append("BFSR (BusFault):")
    for bit, name in bfsr_bits.items():
        if bfsr & (1 << bit):
            lines.append(f"  [{name}] ", end="")
    lines.append("")

    # UFSR decode
    ufsr_bits = {
        9: "DIVBYZERO",
        8: "UNALIGNED",
        3: "NOCP",
        2: "INVPC",
        1: "INVSTATE",
        0: "UNDEFINSTR",
    }
    lines.append("UFSR (UsageFault):")
    for bit, name in ufsr_bits.items():
        if ufsr & (1 << bit):
            lines.append(f"  [{name}] ", end="")
    lines.append("")

    return "\n".join(lines)


def find_section_raw(elf_path: str, section_name: str) -> bytes | None:
    """Find section data by parsing ELF header directly (no pyelftools needed)."""
    with open(elf_path, "rb") as f:
        data = f.read()

    if data[:4] != b"\x7fELF":
        return None

    shoff, shentsize, shnum, shstr_offset = _get_shstr(data)
    sec = _section_by_name(data, shoff, shentsize, shnum, shstr_offset, section_name)
    if sec:
        return data[sec["offset"]:sec["offset"] + sec["size"]]
    return None


def _get_shstr(data: bytes) -> tuple[int, int]:
    """Get section header string table offset and section name string table offset."""
    shoff = struct.unpack_from("<I", data, 32)[0]
    shentsize = struct.unpack_from("<H", data, 46)[0]
    shnum = struct.unpack_from("<H", data, 48)[0]
    shstrndx = struct.unpack_from("<H", data, 50)[0]

    shstr_entry = shoff + shstrndx * shentsize
    shstr_offset = struct.unpack_from("<I", data, shstr_entry + 16)[0]
    return shoff, shentsize, shnum, shstr_offset


def _section_by_name(data: bytes, shoff: int, shentsize: int, shnum: int,
                     shstr_offset: int, target_name: str) -> dict | None:
    """Find a section entry by name. Returns dict with offset/size/type/entsize."""
    for i in range(shnum):
        entry = shoff + i * shentsize
        sh_name = struct.unpack_from("<I", data, entry)[0]
        sh_type = struct.unpack_from("<I", data, entry + 4)[0]
        sh_offset = struct.unpack_from("<I", data, entry + 16)[0]
        sh_size = struct.unpack_from("<I", data, entry + 20)[0]
        sh_entsize = struct.unpack_from("<I", data, entry + 36)[0]

        name = data[shstr_offset + sh_name:].split(b"\x00")[0].decode("ascii", errors="replace")
        if name == target_name:
            return {"offset": sh_offset, "size": sh_size, "type": sh_type, "entsize": sh_entsize}
    return None


def find_symbol_raw(elf_path: str, symbol_name: str) -> tuple[int, int] | None:
    """Find symbol address and size from ELF symtab (no pyelftools needed).
    Returns (address, size) or None."""
    with open(elf_path, "rb") as f:
        data = f.read()

    if data[:4] != b"\x7fELF":
        return None

    shoff, shentsize, shnum, shstr_offset = _get_shstr(data)

    symtab = _section_by_name(data, shoff, shentsize, shnum, shstr_offset, ".symtab")
    strtab = _section_by_name(data, shoff, shentsize, shnum, shstr_offset, ".strtab")

    if not symtab or not strtab:
        return None

    entsize = symtab["entsize"]
    for i in range(0, symtab["size"], entsize):
        st_name = struct.unpack_from("<I", data, symtab["offset"] + i)[0]
        st_value = struct.unpack_from("<I", data, symtab["offset"] + i + 4)[0]
        st_size = struct.unpack_from("<I", data, symtab["offset"] + i + 8)[0]

        name = data[strtab["offset"] + st_name:].split(b"\x00")[0].decode("ascii", errors="replace")
        if name == symbol_name:
            return (st_value, st_size)

    return None


def try_pyelftools(elf_path: str):
    """Try to open ELF with pyelftools. Returns ELF object or None."""
    try:
        from elftools.elf.elffile import ELFFile
        with open(elf_path, "rb") as f:
            return ELFFile(f)
    except ImportError:
        return None


if __name__ == "__main__":
    # Quick test
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <elf_path>")
        sys.exit(1)

    elf_path = sys.argv[1]
    crash = find_section_raw(elf_path, ".crash_dump")
    print(f".crash_dump: {'found' if crash else 'NOT FOUND'} ({len(crash) if crash else 0} bytes)")

    sym = find_symbol_raw(elf_path, "crash_dump")
    print(f"crash_dump symbol: {sym}")

    sym = find_symbol_raw(elf_path, "trace_buf")
    print(f"trace_buf symbol: {sym}")
