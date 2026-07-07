#!/usr/bin/env python3
"""One-shot UART capture: open /dev/ttyACMx, read until 'All tests PASSED'
or timeout, tolerating reset-induced fd loss (reopen). Output to a file."""
import os, sys, select, termios, time, errno

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM1"
BAUD = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
OUT  = sys.argv[3] if len(sys.argv) > 3 else "/tmp/uart_cap.log"
DEADLINE_S = float(sys.argv[4]) if len(sys.argv) > 4 else 60.0

def open_port():
    fd = os.open(PORT, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[0]=0; a[1]=0
    speed = {115200: termios.B115200, 921600: termios.B921600}.get(BAUD, termios.B115200)
    a[2] = speed | termios.CS8 | termios.CREAD | termios.CLOCAL
    a[3] = 0
    a[6][termios.VMIN]=0; a[6][termios.VTIME]=0
    termios.tcsetattr(fd, termios.TCSANOW, a)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd

data = bytearray()
end = time.time() + DEADLINE_S
fd = None
while time.time() < end:
    if fd is None:
        try:
            fd = open_port()
        except OSError:
            time.sleep(0.3)
            continue
    try:
        r,_,_ = select.select([fd], [], [], 0.5)
    except (OSError, ValueError):
        try: os.close(fd)
        except OSError: pass
        fd = None
        time.sleep(0.3)
        continue
    if not r:
        continue
    try:
        chunk = os.read(fd, 4096)
    except BlockingIOError:
        continue
    except OSError:
        try: os.close(fd)
        except OSError: pass
        fd = None
        time.sleep(0.3)
        continue
    if chunk:
        data.extend(chunk)

if fd is not None:
    try: os.close(fd)
    except OSError: pass

with open(OUT, "wb") as f:
    f.write(data)
sys.stderr.write(f"captured {len(data)} bytes -> {OUT}\n")
