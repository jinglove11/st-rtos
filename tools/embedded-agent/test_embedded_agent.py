#!/usr/bin/env python3
"""Host-side tests for the generic embedded agent."""

import os
import pty
import re
import errno
import tempfile
import threading
import time
import unittest

from embedded_agent import Agent, AgentServer, ByteRing, load_config, rpc


class ByteRingTest(unittest.TestCase):
    def test_retains_tail_and_tracks_cursor(self):
        ring = ByteRing(5)
        ring.append(b"abc")
        ring.append(b"def")
        self.assertEqual(ring.bytes(), b"bcdef")
        self.assertEqual(ring.total, 6)
        data, start = ring.since(3)
        self.assertEqual(data, b"def")
        self.assertEqual(start, 3)

    def test_loads_generic_project_config(self):
        with tempfile.NamedTemporaryFile("w", encoding="utf-8") as stream:
            stream.write('{"port":"/dev/test","actions":{"flash":["tool","go"]}}')
            stream.flush()
            config = load_config(stream.name)
        self.assertEqual(config["port"], "/dev/test")
        self.assertEqual(config["actions"]["flash"], ["tool", "go"])


class AgentIntegrationTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.master, slave = pty.openpty()
        self.port = os.ttyname(slave)
        os.close(slave)
        self.agent = Agent(self.port, 115200,
                           os.path.join(self.temp.name, "uart.log"),
                           os.getcwd(), 4096)
        self.agent.start()
        deadline = time.monotonic() + 2
        while not self.agent.connected and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertTrue(self.agent.connected, self.agent.last_error)

    def tearDown(self):
        self.agent.stop()
        os.close(self.master)
        self.temp.cleanup()

    def test_command_waits_for_prompt(self):
        def target():
            command = os.read(self.master, 128)
            self.assertEqual(command, b"svc stats\r")
            os.write(self.master, b"User service stats\r\nmy-rtos> ")

        thread = threading.Thread(target=target)
        thread.start()
        result = self.agent.command("svc stats", timeout=2)
        thread.join(timeout=1)
        self.assertTrue(result["matched"])
        self.assertIn("User service stats", result["text"])

    def test_wait_uses_cursor(self):
        cursor = self.agent.ring.total
        os.write(self.master, b"Passed: 2900\r\nFailed: 0\r\n")
        result = self.agent.wait(re.compile(r"Failed:\s+0"), cursor, 2)
        self.assertTrue(result["matched"])

    def test_unix_rpc(self):
        path = os.path.join(self.temp.name, "agent.sock")
        try:
            server = AgentServer(path, self.agent)
        except PermissionError as exc:
            if exc.errno == errno.EPERM:
                self.skipTest("sandbox disallows Unix domain sockets")
            raise
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            result = rpc(path, {"op": "status"})
            self.assertEqual(result["port"], self.port)
            self.assertTrue(result["connected"])
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=1)

    def test_structured_dispatch(self):
        result = self.agent.dispatch({"op": "status"})
        self.assertEqual(result["baud"], 115200)
        self.assertTrue(result["connected"])


if __name__ == "__main__":
    unittest.main()
