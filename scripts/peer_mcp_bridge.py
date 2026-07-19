#!/usr/bin/env python3
"""MCP stdio bridge → nanobot peer bus on the robot.

Other Grok sessions on BlackCube can load this as an MCP server to call the
robot listener:

  [mcp_servers.nanobot_robot]
  command = "python3"
  args = ["/home/voldemar/Dev/nanobot/scripts/peer_mcp_bridge.py"]

Env:
  NANOBOT_PEER_URL   default http://192.168.1.88:8787
  NANOBOT_PEER_TOKEN optional shared secret (or file)
"""
from __future__ import annotations

import json
import os
import sys
import urllib.error
import urllib.request

BASE = os.environ.get("NANOBOT_PEER_URL", "http://192.168.1.88:8787").rstrip("/")
TOKEN = os.environ.get("NANOBOT_PEER_TOKEN", "")
if not TOKEN:
    for p in (
        os.path.expanduser("~/.nanobot_peer_token"),
        "/mnt/data/nanobot/peer_token",
    ):
        try:
            with open(p) as f:
                for line in f:
                    line = line.strip()
                    if line.startswith("token="):
                        TOKEN = line.split("=", 1)[1].strip()
                        break
                    if line and not line.startswith("#"):
                        TOKEN = line
                        break
        except OSError:
            pass
        if TOKEN:
            break


def read_message() -> dict | None:
    headers: dict[str, str] = {}
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            return None
        if line in (b"\r\n", b"\n"):
            break
        if b":" in line:
            k, v = line.decode("utf-8", "replace").split(":", 1)
            headers[k.strip().lower()] = v.strip()
    n = int(headers.get("content-length", "0"))
    body = sys.stdin.buffer.read(n) if n else b"{}"
    return json.loads(body.decode("utf-8"))


def write_message(msg: dict) -> None:
    data = json.dumps(msg, separators=(",", ":")).encode("utf-8")
    sys.stdout.buffer.write(f"Content-Length: {len(data)}\r\n\r\n".encode())
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.flush()


def http_json(method: str, path: str, payload: dict | None = None) -> dict:
    url = f"{BASE}{path}"
    data = None if payload is None else json.dumps(payload).encode()
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Content-Type", "application/json")
    if TOKEN:
        req.add_header("X-Nanobot-Peer-Token", TOKEN)
    try:
        with urllib.request.urlopen(req, timeout=180) as r:
            return json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", "replace")
        try:
            return json.loads(body)
        except Exception:
            return {"error": body or str(e), "http_status": e.code}
    except Exception as e:
        return {"error": str(e)}


TOOLS = [
    {
        "name": "robot_prompt",
        "description": "Send a prompt to nanobot on the robot (uses robot browser Grok session + shell tool).",
        "inputSchema": {
            "type": "object",
            "properties": {"prompt": {"type": "string"}},
            "required": ["prompt"],
        },
    },
    {
        "name": "robot_shell",
        "description": "Run a shell command on the robot via nanobot peer bus.",
        "inputSchema": {
            "type": "object",
            "properties": {"command": {"type": "string"}},
            "required": ["command"],
        },
    },
    {
        "name": "robot_info",
        "description": "Peer bus health/info from the robot nanobot listener.",
        "inputSchema": {"type": "object", "properties": {}},
    },
]


def main() -> None:
    while True:
        msg = read_message()
        if msg is None:
            break
        mid = msg.get("id")
        method = msg.get("method")
        if method == "initialize":
            write_message(
                {
                    "jsonrpc": "2.0",
                    "id": mid,
                    "result": {
                        "protocolVersion": "2024-11-05",
                        "capabilities": {"tools": {}},
                        "serverInfo": {"name": "nanobot-robot-peer", "version": "0.1.0"},
                    },
                }
            )
        elif method in ("notifications/initialized", "initialized", "ping"):
            if mid is not None:
                write_message({"jsonrpc": "2.0", "id": mid, "result": {}})
        elif method == "tools/list":
            write_message({"jsonrpc": "2.0", "id": mid, "result": {"tools": TOOLS}})
        elif method == "tools/call":
            params = msg.get("params") or {}
            name = params.get("name")
            args = params.get("arguments") or {}
            if name == "robot_info":
                out = http_json("GET", "/peer/v1/info")
            elif name == "robot_prompt":
                out = http_json("POST", "/peer/v1/prompt", {"prompt": args.get("prompt", "")})
            elif name == "robot_shell":
                out = http_json("POST", "/peer/v1/shell", {"command": args.get("command", "")})
            else:
                out = {"error": f"unknown tool {name}"}
            text = json.dumps(out, indent=2)
            write_message(
                {
                    "jsonrpc": "2.0",
                    "id": mid,
                    "result": {
                        "content": [{"type": "text", "text": text}],
                        "isError": bool(out.get("error")),
                    },
                }
            )
        elif method and method.startswith("notifications/"):
            pass
        elif mid is not None:
            write_message(
                {
                    "jsonrpc": "2.0",
                    "id": mid,
                    "error": {"code": -32601, "message": f"method not found: {method}"},
                }
            )


if __name__ == "__main__":
    main()
