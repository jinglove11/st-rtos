#!/usr/bin/env python3
"""Serial smoke test for the Phase 5 service runtime shell commands."""

from __future__ import annotations

import argparse
import errno
import os
import re
import select
import sys
import termios
import time
from typing import Pattern


BAUD_RATES = {
    9600: termios.B9600,
    19200: termios.B19200,
    38400: termios.B38400,
    57600: termios.B57600,
    115200: termios.B115200,
}


class SerialShell:
    def __init__(self, port: str, baud: int, prompt: str, timeout: float):
        if baud not in BAUD_RATES:
            raise ValueError(f"unsupported baud rate: {baud}")

        self.prompt = prompt
        self.timeout = timeout
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        self._old_attrs = termios.tcgetattr(self.fd)
        self._configure(baud)
        self.debug_raw = False

    def close(self) -> None:
        termios.tcsetattr(self.fd, termios.TCSANOW, self._old_attrs)
        os.close(self.fd)

    def _configure(self, baud: int) -> None:
        attrs = termios.tcgetattr(self.fd)
        attrs[0] = 0
        attrs[1] = 0
        attrs[2] = BAUD_RATES[baud] | termios.CS8 | termios.CREAD | termios.CLOCAL
        attrs[3] = 0
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        termios.tcflush(self.fd, termios.TCIOFLUSH)

    def write_line(self, line: str) -> None:
        os.write(self.fd, (line + "\r").encode("ascii"))

    def clear_input_line(self) -> None:
        os.write(self.fd, b"\x08" * 96)
        self.drain(quiet_time=0.03, max_time=0.2)

    def read_chunk(self) -> bytes:
        try:
            return os.read(self.fd, 4096)
        except BlockingIOError as exc:
            if exc.errno in (errno.EAGAIN, errno.EWOULDBLOCK):
                return b""
            raise

    def drain(self, quiet_time: float = 0.15, max_time: float = 1.0) -> str:
        deadline = time.monotonic() + max_time
        quiet_deadline = time.monotonic() + quiet_time
        data = bytearray()
        while time.monotonic() < deadline:
            wait = max(0.0, min(0.05, quiet_deadline - time.monotonic()))
            ready, _, _ = select.select([self.fd], [], [], wait)
            if not ready:
                if time.monotonic() >= quiet_deadline:
                    break
                continue
            chunk = self.read_chunk()
            if not chunk:
                continue
            data.extend(chunk)
            quiet_deadline = time.monotonic() + quiet_time
        out = data.decode("utf-8", errors="replace")
        if self.debug_raw and out:
            print(f"[drain raw] {out!r}")
        return out

    def read_until_prompt(self, timeout: float | None = None) -> str:
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        data = bytearray()
        prompt_bytes = self.prompt.encode("ascii")
        while time.monotonic() < deadline:
            ready, _, _ = select.select([self.fd], [], [], 0.05)
            if not ready:
                continue
            chunk = self.read_chunk()
            if not chunk:
                continue
            data.extend(chunk)
            if prompt_bytes in data:
                break
        out = data.decode("utf-8", errors="replace")
        if self.debug_raw:
            print(f"[read raw] {out!r}")
        return out

    def read_until_pattern(self, pattern: Pattern[str], timeout: float | None = None) -> str:
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        data = bytearray()
        while time.monotonic() < deadline:
            ready, _, _ = select.select([self.fd], [], [], 0.05)
            if not ready:
                continue
            chunk = self.read_chunk()
            if not chunk:
                continue
            data.extend(chunk)
            out = data.decode("utf-8", errors="replace")
            if pattern.search(out):
                if self.debug_raw:
                    print(f"[read raw] {out!r}")
                return out
        out = data.decode("utf-8", errors="replace")
        if self.debug_raw:
            print(f"[read raw] {out!r}")
        return out

    def sync_prompt(self) -> None:
        drained = self.drain(quiet_time=0.05, max_time=0.3)
        if self.prompt in drained:
            return

        for line in ("", "", ""):
            self.write_line(line)
            out = self.read_until_prompt(self.timeout)
            if self.prompt in out:
                return
        raise AssertionError("shell prompt not found")

    def command(self, line: str, expect: str | Pattern[str], timeout: float | None = None) -> str:
        print(f"$ {line}")
        self.clear_input_line()
        self.write_line(line)
        pattern = re.compile(expect) if isinstance(expect, str) else expect
        out = self.read_until_pattern(pattern, timeout)
        tail = self.drain(quiet_time=0.05, max_time=0.3)
        full = out + tail
        print(full, end="" if full.endswith("\n") else "\n")
        if not pattern.search(full):
            raise AssertionError(f"expected pattern not found after '{line}': {pattern.pattern}")
        return full


def extract_ticks(output: str) -> int:
    match = re.search(r"runtime ticks:\s+([0-9]+)", output)
    if not match:
        raise AssertionError("runtime ticks field not found")
    return int(match.group(1))


def extract_counter(output: str, name: str) -> int:
    match = re.search(rf"{re.escape(name)}:\s+([0-9]+)", output)
    if not match:
        raise AssertionError(f"{name} field not found")
    return int(match.group(1))


def extract_service_counter(output: str, service: str, name: str) -> int:
    match = re.search(
        rf"{re.escape(service)}[\s\S]*?restarts:\s+([0-9]+)\s+recovers:\s+([0-9]+)\s+faults:\s+([0-9]+)",
        output,
    )
    if not match:
        raise AssertionError(f"{service} counter row not found")
    names = {"restarts": 1, "recovers": 2, "faults": 3}
    if name not in names:
        raise AssertionError(f"unknown service counter: {name}")
    return int(match.group(names[name]))


def run_test(args: argparse.Namespace) -> None:
    shell = SerialShell(args.port, args.baud, args.prompt, args.timeout)
    shell.debug_raw = args.debug_raw
    try:
        time.sleep(args.startup_wait)
        try:
            shell.sync_prompt()
        except AssertionError:
            if args.debug_raw:
                print("[sync] prompt not found; continuing with command-driven reads")

        status = shell.command("svc runtime status", r"User service runtime[\s\S]*(state:\s+|task:\s+)")
        if not re.search(r"(state:\s+stopped|task:\s+none)", status):
            shell.command("svc runtime stop", r"(User service runtime stopped|svc runtime: already stopped)")
            shell.command("svc runtime status", r"User service runtime[\s\S]*(state:\s+stopped|task:\s+none)")
        shell.command(f"svc runtime start {args.period}", r"User service runtime started[\s\S]*mode:\s+tick-only")

        time.sleep(args.tick_wait)
        first = extract_ticks(shell.command("svc runtime status", r"runtime ticks:\s+[0-9]+"))
        if first == 0:
            raise AssertionError("runtime ticks did not advance after start")

        time.sleep(args.tick_wait)
        second = extract_ticks(shell.command("svc runtime status", r"runtime ticks:\s+[0-9]+"))
        if second <= first:
            raise AssertionError(f"runtime ticks did not increase: {first} -> {second}")

        shell.command(f"svc runtime start {args.period}", r"svc runtime: already running")
        shell.command("svc runtime stop", r"User service runtime stopped[\s\S]*task:\s+none")
        shell.command("svc runtime stop", r"svc runtime: already stopped")
        shell.command("svc runtime start 0", r"period must be 1\.\.10000 ticks")
        shell.command("svc runtime start 10001", r"period must be 1\.\.10000 ticks")

        shell.command(f"svc runtime start {args.period} health",
                      r"User service runtime started[\s\S]*mode:\s+health-sweep")
        time.sleep(args.tick_wait)
        health = shell.command("svc runtime status",
                               r"mode:\s+health-sweep[\s\S]*sweeps:\s+[0-9]+")
        sweeps = extract_counter(health, "sweeps")
        checks = extract_counter(health, "checks")
        if sweeps == 0:
            raise AssertionError("health sweeps did not advance")
        if checks == 0:
            raise AssertionError("health checks did not advance")
        shell.command("svc runtime stop", r"User service runtime stopped[\s\S]*task:\s+none")
        shell.command(f"svc runtime start {args.period} auto",
                      r"User service runtime started[\s\S]*mode:\s+auto-restart")
        time.sleep(args.tick_wait)
        auto = shell.command("svc runtime status",
                             r"mode:\s+auto-restart[\s\S]*actions:\s+[0-9]+")
        actions = extract_counter(auto, "actions")
        if actions != 0:
            raise AssertionError(f"auto mode acted without auto policy: {actions}")
        shell.command("svc runtime stop", r"User service runtime stopped[\s\S]*task:\s+none")

        shell.command("svc start dev.uart0", r"User driver stack ready[\s\S]*last health:\s+ok")
        shell.command("svc policy dev.uart0 auto 2",
                      r"User service policy updated[\s\S]*policy:\s+auto")
        shell.command("svc clear dev.uart0",
                      r"User service counters cleared[\s\S]*faults:\s+0")
        shell.command("svc fault dev.uart0",
                      r"User driver UART service fault injected[\s\S]*last health:\s+fault")
        shell.command(f"svc runtime start {args.period} auto",
                      r"User service runtime started[\s\S]*mode:\s+auto-restart")
        time.sleep(args.tick_wait)
        recovered = shell.command("svc runtime status",
                                  r"mode:\s+auto-restart[\s\S]*actions:\s+[1-9][0-9]*")
        recover_actions = extract_counter(recovered, "actions")
        if recover_actions == 0:
            raise AssertionError("auto mode did not restart faulted service")
        status = shell.command("svc", r"dev\.uart0[\s\S]*health:\s+ok")
        restarts = extract_service_counter(status, "dev.uart0", "restarts")
        if restarts == 0:
            raise AssertionError("dev.uart0 restart counter did not increase")
        shell.command("svc runtime stop", r"User service runtime stopped[\s\S]*task:\s+none")
        shell.command("svc reset dev.uart0", r"User service supervisor reset")
        shell.command("svc stop dev.uart0", r"User driver stack stopped")
        shell.command(f"svc runtime start {args.period} unknown",
                      r"svc runtime: mode must be tick, health, or auto")
    finally:
        try:
            shell.command("svc runtime stop",
                          r"(User service runtime stopped|svc runtime: already stopped)",
                          timeout=1.0)
        except Exception:
            pass
        shell.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="serial device, for example /dev/ttyACM0")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--prompt", default="my-rtos> ")
    parser.add_argument("--timeout", type=float, default=3.0)
    parser.add_argument("--startup-wait", type=float, default=0.2)
    parser.add_argument("--tick-wait", type=float, default=0.75)
    parser.add_argument("--period", type=int, default=5)
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--debug-raw", action="store_true")
    args = parser.parse_args()

    try:
        run_test(args)
    except Exception as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1

    print("PASS: svc runtime serial smoke test")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
