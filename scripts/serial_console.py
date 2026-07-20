#!/usr/bin/env python3
"""
serial_console.py — AI 友好的串口交互工具

用法:
  读 2 秒输出:   python3 scripts/serial_console.py read 2
  发送命令:       python3 scripts/serial_console.py send "ls /"
  读+发+读:       python3 scripts/serial_console.py interact "reset" 5
  持续监听:       python3 scripts/serial_console.py monitor

特点:
  - read: 非阻塞读,超时后输出所有收到的文本
  - send: 发送一行命令 (+ \\r\\n),自动读 0.5s 回显
  - interact: send 后等 N 秒读完整输出
  - monitor: 持续读直到 Ctrl+C,实时打印
  - 自动重连 (USB CDC 重新枚举后自动恢复)
"""

import sys, os, time, select, termios, errno

PORT = "/dev/ttyACM0"
BAUD = 115200

def open_port():
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

def read_duration(fd, seconds):
    """非阻塞读 N 秒,返回所有文本"""
    data = bytearray()
    end = time.time() + seconds
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try:
                chunk = os.read(fd, 4096)
                data += chunk
            except OSError:
                pass
    return data.decode('utf-8', errors='replace')

def send_line(fd, line):
    """发送一行 + \\r\\n"""
    os.write(fd, (line + "\r\n").encode())
    time.sleep(0.05)
    termios.tcdrain(fd)

def cmd_read(seconds):
    """读 N 秒"""
    try:
        fd = open_port()
    except OSError as e:
        print(f"ERROR: cannot open {PORT}: {e}", file=sys.stderr)
        sys.exit(1)
    text = read_duration(fd, seconds)
    os.close(fd)
    sys.stdout.write(text)
    sys.stdout.flush()

def cmd_send(line):
    """发送命令,读 0.5s 回显"""
    try:
        fd = open_port()
    except OSError as e:
        print(f"ERROR: cannot open {PORT}: {e}", file=sys.stderr)
        sys.exit(1)
    send_line(fd, line)
    text = read_duration(fd, 0.5)
    os.close(fd)
    sys.stdout.write(text)
    sys.stdout.flush()

def cmd_interact(line, wait):
    """发送命令 + 等 N 秒读输出"""
    try:
        fd = open_port()
    except OSError as e:
        print(f"ERROR: cannot open {PORT}: {e}", file=sys.stderr)
        sys.exit(1)
    send_line(fd, line)
    time.sleep(0.1)
    text = read_duration(fd, wait)
    os.close(fd)
    sys.stdout.write(text)
    sys.stdout.flush()

def cmd_monitor():
    """持续监听"""
    fd = None
    try:
        while True:
            if fd is None:
                try:
                    fd = open_port()
                    print(f"[connected {PORT}]", file=sys.stderr)
                except OSError:
                    time.sleep(0.5)
                    continue
            try:
                r, _, _ = select.select([fd], [], [], 0.5)
                if r:
                    data = os.read(fd, 4096)
                    sys.stdout.write(data.decode('utf-8', errors='replace'))
                    sys.stdout.flush()
            except OSError:
                print(f"\n[disconnected, reconnecting...]", file=sys.stderr)
                try: os.close(fd)
                except: pass
                fd = None
                time.sleep(0.5)
    except KeyboardInterrupt:
        print("\n[stopped]", file=sys.stderr)
        if fd: os.close(fd)

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(0)

    mode = sys.argv[1]

    if mode == "read":
        seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 2.0
        cmd_read(seconds)

    elif mode == "send":
        line = sys.argv[2] if len(sys.argv) > 2 else ""
        cmd_send(line)

    elif mode == "interact":
        if len(sys.argv) < 3:
            print("Usage: interact <command> <wait_seconds>")
            sys.exit(1)
        line = sys.argv[2]
        wait = float(sys.argv[3]) if len(sys.argv) > 3 else 3.0
        cmd_interact(line, wait)

    elif mode == "monitor":
        cmd_monitor()

    else:
        print(f"Unknown mode: {mode}")
        print(__doc__)
