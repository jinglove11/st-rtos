#!/usr/bin/env python3
"""Run the RP2350 SMP boot gate, then monitor shell liveness over UART."""

import argparse
import errno
import os
import select
import sys
import termios
import time


BAUD_RATES = {115200: termios.B115200}
if hasattr(termios, "B921600"):
    BAUD_RATES[921600] = termios.B921600

BOOT_FATAL = ("[FAIL]", "!!! KERNEL PANIC !!!", "[LOCKDEP]", "ASSERT FAILED")
SOAK_FATAL = BOOT_FATAL + ("!!! FAULT !!!", "Test Framework Starting...")


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


def read_chunk(fd, timeout=0.2):
    ready, _, _ = select.select([fd], [], [], timeout)
    if not ready:
        return b""
    try:
        return os.read(fd, 4096)
    except BlockingIOError as exc:
        if exc.errno in (errno.EAGAIN, errno.EWOULDBLOCK):
            return b""
        raise


def find_fatal(text, patterns):
    for pattern in patterns:
        if pattern in text:
            return pattern
    return None


def emit(chunk, log_file):
    text = chunk.decode("utf-8", errors="replace")
    sys.stdout.write(text)
    sys.stdout.flush()
    log_file.write(text)
    log_file.flush()
    return text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, choices=BAUD_RATES, default=115200)
    parser.add_argument("--duration", type=float, default=1800.0,
                        help="seconds to monitor after the boot suite passes")
    parser.add_argument("--startup-timeout", type=float, default=1800.0,
                        help="seconds allowed for the 1M-iteration suite")
    parser.add_argument("--probe-interval", type=float, default=60.0)
    parser.add_argument("--probe-timeout", type=float, default=10.0)
    parser.add_argument("--prompt", default="my-rtos> ")
    parser.add_argument("--probe-command", default="ps")
    parser.add_argument("--log", default="/tmp/my-rtos-smp-soak.log")
    args = parser.parse_args()

    if args.duration <= 0 or args.startup_timeout <= 0:
        parser.error("duration and startup-timeout must be positive")
    if args.probe_interval <= 0 or args.probe_timeout <= 0:
        parser.error("probe intervals must be positive")

    fd = None
    old_attrs = None
    try:
        fd = os.open(args.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        old_attrs = termios.tcgetattr(fd)
        configure(fd, args.baud)

        with open(args.log, "w", encoding="utf-8") as log_file:
            print(f"[soak] waiting for SMP suite on {args.port}@{args.baud}")
            startup_deadline = time.monotonic() + args.startup_timeout
            boot_text = ""
            while time.monotonic() < startup_deadline:
                chunk = read_chunk(fd)
                if not chunk:
                    continue
                boot_text = (boot_text + emit(chunk, log_file))[-65536:]
                fatal = find_fatal(boot_text, BOOT_FATAL)
                if fatal:
                    raise RuntimeError(f"boot gate reported {fatal!r}")
                if "All tests PASSED!" in boot_text and args.prompt in boot_text:
                    break
            else:
                raise RuntimeError("SMP boot suite did not reach a passing shell prompt")

            if "[MODULE] smp" not in boot_text:
                raise RuntimeError("SMP module result missing; wrong image/profile is running")

            print(f"\n[soak] boot gate passed; monitoring for {args.duration:.0f}s")
            soak_start = time.monotonic()
            soak_deadline = soak_start + args.duration
            next_probe = soak_start
            probe_deadline = 0.0
            probe_pending = False
            monitor_text = ""
            probe_text = ""
            probe_count = 0

            while time.monotonic() < soak_deadline:
                now = time.monotonic()
                if not probe_pending and now >= next_probe:
                    os.write(fd, (args.probe_command + "\r").encode("ascii"))
                    probe_pending = True
                    probe_deadline = now + args.probe_timeout
                    probe_text = ""
                    probe_count += 1

                chunk = read_chunk(fd)
                if chunk:
                    text = emit(chunk, log_file)
                    monitor_text = (monitor_text + text)[-4096:]
                    probe_text += text
                    fatal = find_fatal(monitor_text, SOAK_FATAL)
                    if fatal:
                        raise RuntimeError(f"soak reported {fatal!r}")
                    if probe_pending and args.prompt in probe_text:
                        elapsed = time.monotonic() - soak_start
                        print(f"\n[soak] heartbeat {probe_count} OK at {elapsed:.0f}s")
                        probe_pending = False
                        next_probe = time.monotonic() + args.probe_interval
                elif probe_pending and time.monotonic() >= probe_deadline:
                    raise RuntimeError(
                        f"shell liveness probe {probe_count} timed out")

            if probe_pending:
                while time.monotonic() < probe_deadline:
                    chunk = read_chunk(fd)
                    if not chunk:
                        continue
                    text = emit(chunk, log_file)
                    probe_text += text
                    if args.prompt in probe_text:
                        probe_pending = False
                        break
                if probe_pending:
                    raise RuntimeError(
                        f"final shell liveness probe {probe_count} timed out")

        print(f"\nPASS: SMP soak completed ({args.duration:.0f}s, "
              f"{probe_count} liveness probes); log: {args.log}")
        return 0
    except Exception as exc:
        print(f"\nFAIL: {exc}", file=sys.stderr)
        print(f"log: {args.log}", file=sys.stderr)
        return 1
    finally:
        if fd is not None:
            if old_attrs is not None:
                termios.tcsetattr(fd, termios.TCSANOW, old_attrs)
            os.close(fd)


if __name__ == "__main__":
    sys.exit(main())
