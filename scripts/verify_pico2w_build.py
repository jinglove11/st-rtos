#!/usr/bin/env python3
import argparse
import pathlib
import struct
import subprocess
import sys

# UF2 family ID(boot/uf2.h)。校验意图与 picotool info 相同:
# 目标芯片 RP2350、ARM Secure、板卡 pico2_w。不再用 picotool 解析——
# 它对本镜像内的 ASCII 字符串有误读为地址的 bug(failed to read
# memory at 0x…,KNOWN_ISSUES),且随镜像布局间歇性触发。
UF2_MAGIC0 = 0x0A324655
UF2_MAGIC1 = 0x9E5D5157
UF2_FLAG_FAMILY_ID = 0x00002000
RP2350_ARM_S_FAMILY_ID = 0xE48BFF59
ABSOLUTE_FAMILY_ID = 0xE48BFF57
DATA_FAMILY_ID = 0xE48BFF58


def run(command):
    return subprocess.run(command, check=True, text=True,
                          stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT).stdout


def verify_uf2(uf2):
    data = uf2.read_bytes()
    if not data or len(data) % 512 != 0:
        raise RuntimeError("UF2 is not a sequence of 512-byte blocks")

    families = set()
    arm_s_blocks = 0
    for off in range(0, len(data), 512):
        block = data[off:off + 512]
        magic0, magic1, flags = struct.unpack_from("<III", block, 0)
        if magic0 != UF2_MAGIC0 or magic1 != UF2_MAGIC1:
            raise RuntimeError(f"bad UF2 block magic at offset {off}")
        if flags & UF2_FLAG_FAMILY_ID:
            families.add(struct.unpack_from("<I", block, 28)[0])
            if struct.unpack_from("<I", block, 28)[0] == RP2350_ARM_S_FAMILY_ID:
                arm_s_blocks += 1

    unexpected = families - {RP2350_ARM_S_FAMILY_ID, ABSOLUTE_FAMILY_ID,
                             DATA_FAMILY_ID}
    if unexpected:
        raise RuntimeError("unexpected UF2 family IDs: " +
                           ", ".join(hex(f) for f in sorted(unexpected)))
    if arm_s_blocks == 0:
        raise RuntimeError("no RP2350 ARM Secure blocks in UF2")
    if b"pico2_w" not in data:
        raise RuntimeError("pico2_w board metadata string missing")


def main():
    parser = argparse.ArgumentParser(description="Validate a My-RTOS Pico 2 W image")
    parser.add_argument("--elf", required=True)
    parser.add_argument("--uf2", required=True)
    parser.add_argument("--nm", required=True)
    parser.add_argument("--picotool", required=False,
                        help="unused (kept for ci_local.sh compatibility)")
    args = parser.parse_args()

    elf = pathlib.Path(args.elf)
    uf2 = pathlib.Path(args.uf2)
    if not elf.is_file() or not uf2.is_file():
        raise RuntimeError("ELF or UF2 output is missing")

    symbols = run([args.nm, str(elf)])
    required = {
        "isr_pendsv", "isr_svcall", "isr_systick",
        "__test_modules_start", "__test_modules_end",
        "uart_init", "kern_sem_init",
        "mpu_init", "mpu_region_encode", "isr_memmanage",
    }
    missing = sorted(symbol for symbol in required if symbol not in symbols)
    if missing:
        raise RuntimeError("missing required symbols: " + ", ".join(missing))

    verify_uf2(uf2)

    print(f"PASS: Pico 2 W image verified ({uf2.stat().st_size} bytes)")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
