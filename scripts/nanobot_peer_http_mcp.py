#!/usr/bin/env python3
"""Minimal streamable MCP-over-HTTP for BlackCube nanobot peer (Grok remote MCP).

Env:
  NANOBOT_PEER_URL   default http://127.0.0.1:18787
  NANOBOT_PEER_TOKEN from env or ~/.nanobot/peer_token
  MCP_PORT           default 18790
"""
from __future__ import annotations

import json
import os
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

# Import BrainCube ecosystem helpers (canonical git path preferred)
_BC = None
for _cand in (
    __import__('os').environ.get('BRAINCUBE_BRIDGE'),
    str(Path(__file__).resolve().parent / 'braincube_bridge'),
    str(Path.home() / 'Dev/AI/nanobot/scripts/braincube_bridge'),
    str(Path.home() / 'Dev/lab/braincube_bridge'),
):
    if not _cand:
        continue
    _p = Path(_cand)
    if (_p / 'bc_api.py').is_file():
        import sys as _sys
        _sys.path.insert(0, str(_p))
        try:
            import bc_api as _BC  # type: ignore
            break
        except Exception:
            _BC = None

PORT = int(os.environ.get("MCP_PORT", "18790"))
PEER = (os.environ.get("NANOBOT_PEER_URL") or "http://127.0.0.1:18787").rstrip("/")


def peer_token() -> str:
    tok = (os.environ.get("NANOBOT_PEER_TOKEN") or "").strip()
    if tok:
        return tok[6:] if tok.startswith("token=") else tok
    p = Path(os.path.expanduser("~/.nanobot/peer_token"))
    try:
        line = p.read_text().strip().splitlines()[0].strip()
        return line[6:] if line.startswith("token=") else line
    except Exception:
        return ""


def peer_json(method: str, path: str, payload: dict | None = None, timeout: float = 120) -> dict:
    data = None if payload is None else json.dumps(payload).encode()
    req = urllib.request.Request(f"{PEER}{path}", data=data, method=method)
    req.add_header("Content-Type", "application/json")
    tok = peer_token()
    if tok:
        req.add_header("X-Nanobot-Peer-Token", tok)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode() or "{}")
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
        "name": "blackcube_nanobot_prompt",
        "description": "Run a prompt on BlackCube nanobot (Grok-auth agent host) for lab/dev help.",
        "inputSchema": {
            "type": "object",
            "properties": {"prompt": {"type": "string"}},
            "required": ["prompt"],
        },
    },
    {
        "name": "blackcube_nanobot_shell",
        "description": "Run a shell command on BlackCube via nanobot peer (token-gated).",
        "inputSchema": {
            "type": "object",
            "properties": {"command": {"type": "string"}},
            "required": ["command"],
        },
    },
    {
        "name": "blackcube_nanobot_info",
        "description": "BlackCube nanobot peer health/info (auth, model, version).",
        "inputSchema": {"type": "object", "properties": {}},
    },
]

if _BC is not None:
    TOOLS.extend([
        {"name": "braincube_info", "description": "Ecosystem BrainCube bridge: peer, LAW, organs.", "inputSchema": {"type": "object", "properties": {}}},
        {"name": "braincube_live", "description": "Live lattice snapshot for Neural Cube / commanders.", "inputSchema": {"type": "object", "properties": {}}},
        {"name": "braincube_law", "description": "First Cube LAW scoreboard only.", "inputSchema": {"type": "object", "properties": {}}},
        {"name": "braincube_export", "description": "Export chain_state (heavier).", "inputSchema": {"type": "object", "properties": {}}},
        {"name": "braincell_status", "description": "Braincell mini-hive cells + law.", "inputSchema": {"type": "object", "properties": {}}},
        {"name": "cubalc_run", "description": "Run allowlisted CubalC hive board or short source.", "inputSchema": {"type": "object", "properties": {"board": {"type": "string"}, "source": {"type": "string"}}}},
    ])

def handle_rpc(msg: dict) -> dict:
    mid = msg.get("id")
    method = msg.get("method")
    params = msg.get("params") or {}
    if method == "initialize":
        return {
            "jsonrpc": "2.0",
            "id": mid,
            "result": {
                "protocolVersion": "2024-11-05",
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "blackcube-nanobot", "version": "0.5.3"},
            },
        }
    if method == "notifications/initialized" or method == "initialized":
        return {"jsonrpc": "2.0", "id": mid, "result": {}}
    if method == "tools/list":
        return {"jsonrpc": "2.0", "id": mid, "result": {"tools": TOOLS}}
    if method == "tools/call":
        name = params.get("name") or ""
        args = params.get("arguments") or {}
        if name == "blackcube_nanobot_info":
            out = peer_json("GET", "/peer/v1/info", timeout=10)
        elif name == "blackcube_nanobot_prompt":
            out = peer_json(
                "POST",
                "/peer/v1/prompt",
                {"prompt": str(args.get("prompt") or "")},
                timeout=180,
            )
        elif name == "blackcube_nanobot_shell":
            out = peer_json(
                "POST",
                "/peer/v1/shell",
                {"command": str(args.get("command") or "")},
                timeout=120,
            )
        elif _BC is not None and name == "braincube_info":
            out = _BC.info()
        elif _BC is not None and name == "braincube_live":
            out = _BC.bc("live", 15)
        elif _BC is not None and name == "braincube_law":
            out = _BC.bc("law", 15)
        elif _BC is not None and name == "braincube_export":
            out = _BC.bc("export", 45)
        elif _BC is not None and name == "braincell_status":
            out = _BC.braincell_status()
        elif _BC is not None and name == "cubalc_run":
            out = _BC.cubalc_run(str(args.get("board") or ""), str(args.get("source") or ""))
        else:
            out = {"error": f"unknown tool {name}"}
        text = json.dumps(out, ensure_ascii=False, indent=2)[:50000]
        err = bool(out.get("error")) and not out.get("ok", True)
        return {
            "jsonrpc": "2.0",
            "id": mid,
            "result": {"content": [{"type": "text", "text": text}], "isError": err},
        }
    if method == "ping":
        return {"jsonrpc": "2.0", "id": mid, "result": {}}
    return {
        "jsonrpc": "2.0",
        "id": mid,
        "error": {"code": -32601, "message": f"Method not found: {method}"},
    }


class H(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):  # quieter
        pass

    def _read_json(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b"{}"
        try:
            return json.loads(raw.decode() or "{}")
        except Exception:
            return {}

    def _send(self, code: int, obj: dict):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "*")
        self.end_headers()

    def do_GET(self):
        if self.path in ("/", "/health", "/mcp"):
            info = peer_json("GET", "/peer/v1/info", timeout=5)
            self._send(200, {"ok": True, "service": "blackcube-nanobot-http-mcp", "port": PORT, "peer": PEER, "braincube": bool(_BC), "tools": [t["name"] for t in TOOLS], "info": info})
            return
        self._send(404, {"error": "not_found"})

    def do_POST(self):
        # Accept /mcp, /sse, / for Grok remote MCP
        if not (self.path.startswith("/mcp") or self.path in ("/", "/message")):
            self._send(404, {"error": "not_found"})
            return
        msg = self._read_json()
        if isinstance(msg, list):
            out = [handle_rpc(m) for m in msg]
            # single-response clients: return last with id
            self._send(200, out if len(out) != 1 else out[0])
            return
        self._send(200, handle_rpc(msg))


def main():
    srv = ThreadingHTTPServer(("0.0.0.0", PORT), H)
    print(f"blackcube-nanobot-http-mcp peer={PEER} port={PORT} braincube={bool(_BC)} tools={len(TOOLS)}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
