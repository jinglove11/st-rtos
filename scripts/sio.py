#!/usr/bin/env python3
"""
sio.py — 串口交互工具 (AI 实时调试用)
基于 os.open + termios (和 cap.py 同底层,可靠重连)

用法:
  读 3 秒:     python3 scripts/sio.py read 3
  发命令:       python3 scripts/sio.py send "ls /"
  发+等+读:     python3 scripts/sio.py interact "help" 3
  持续监听:     python3 scripts/sio.py monitor
"""

import sys, os, time, select, termios

PORT = os.environ.get("SIO_PORT", "/dev/ttyACM0")
BAUD = int(os.environ.get("SIO_BAUD", "115200"))

def open_port():
    """打开串口 (带重试,reset 后 USB CDC 会断开)"""
    for _ in range(20):
        try:
            fd = os.open(PORT, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
            a = termios.tcgetattr(fd)
            a[0] = 0; a[1] = 0
            speed = {115200: termios.B115200, 921600: termios.B921600}.get(BAUD, termios.B115200)
            a[2] = speed | termios.CS8 | termios.CREAD | termios.CLOCAL
            a[3] = 0
            a[6][termios.VMIN] = 0; a[6][termios.VTIME] = 0
            termios.tcsetattr(fd, termios.TCSANOW, a)
            termios.tcflush(fd, termios.TCIOFLUSH)
            return fd
        except OSError:
            time.sleep(0.3)
    raise RuntimeError(f"cannot open {PORT}")

def read_fd(fd, seconds):
    """非阻塞读 N 秒"""
    buf = bytearray()
    end = time.time() + seconds
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try:
                chunk = os.read(fd, 4096)
                buf += chunk
            except OSError:
                pass
    return bytes(buf)

def cmd_read(seconds):
    fd = open_port()
    data = read_fd(fd, seconds)
    os.close(fd)
    sys.stdout.buffer.write(data)
    sys.stdout.flush()

def cmd_send(line):
    fd = open_port()
    os.write(fd, (line + "\r\n").encode())
    time.sleep(0.3)
    data = read_fd(fd, 1.0)
    os.close(fd)
    sys.stdout.buffer.write(data)
    sys.stdout.flush()

def cmd_interact(line, wait):
    fd = open_port()
    # 先清空旧缓冲
    read_fd(fd, 0.2)
    # 发命令
    os.write(fd, (line + "\r\n").encode())
    time.sleep(0.1)
    # 读 wait 秒
    data = read_fd(fd, wait)
    os.close(fd)
    sys.stdout.buffer.write(data)
    sys.stdout.flush()

def cmd_monitor():
    fd = None
    while True:
        if fd is None:
            try:
                fd = open_port()
            except RuntimeError:
                time.sleep(0.5)
                continue
        try:
            r, _, _ = select.select([fd], [], [], 0.5)
            if r:
                data = os.read(fd, 4096)
                if data:
                    sys.stdout.buffer.write(data)
                    sys.stdout.flush()
        except OSError:
            try: os.close(fd)
            except: pass
            fd = None
            time.sleep(0.3)

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(0)
    mode = sys.argv[1]
    if mode == "read":
        cmd_read(float(sys.argv[2]) if len(sys.argv) > 2 else 2.0)
    elif mode == "send":
        cmd_send(sys.argv[2] if len(sys.argv) > 2 else "")
    elif mode == "interact":
        cmd_interact(sys.argv[2], float(sys.argv[3]) if len(sys.argv) > 3 else 3.0)
    elif mode == "monitor":
        cmd_monitor()
