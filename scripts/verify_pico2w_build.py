#!/usr/bin/env python3
import argparse
import pathlib
import subprocess
import sys


def run(command):
    return subprocess.run(command, check=True, text=True,
                          stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT).stdout


def main():
    parser = argparse.ArgumentParser(description="Validate a My-RTOS Pico 2 W image")
    parser.add_argument("--elf", required=True)
    parser.add_argument("--uf2", required=True)
    parser.add_argument("--nm", required=True)
    parser.add_argument("--picotool", required=True)
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

    info = run([args.picotool, "info", "-a", str(uf2)])
    expected = ("target chip:         RP2350", "image type:          ARM Secure",
                "pico_board:          pico2_w")
    missing_info = [item.strip() for item in expected if item not in info]
    if missing_info:
        raise RuntimeError("invalid UF2 metadata: " + ", ".join(missing_info))

    print(f"PASS: Pico 2 W image verified ({uf2.stat().st_size} bytes)")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
