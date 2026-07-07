#!/usr/bin/env python3
"""Dependency-free MCP stdio adapter for the embedded hardware agent."""

import argparse
import json
import sys

from embedded_agent import DEFAULT_SOCKET, load_config, rpc

TOOLS = [
    {
        "name": "board_status",
        "description": "Get UART connection state, baud rate and log counters.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "serial_tail",
        "description": "Read the latest UART output without consuming it.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "max_bytes": {"type": "integer", "minimum": 1,
                              "maximum": 1048576, "default": 8192}
            },
        },
    },
    {
        "name": "serial_command",
        "description": "Send one target-console command and wait for its response.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "command": {"type": "string"},
                "timeout": {"type": "number", "minimum": 0, "default": 5},
                "pattern": {"type": "string",
                            "description": "Override the prompt regex from config."},
            },
            "required": ["command"],
        },
    },
    {
        "name": "serial_wait",
        "description": "Wait until UART output matches a regular expression.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "pattern": {"type": "string"},
                "cursor": {"type": "integer", "minimum": 0},
                "timeout": {"type": "number", "minimum": 0, "default": 30},
            },
            "required": ["pattern"],
        },
    },
    {
        "name": "board_verify",
        "description": "Run the configured verify action for the target project.",
        "inputSchema": {"type": "object", "properties": {
            "timeout": {"type": "number", "minimum": 1, "default": 120}
        }},
    },
    {
        "name": "board_flash",
        "description": "Run the configured firmware flash action.",
        "inputSchema": {"type": "object", "properties": {
            "timeout": {"type": "number", "minimum": 1, "default": 180}
        }},
    },
    {
        "name": "board_reset",
        "description": "Run the configured target reset action.",
        "inputSchema": {"type": "object", "properties": {
            "timeout": {"type": "number", "minimum": 1, "default": 30}
        }},
    },
]

TOOL_OPS = {
    "board_status": "status",
    "serial_tail": "tail",
    "serial_command": "command",
    "serial_wait": "wait",
    "board_verify": "verify",
    "board_flash": "flash",
    "board_reset": "reset",
}


def response(request_id, result=None, error=None):
    value = {"jsonrpc": "2.0", "id": request_id}
    if error is not None:
        value["error"] = error
    else:
        value["result"] = result
    sys.stdout.write(json.dumps(value, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def call_tool(socket_path, params):
    name = params.get("name")
    if name not in TOOL_OPS:
        raise ValueError(f"unknown tool: {name}")
    arguments = params.get("arguments") or {}
    request = dict(arguments)
    request["op"] = TOOL_OPS[name]
    timeout = float(request.get("timeout", 120)) + 10
    result = rpc(socket_path, request, timeout=timeout)
    text = result.get("text") if isinstance(result, dict) else None
    if text is None:
        text = json.dumps(result, ensure_ascii=False, indent=2)
    return {
        "content": [{"type": "text", "text": text}],
        "structuredContent": result,
        "isError": bool(isinstance(result, dict) and
                        result.get("returncode", 0) != 0),
    }


def serve(socket_path):
    for line in sys.stdin:
        try:
            message = json.loads(line)
        except json.JSONDecodeError:
            continue
        request_id = message.get("id")
        if request_id is None:
            continue
        method = message.get("method")
        try:
            if method == "initialize":
                result = {
                    "protocolVersion": "2024-11-05",
                    "capabilities": {"tools": {"listChanged": False}},
                    "serverInfo": {"name": "embedded-hardware-agent",
                                   "version": "1.0.0"},
                }
            elif method == "ping":
                result = {}
            elif method == "tools/list":
                result = {"tools": TOOLS}
            elif method == "tools/call":
                result = call_tool(socket_path, message.get("params") or {})
            else:
                response(request_id, error={"code": -32601,
                                            "message": "Method not found"})
                continue
            response(request_id, result=result)
        except Exception as exc:
            response(request_id, error={"code": -32000, "message": str(exc)})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config")
    parser.add_argument("--socket")
    args = parser.parse_args()
    config = load_config(args.config)
    serve(args.socket or config.get("socket", DEFAULT_SOCKET))


if __name__ == "__main__":
    main()
