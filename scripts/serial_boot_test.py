#!/usr/bin/env python3
"""Capture and validate the automatic My-RTOS boot test suite over UART."""

import argparse
import errno
import os
import re
import select
import subprocess
import sys
import termios
import time


BAUD_RATES = {115200: termios.B115200}


def configure(fd, baud):
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = BAUD_RATES[baud] | termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIOFLUSH)


def capture(port, baud, timeout, prompt, reset_command=None):
    fd = os.open(port, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
    old_attrs = termios.tcgetattr(fd)
    try:
        configure(fd, baud)
        if reset_command:
            result = subprocess.run(reset_command, text=True,
                                    stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT)
            if result.returncode != 0:
                raise RuntimeError("DAPLink flash/reset failed:\n" + result.stdout)
        deadline = time.monotonic() + timeout
        data = bytearray()
        while time.monotonic() < deadline:
            ready, _, _ = select.select([fd], [], [], 0.1)
            if not ready:
                continue
            try:
                chunk = os.read(fd, 4096)
            except BlockingIOError as exc:
                if exc.errno in (errno.EAGAIN, errno.EWOULDBLOCK):
                    continue
                raise
            if chunk:
                data.extend(chunk)
                text = data.decode("utf-8", errors="replace")
                if prompt in text:
                    return text
        return data.decode("utf-8", errors="replace")
    finally:
        termios.tcsetattr(fd, termios.TCSANOW, old_attrs)
        os.close(fd)


def validate(output):
    if "My-RTOS Test Suite v2.0" not in output:
        raise AssertionError("test suite banner not found; reset or reconnect the board")
    if "All tests PASSED!" not in output:
        raise AssertionError("automatic test suite did not pass")
    failures = re.findall(r"\[MODULE\]\s+(\S+).*?fail \+([1-9][0-9]*)", output)
    if failures:
        detail = ", ".join(f"{name}:{count}" for name, count in failures)
        raise AssertionError("module failures: " + detail)
    modules = re.search(r"Modules:\s+([0-9]+)", output)
    if not modules or int(modules.group(1)) < 15:
        raise AssertionError("module count is missing or unexpectedly low")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, choices=BAUD_RATES, default=115200)
    parser.add_argument("--timeout", type=float, default=45.0)
    parser.add_argument("--prompt", default="my-rtos> ")
    parser.add_argument("--daplink-flash", action="store_true",
                        help="flash/reset with OpenOCD after opening UART")
    parser.add_argument("--openocd", default="openocd")
    parser.add_argument("--openocd-config", default="tools/openocd.cfg")
    parser.add_argument(
        "--elf", default="build/rp2350-pico-sdk/my-rtos-pico2w.elf")
    args = parser.parse_args()

    try:
        reset_command = None
        if args.daplink_flash:
            reset_command = [
                args.openocd, "-f", args.openocd_config,
                "-c", f"program {args.elf} verify reset exit",
            ]
        output = capture(args.port, args.baud, args.timeout, args.prompt,
                         reset_command)
        print(output, end="" if output.endswith("\n") else "\n")
        validate(output)
    except Exception as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1

    print("PASS: Pico 2 W automatic boot test")
    return 0


if __name__ == "__main__":
    sys.exit(main())
