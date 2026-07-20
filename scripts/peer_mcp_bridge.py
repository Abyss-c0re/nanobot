#!/usr/bin/env python3
"""stdio MCP bridge → any nanobot peer HTTP bus (product-agnostic).

NDJSON (also Content-Length). No vacuum/ dependency.
URL: NANOBOT_PEER_URL | ~/.nanobot/peer_url | http://127.0.0.1:8787
token: NANOBOT_PEER_TOKEN | ~/.nanobot/peer_token (reread each call)
Tools: nanobot_* (* accepted as legacy aliases).
"""
from __future__ import annotations

import json
import os
import sys
import urllib.error
import urllib.request

_TOKEN_FILES = (
    os.path.expanduser("~/.nanobot/peer_token"),
    os.path.expanduser("~/.nanobot/peer_token"),
    os.path.expanduser("~/.nanobot_peer_token"),
    os.path.expanduser("~/.nanobot_peer_token"),
)
_URL_FILES = (
    os.path.expanduser("~/.nanobot/peer_url"),
    os.path.expanduser("~/.nanobot/peer_url"),
)


def _read_first_line(path: str) -> str:
    try:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                return line
    except OSError:
        pass
    return ""


def peer_base() -> str:
    """Reload each call so peer_url file / env changes apply without MCP restart."""
    base = (
        os.environ.get("NANOBOT_PEER_URL")
        or os.environ.get("NANOBOT_PEER_URL")
        or ""
    ).strip()
    if not base:
        for p in _URL_FILES:
            line = _read_first_line(p)
            if line:
                base = line
                break
    if not base:
        base = "http://127.0.0.1:8787"
    return base.rstrip("/")


def peer_token() -> str:
    """Reload each call so rotated ~/.nanobot/peer_token is picked up live."""
    tok = (
        os.environ.get("NANOBOT_PEER_TOKEN")
        or os.environ.get("NANOBOT_PEER_TOKEN")
        or ""
    ).strip()
    if tok:
        if tok.startswith("token="):
            tok = tok.split("=", 1)[1].strip()
        return tok
    for p in _TOKEN_FILES:
        line = _read_first_line(p)
        if not line:
            continue
        if line.startswith("token="):
            return line.split("=", 1)[1].strip()
        return line
    return ""


def read_message() -> dict | None:
    line = sys.stdin.buffer.readline()
    if not line:
        return None
    if line.lower().startswith(b"content-length:"):
        headers: dict[str, str] = {}
        while True:
            if line in (b"\r\n", b"\n"):
                break
            if b":" in line:
                k, v = line.decode("utf-8", "replace").split(":", 1)
                headers[k.strip().lower()] = v.strip()
            line = sys.stdin.buffer.readline()
            if not line:
                return None
        n = int(headers.get("content-length", "0"))
        body = sys.stdin.buffer.read(n) if n else b"{}"
        return json.loads(body.decode("utf-8"))
    text = line.decode("utf-8", "replace").strip()
    if not text:
        return read_message()
    return json.loads(text)


def write_message(msg: dict) -> None:
    sys.stdout.write(json.dumps(msg, ensure_ascii=False, separators=(",", ":")) + "\n")
    sys.stdout.flush()


def http_json(method: str, path: str, payload: dict | None = None, timeout: float = 30) -> dict:
    url = f"{peer_base()}{path}"
    data = None if payload is None else json.dumps(payload).encode()
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Content-Type", "application/json")
    tok = peer_token()
    if tok:
        req.add_header("X-Nanobot-Peer-Token", tok)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", "replace")
        try:
            return json.loads(body)
        except Exception:
            return {"error": body or str(e), "http_status": e.code}
    except Exception as e:
        return {"error": str(e)}


def start_job(kind: str, prompt: str = "", command: str = "") -> dict:
    """Fire-and-poll: return immediately with job id, then optional short wait."""
    body: dict = {"kind": kind}
    if prompt:
        body["prompt"] = prompt
    if command:
        body["command"] = command
    ack = http_json("POST", "/peer/v1/jobs", body, timeout=8)
    jid = ack.get("id")
    if not jid:
        return ack
    # brief poll so simple callers still get a result without blocking long
    import time

    for _ in range(12):
        time.sleep(0.4)
        st = http_json("GET", f"/peer/v1/jobs/{jid}", timeout=5)
        if st.get("status") in ("done", "error"):
            return st
    return {"ok": True, "id": jid, "status": "running", "poll": f"/peer/v1/jobs/{jid}", "queued": ack}


TOOLS = [
    {
        "name": "nanobot_prompt",
        "description": "Queue a prompt on the nanobot peer (async). Returns job id; poll if running.",
        "inputSchema": {
            "type": "object",
            "properties": {"prompt": {"type": "string"}},
            "required": ["prompt"],
        },
    },
    {
        "name": "nanobot_shell",
        "description": "Queue a shell command on the peer (async; needs shell_enabled).",
        "inputSchema": {
            "type": "object",
            "properties": {"command": {"type": "string"}},
            "required": ["command"],
        },
    },
    {
        "name": "nanobot_job_status",
        "description": "Poll async job by id from nanobot_prompt/shell.",
        "inputSchema": {
            "type": "object",
            "properties": {"id": {"type": "string"}},
            "required": ["id"],
        },
    },
    {
        "name": "nanobot_info",
        "description": "Peer bus health/info.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "nanobot_control",
        "description": "Enable/disable shell or watcher on the peer.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "service": {"type": "string", "description": "shell|watcher"},
                "action": {"type": "string", "description": "on|off"},
            },
            "required": ["service", "action"],
        },
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
                        "serverInfo": {"name": "nanobot-peer", "version": "0.5.0"},
                    },
                }
            )
        elif method in ("notifications/initialized", "initialized"):
            continue
        elif method == "ping":
            if mid is not None:
                write_message({"jsonrpc": "2.0", "id": mid, "result": {}})
        elif method == "tools/list":
            write_message({"jsonrpc": "2.0", "id": mid, "result": {"tools": TOOLS}})
        elif method == "tools/call":
            params = msg.get("params") or {}
            name = params.get("name")
            args = params.get("arguments") or {}
            sync = os.environ.get("NANOBOT_PEER_SYNC", "").lower() in ("1", "true", "yes")
            if name in ("nanobot_info", "host_info", "info"):
                out = http_json("GET", "/peer/v1/info", timeout=8)
            elif name in ("nanobot_job_status", "job_status"):
                out = http_json("GET", f"/peer/v1/jobs/{args.get('id', '')}", timeout=8)
            elif name in ("nanobot_control", "control"):
                out = http_json(
                    "POST",
                    "/peer/v1/control",
                    {"service": args.get("service", ""), "action": args.get("action", "")},
                    timeout=8,
                )
            elif name in ("nanobot_prompt", "host_prompt", "prompt"):
                if sync:
                    out = http_json(
                        "POST", "/peer/v1/prompt", {"prompt": args.get("prompt", "")}, timeout=180
                    )
                else:
                    out = start_job("prompt", prompt=args.get("prompt", ""))
            elif name in ("nanobot_shell", "host_shell", "shell"):
                if sync:
                    out = http_json(
                        "POST",
                        "/peer/v1/shell",
                        {"command": args.get("command", "")},
                        timeout=120,
                    )
                else:
                    out = start_job("shell", command=args.get("command", ""))
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
        elif method == "shutdown":
            if mid is not None:
                write_message({"jsonrpc": "2.0", "id": mid, "result": {}})
            break
        else:
            if mid is not None:
                write_message(
                    {
                        "jsonrpc": "2.0",
                        "id": mid,
                        "error": {"code": -32601, "message": f"Method not found: {method}"},
                    }
                )


if __name__ == "__main__":
    main()
