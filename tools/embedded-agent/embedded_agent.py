#!/usr/bin/env python3
"""Configuration-driven UART and debug-probe agent for embedded targets."""

import argparse
import base64
import fcntl
import json
import os
import re
import selectors
import signal
import socket
import socketserver
import subprocess
import sys
import termios
import threading
import time
from collections import deque
from pathlib import Path

DEFAULT_SOCKET = "/tmp/embedded-agent.sock"
DEFAULT_PORT = "/dev/ttyACM0"
DEFAULT_BAUD = 921600
DEFAULT_LOG = "/tmp/embedded-uart.log"
DEFAULT_PROMPT = r"(?:>|#|\$)\s*$"
MAX_REQUEST = 1024 * 1024


def load_config(path):
    if not path:
        return {}
    with open(path, "r", encoding="utf-8") as stream:
        value = json.load(stream)
    if not isinstance(value, dict):
        raise ValueError("agent config must be a JSON object")
    return value


class ByteRing:
    def __init__(self, capacity):
        self.capacity = capacity
        self.chunks = deque()
        self.size = 0
        self.total = 0

    def append(self, data):
        if not data:
            return
        data = bytes(data)
        self.chunks.append(data)
        self.size += len(data)
        self.total += len(data)
        while self.size > self.capacity and self.chunks:
            excess = self.size - self.capacity
            head = self.chunks[0]
            if len(head) <= excess:
                self.chunks.popleft()
                self.size -= len(head)
            else:
                self.chunks[0] = head[excess:]
                self.size -= excess

    def bytes(self):
        return b"".join(self.chunks)

    def since(self, cursor):
        retained_start = self.total - self.size
        data = self.bytes()
        if cursor <= retained_start:
            return data, retained_start
        if cursor >= self.total:
            return b"", cursor
        return data[cursor - retained_start :], cursor


class Agent:
    def __init__(self, port, baud, log_path, repo, ring_bytes,
                 prompt=DEFAULT_PROMPT, actions=None):
        self.port = port
        self.baud = baud
        self.log_path = Path(log_path)
        self.repo = Path(repo).resolve()
        self.prompt = prompt
        self.actions = actions or {}
        self.ring = ByteRing(ring_bytes)
        self.condition = threading.Condition()
        self.fd_lock = threading.Lock()
        self.hardware_lock = threading.Lock()
        self.fd = None
        self.connected = False
        self.last_error = None
        self.started_at = time.time()
        self.stop_event = threading.Event()
        self.reader = threading.Thread(target=self._serial_loop,
                                       name="uart-reader", daemon=True)

    def start(self):
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        self.log_path.write_bytes(b"")
        self.reader.start()

    def stop(self):
        self.stop_event.set()
        with self.fd_lock:
            if self.fd is not None:
                try:
                    os.close(self.fd)
                except OSError:
                    pass
                self.fd = None
        with self.condition:
            self.condition.notify_all()
        self.reader.join(timeout=2)

    def _speed(self):
        speed = getattr(termios, f"B{self.baud}", None)
        if speed is None:
            raise ValueError(f"host does not support baud rate {self.baud}")
        return speed

    def _open_serial(self):
        fd = os.open(self.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        attrs = termios.tcgetattr(fd)
        attrs[0] = 0
        attrs[1] = 0
        attrs[2] = self._speed() | termios.CS8 | termios.CREAD | termios.CLOCAL
        attrs[3] = 0
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        return fd

    def _set_connection(self, fd, error=None):
        with self.fd_lock:
            self.fd = fd
            self.connected = fd is not None
            self.last_error = error
        with self.condition:
            self.condition.notify_all()

    def _serial_loop(self):
        while not self.stop_event.is_set():
            try:
                fd = self._open_serial()
                self._set_connection(fd)
            except (OSError, ValueError) as exc:
                self._set_connection(None, str(exc))
                self.stop_event.wait(0.5)
                continue

            selector = selectors.DefaultSelector()
            try:
                selector.register(fd, selectors.EVENT_READ)
                with self.log_path.open("ab", buffering=0) as log:
                    while not self.stop_event.is_set():
                        if not selector.select(timeout=0.5):
                            continue
                        try:
                            data = os.read(fd, 4096)
                        except BlockingIOError:
                            continue
                        if not data:
                            continue
                        log.write(data)
                        with self.condition:
                            self.ring.append(data)
                            self.condition.notify_all()
            except OSError as exc:
                self._set_connection(None, str(exc))
            finally:
                selector.close()
                with self.fd_lock:
                    if self.fd == fd:
                        self.fd = None
                        self.connected = False
                try:
                    os.close(fd)
                except OSError:
                    pass
            self.stop_event.wait(0.2)

    def status(self):
        with self.condition:
            total = self.ring.total
            retained = self.ring.size
        return {
            "connected": self.connected,
            "port": self.port,
            "baud": self.baud,
            "log": str(self.log_path),
            "bytes_total": total,
            "bytes_retained": retained,
            "uptime_seconds": round(time.time() - self.started_at, 3),
            "last_error": self.last_error,
            "prompt": self.prompt,
            "actions": sorted(self.actions),
        }

    def tail(self, max_bytes=8192):
        max_bytes = max(1, min(int(max_bytes), self.ring.capacity))
        with self.condition:
            data = self.ring.bytes()[-max_bytes:]
            cursor = self.ring.total
        return self._render(data, cursor)

    @staticmethod
    def _render(data, cursor):
        return {
            "cursor": cursor,
            "text": data.decode("utf-8", errors="replace"),
            "data_base64": base64.b64encode(data).decode("ascii"),
        }

    def write(self, data):
        with self.fd_lock:
            if self.fd is None:
                raise RuntimeError("serial port is not connected")
            written = os.write(self.fd, data)
        return {"bytes_written": written}

    def command(self, command, timeout=5.0, pattern=None):
        if not isinstance(command, str) or "\n" in command or "\r" in command:
            raise ValueError("command must be one line")
        regex = re.compile(pattern or self.prompt, re.MULTILINE)
        with self.condition:
            cursor = self.ring.total
        self.write(command.encode("utf-8") + b"\r")
        return self.wait(regex, cursor, timeout)

    def wait(self, regex, cursor, timeout):
        deadline = time.monotonic() + max(0.0, float(timeout))
        with self.condition:
            while True:
                data, actual_start = self.ring.since(int(cursor))
                text = data.decode("utf-8", errors="replace")
                match = regex.search(text)
                if match:
                    result = self._render(data, self.ring.total)
                    result.update({"matched": True, "match": match.group(0),
                                   "truncated": actual_start > cursor})
                    return result
                remaining = deadline - time.monotonic()
                if remaining <= 0 or self.stop_event.is_set():
                    result = self._render(data, self.ring.total)
                    result.update({"matched": False,
                                   "truncated": actual_start > cursor})
                    return result
                self.condition.wait(min(remaining, 0.5))

    def run_board_action(self, action, timeout):
        command = self.actions.get(action)
        if not isinstance(command, list) or not command or not all(
                isinstance(item, str) and item for item in command):
            raise ValueError(f"board action is not configured: {action}")
        env = os.environ.copy()
        env.update({"PORT": self.port, "BAUD": str(self.baud),
                    "EMBEDDED_AGENT_PORT": self.port,
                    "EMBEDDED_AGENT_BAUD": str(self.baud)})
        with self.hardware_lock:
            proc = subprocess.run(command, cwd=self.repo,
                                  text=True, capture_output=True,
                                  timeout=float(timeout), check=False, env=env)
        output = proc.stdout + proc.stderr
        return {"action": action, "returncode": proc.returncode,
                "output": output[-50000:]}

    def dispatch(self, request):
        op = request.get("op")
        if op == "status":
            return self.status()
        if op == "tail":
            return self.tail(request.get("max_bytes", 8192))
        if op == "write":
            data = base64.b64decode(request["data_base64"], validate=True)
            return self.write(data)
        if op == "command":
            return self.command(request["command"], request.get("timeout", 5),
                                request.get("pattern"))
        if op == "wait":
            regex = re.compile(request["pattern"], re.MULTILINE)
            return self.wait(regex, request.get("cursor", self.ring.total),
                             request.get("timeout", 5))
        if op in ("flash", "reset", "verify"):
            return self.run_board_action(op, request.get("timeout", 120))
        raise ValueError(f"unknown operation: {op}")


class RequestHandler(socketserver.StreamRequestHandler):
    def handle(self):
        line = self.rfile.readline(MAX_REQUEST + 1)
        if len(line) > MAX_REQUEST:
            return self._reply({"ok": False, "error": "request too large"})
        try:
            request = json.loads(line)
            result = self.server.agent.dispatch(request)
            self._reply({"ok": True, "result": result})
        except Exception as exc:
            self._reply({"ok": False, "error": str(exc),
                         "error_type": type(exc).__name__})

    def _reply(self, value):
        self.wfile.write(json.dumps(value, ensure_ascii=False).encode() + b"\n")


class AgentServer(socketserver.ThreadingUnixStreamServer):
    daemon_threads = True

    def __init__(self, path, agent):
        self.agent = agent
        super().__init__(path, RequestHandler)


def rpc(socket_path, request, timeout=130):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.settimeout(timeout)
        sock.connect(socket_path)
        sock.sendall(json.dumps(request).encode() + b"\n")
        chunks = bytearray()
        while b"\n" not in chunks:
            data = sock.recv(65536)
            if not data:
                break
            chunks.extend(data)
    response = json.loads(bytes(chunks).split(b"\n", 1)[0])
    if not response.get("ok"):
        raise RuntimeError(response.get("error", "agent request failed"))
    return response["result"]


def daemon_main(args):
    config = load_config(args.config)
    socket_path = args.socket or config.get("socket", DEFAULT_SOCKET)
    port = args.port or config.get("port", DEFAULT_PORT)
    baud = args.baud or int(config.get("baud", DEFAULT_BAUD))
    log_path = args.log or config.get("log", DEFAULT_LOG)
    repo_value = args.repo or config.get("working_directory") or str(
        Path(__file__).resolve().parents[2])
    repo_path = Path(repo_value)
    if not repo_path.is_absolute() and args.config:
        repo_path = Path(args.config).resolve().parent / repo_path
    repo = str(repo_path.resolve())
    ring_bytes = args.ring_bytes or int(config.get("ring_bytes", 1024 * 1024))
    prompt = args.prompt or config.get("prompt", DEFAULT_PROMPT)
    actions = config.get("actions", {})

    lock_path = socket_path + ".lock"
    lock_fd = os.open(lock_path, os.O_CREAT | os.O_RDWR, 0o600)
    try:
        fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        raise SystemExit("embedded agent is already running")
    try:
        os.unlink(socket_path)
    except FileNotFoundError:
        pass

    agent = Agent(port, baud, log_path, repo, ring_bytes, prompt, actions)
    server = AgentServer(socket_path, agent)
    os.chmod(socket_path, 0o600)
    agent.start()

    def stop(_signum, _frame):
        threading.Thread(target=server.shutdown, daemon=True).start()

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    try:
        server.serve_forever(poll_interval=0.2)
    finally:
        server.server_close()
        agent.stop()
        try:
            os.unlink(socket_path)
        except FileNotFoundError:
            pass
        os.close(lock_fd)


def cli_main(args):
    config = load_config(args.config)
    socket_path = args.socket or config.get("socket", DEFAULT_SOCKET)
    request = {"op": args.operation}
    for name in ("max_bytes", "command", "pattern", "cursor", "timeout"):
        value = getattr(args, name, None)
        if value is not None:
            request[name] = value
    result = rpc(socket_path, request, timeout=(args.timeout or 120) + 10)
    if args.json:
        print(json.dumps(result, ensure_ascii=False, indent=2))
    elif "text" in result:
        print(result["text"], end="")
    elif "output" in result:
        print(result["output"], end="")
        raise SystemExit(result["returncode"])
    else:
        print(json.dumps(result, ensure_ascii=False, indent=2))


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="mode", required=True)
    daemon = sub.add_parser("daemon")
    daemon.add_argument("--config")
    daemon.add_argument("--socket")
    daemon.add_argument("--port")
    daemon.add_argument("--baud", type=int)
    daemon.add_argument("--log")
    daemon.add_argument("--repo")
    daemon.add_argument("--ring-bytes", type=int)
    daemon.add_argument("--prompt")

    cli = sub.add_parser("ctl")
    cli.add_argument("--config")
    cli.add_argument("--socket")
    cli.add_argument("--json", action="store_true")
    cli.add_argument("operation", choices=("status", "tail", "command", "wait",
                                            "flash", "reset", "verify"))
    cli.add_argument("command", nargs="?")
    cli.add_argument("--pattern")
    cli.add_argument("--cursor", type=int)
    cli.add_argument("--timeout", type=float)
    cli.add_argument("--max-bytes", type=int)
    return parser


def main():
    args = build_parser().parse_args()
    if args.mode == "daemon":
        daemon_main(args)
    else:
        if args.operation == "command" and args.command is None:
            raise SystemExit("command operation requires a command string")
        cli_main(args)


if __name__ == "__main__":
    main()
