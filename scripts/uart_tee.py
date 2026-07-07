#!/usr/bin/env python3
"""Background UART tee: /dev/ttyACM0 -> /tmp/uart.log.

Started by `make debug-uart-start`. Captures raw bytes from the Pico 2 W
UART (115200 8N1) and appends to /tmp/uart.log, flushing on every chunk
so a `Read /tmp/uart.log` from Claude sees fresh output promptly.

Truncates the log on start so each debug session is clean. SIGTERM/SIGINT
handled so `make debug-stop` works.
"""
import os
import signal
import sys

import serial

PORT = os.environ.get("UART_PORT", "/dev/ttyACM0")
BAUD = int(os.environ.get("UART_BAUD", "921600"))
LOG = os.environ.get("UART_LOG", "/tmp/uart.log")

running = True


def stop(signum, _frame):
    global running
    running = False
    sys.stderr.write(f"[uart_tee] signal {signum}, stopping\n")
    sys.stderr.flush()


signal.signal(signal.SIGTERM, stop)
signal.signal(signal.SIGINT, stop)

# Truncate on start so each session is readable from offset 0.
open(LOG, "wb").close()
sys.stderr.write(f"[uart_tee] {PORT}@{BAUD} -> {LOG}, pid={os.getpid()}\n")
sys.stderr.flush()

try:
    ser = serial.Serial(PORT, BAUD, timeout=1)
except Exception as e:
    sys.stderr.write(f"[uart_tee] open {PORT} failed: {e}\n")
    sys.exit(1)

while running:
    chunk = ser.read(4096)
    if chunk:
        with open(LOG, "ab") as f:
            f.write(chunk)
            f.flush()

ser.close()
sys.stderr.write("[uart_tee] stopped\n")
