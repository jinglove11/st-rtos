#!/usr/bin/env python3
"""Interactive serial debug bridge for the Pico 2 W (file-mediated).

Designed for asynchronous / AI-driven debugging. Runs as a long-lived
background daemon with three file interfaces instead of an interactive
terminal, so a caller can poke the board between tool invocations:

  /tmp/serial_out.log     — every byte received from the UART (appended,
                            flushed per chunk). Read this to see test output,
                            shell prompts, crash dumps, command replies.
  /tmp/serial_cmd.fifo    — a named pipe. Write one line to send a shell
                            command to the board:
                                echo "ps" > /tmp/serial_cmd.fifo
                            The line is forwarded to the UART with a CR.
  /tmp/serial_status.log  — connection lifecycle: connected / disconnected /
                            reconnected timestamps + each command echoed.

Why os-level termios instead of pyserial: pyserial raises a spurious
SerialException ("device reports readiness to read but returned no data")
on this CDC-ACM port when the board simply has nothing to send, which a
naive reader mistakes for a disconnect and thrashes reconnects. The raw
os.open + select + read path returns b'' cleanly on idle and survives
board resets (the fd errors out only on a real USB re-enumeration, which
we handle by reopening).

Usage:
  python3 scripts/serial_debug.py                 # defaults
  python3 scripts/serial_debug.py --port /dev/ttyACM0 --baud 115200

Stop with SIGTERM / SIGINT (the fifo is left in place for reuse).
"""

import argparse
import errno
import fcntl
import os
import select
import signal
import stat
import sys
import termios
import threading
import time
import tty
from datetime import datetime

DEFAULT_PORT = "/dev/ttyACM0"
DEFAULT_BAUD = 921600
DEFAULT_OUT = "/tmp/serial_out.log"
DEFAULT_FIFO = "/tmp/serial_cmd.fifo"
DEFAULT_STATUS = "/tmp/serial_status.log"

PORT_RECONNECT_DELAY = 0.5

running = True


def _ts():
    return datetime.now().strftime("%H:%M:%S")


class Status:
    def __init__(self, path):
        self.path = path
        self._lock = threading.Lock()
        open(path, "w").close()

    def write(self, msg):
        line = f"[{_ts()}] {msg}\n"
        with self._lock:
            with open(self.path, "a") as f:
                f.write(line)
                f.flush()
        sys.stderr.write(line)
        sys.stderr.flush()


def open_port(port, baud, status):
    """Open the tty raw at the given baud. Retries until success or shutdown.

    Returns an os-level fd (O_RDWR | O_NOCTTY | O_NONBLOCK)."""
    while running:
        try:
            fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        except OSError as e:
            status.write(f"open failed ({e}); retry in {PORT_RECONNECT_DELAY}s")
            _sleep(PORT_RECONNECT_DELAY)
            continue
        try:
            attrs = termios.tcgetattr(fd)
            attrs[0] = 0  # iflag: raw
            attrs[1] = 0  # oflag: raw
            attrs[2] = termios.B115200 if baud == 115200 else _cfsetspeed(attrs, baud)
            attrs[2] |= termios.CS8 | termios.CREAD | termios.CLOCAL
            attrs[3] = 0  # lflag: raw (no echo, no canonical)
            attrs[6][termios.VMIN] = 0
            attrs[6][termios.VTIME] = 0
            termios.tcsetattr(fd, termios.TCSANOW, attrs)
            termios.tcflush(fd, termios.TCIOFLUSH)
        except Exception as e:
            status.write(f"termios setup failed ({e}); retry")
            try:
                os.close(fd)
            except OSError:
                pass
            _sleep(PORT_RECONNECT_DELAY)
            continue
        status.write(f"connected {port}@{baud} fd={fd}")
        return fd
    return -1


def _cfsetspeed(attrs, baud):
    """Map a baud rate to a termios speed constant for cflag."""
    table = {50: termios.B50, 75: termios.B75, 110: termios.B110,
             134: termios.B134, 150: termios.B150, 200: termios.B200,
             300: termios.B300, 600: termios.B600, 1200: termios.B1200,
             1800: termios.B1800, 2400: termios.B2400, 4800: termios.B4800,
             9600: termios.B9600, 19200: termios.B19200, 38400: termios.B38400,
             57600: termios.B57600, 115200: termios.B115200, 230400: termios.B230400}
    return table.get(baud, termios.B115200)


def reader_loop(port, baud, out_path, status, stop_evt, holder):
    """Drain UART -> out_path. Reopens on real disconnect (USB re-enumeration).

    holder is a shared dict {"fd": int|None}; the writer reads from it under
    the fd_ready event. On reopen we clear holder["fd"] so the writer pauses."""
    while running and not stop_evt.is_set():
        fd = open_port(port, baud, status)
        if fd < 0:
            break
        holder["fd"] = fd
        try:
            with open(out_path, "ab") as out:
                while running and not stop_evt.is_set():
                    try:
                        r, _, _ = select.select([fd], [], [], 0.5)
                    except (OSError, ValueError):
                        # fd closed under us — treat as disconnect.
                        break
                    if not r:
                        continue
                    try:
                        chunk = os.read(fd, 4096)
                    except BlockingIOError:
                        # O_NONBLOCK + select wakeup with nothing ready yet.
                        continue
                    except OSError as e:
                        status.write(f"read error ({e}); reconnecting")
                        break
                    if chunk:
                        out.write(chunk)
                        out.flush()
                    # empty chunk on a readable fd: on CDC-ACM this is an
                    # unreliable signal (happens both on idle blips and real
                    # disconnects). Ignore it here; real disconnects surface
                    # as OSError on the next read/select, or as repeated
                    # BlockingIOError. The board_reset.sh helper handles the
                    # known reset case by restarting this tool, so this reader
                    # only needs to cope with surprises — and idling on an
                    # empty read is safer than thrashing reconnects.
        finally:
            holder["fd"] = None
            try:
                os.close(fd)
            except OSError:
                pass
        if running and not stop_evt.is_set():
            status.write("port lost; reconnecting")
            _sleep(PORT_RECONNECT_DELAY)

    status.write("reader exit")


def writer_loop(fifo_path, status, holder, stop_evt):
    """Read commands from the fifo and forward them to the UART.

    Commands are dropped (with a status note) if the reader has no port open;
    this keeps the fifo drainable during reconnect windows."""
    if os.path.exists(fifo_path):
        if not stat.S_ISFIFO(os.stat(fifo_path).st_mode):
            try:
                os.remove(fifo_path)
            except OSError as e:
                status.write(f"remove stale non-fifo ({e})")
                return
    if not os.path.exists(fifo_path):
        try:
            os.mkfifo(fifo_path)
        except OSError as e:
            status.write(f"mkfifo failed ({e})")
            return

    while running and not stop_evt.is_set():
        try:
            with open(fifo_path, "r") as fifo:
                status.write("fifo ready")
                for raw in fifo:
                    if stop_evt.is_set() or not running:
                        break
                    cmd = raw.rstrip("\r\n")
                    if not cmd:
                        continue
                    fd = holder.get("fd")
                    if fd is None or fd < 0:
                        status.write(f"cmd dropped (port down): {cmd}")
                        continue
                    try:
                        os.write(fd, (cmd + "\r").encode("utf-8"))
                        status.write(f"cmd: {cmd}")
                    except OSError as e:
                        status.write(f"cmd write failed ({e}): {cmd}")
        except OSError as e:
            if e.errno == errno.EINTR:
                continue
            status.write(f"fifo open error ({e}); retry")
            _sleep(0.2)

    status.write("writer exit")


def _sleep(seconds):
    end = time.time() + seconds
    while running and time.time() < end:
        time.sleep(min(0.1, max(0.0, end - time.time())))


def handle_signal(signum, _frame):
    global running
    running = False
    sys.stderr.write(f"[serial_debug] signal {signum}, stopping\n")
    sys.stderr.flush()


def main():
    global running
    ap = argparse.ArgumentParser(description="File-mediated serial debug bridge.")
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--fifo", default=DEFAULT_FIFO)
    ap.add_argument("--status", default=DEFAULT_STATUS)
    args = ap.parse_args()

    signal.signal(signal.SIGTERM, handle_signal)
    signal.signal(signal.SIGINT, handle_signal)

    status = Status(args.status)
    open(args.out, "wb").close()
    status.write(
        f"starting port={args.port} baud={args.baud} out={args.out} fifo={args.fifo}")

    stop_evt = threading.Event()
    holder = {"fd": None}

    rt = threading.Thread(target=reader_loop,
                          args=(args.port, args.baud, args.out, status, stop_evt, holder),
                          name="reader", daemon=True)
    wt = threading.Thread(target=writer_loop,
                          args=(args.fifo, status, holder, stop_evt),
                          name="writer", daemon=True)
    rt.start()
    wt.start()

    try:
        while running:
            time.sleep(0.5)
    finally:
        running = False
        stop_evt.set()
        # Poke the fifo so the writer's blocking open/read unblocks cleanly.
        try:
            with open(args.fifo, "w") as f:
                f.write("\n")
        except Exception:
            pass
        rt.join(timeout=3)
        wt.join(timeout=3)
        status.write("stopped")


if __name__ == "__main__":
    main()
