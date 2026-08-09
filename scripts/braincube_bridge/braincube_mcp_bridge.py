#!/usr/bin/env python3
"""BrainCube ecosystem MCP (stdio) u2014 mesh organs on BlackCube hub."""
from __future__ import annotations
import json, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import bc_api as api

TOOLS = [
    {"name": "braincube_info", "description": "Ecosystem BrainCube bridge: peer, LAW, organs (CubalC/Neural/Nexus/Atlas).", "inputSchema": {"type": "object", "properties": {}}},
    {"name": "braincube_live", "description": "Live lattice snapshot for Neural Cube / commanders.", "inputSchema": {"type": "object", "properties": {}}},
    {"name": "braincube_law", "description": "First Cube LAW scoreboard only.", "inputSchema": {"type": "object", "properties": {}}},
    {"name": "braincube_export", "description": "Export chain_state (heavier).", "inputSchema": {"type": "object", "properties": {}}},
    {"name": "braincell_status", "description": "Braincell mini-hive cells + law.", "inputSchema": {"type": "object", "properties": {}}},
    {"name": "cubalc_run", "description": "Run allowlisted CubalC hive board or short source.", "inputSchema": {"type": "object", "properties": {"board": {"type": "string"}, "source": {"type": "string"}}}},
]

def read_msg():
    line = sys.stdin.readline()
    if not line: return None
    line = line.strip()
    if not line: return read_msg()
    if line.startswith("{"):
        return json.loads(line)
    headers = {}
    if ":" in line:
        k, v = line.split(":", 1)
        headers[k.strip().lower()] = v.strip()
    while True:
        l = sys.stdin.readline()
        if not l: return None
        l = l.strip()
        if not l: break
        if ":" in l:
            k, v = l.split(":", 1)
            headers[k.strip().lower()] = v.strip()
    n = int(headers.get("content-length", "0"))
    body = sys.stdin.buffer.read(n) if n else b"{}"
    return json.loads(body.decode())

def write_msg(m):
    sys.stdout.write(json.dumps(m, ensure_ascii=False, separators=(",", ":")) + "\n")
    sys.stdout.flush()

def _missing(err: str, **extra) -> dict:
    """Dual-wire fail plate (align peer http_peer_err / HTTP MCP _missing)."""
    out = {
        "schema": "nanobot.peer_http.v1",
        "ok": False,
        "action": "error",
        "error": err,
        "product_wire": "smx2",
        "peer_http": "lab_ops_only",
        "peer_http_is_product_bus": False,
        "share": "state_matrix_only",
        "hold_flash": 1,
        "llm_is_commander": False,
        "python": 0,
    }
    out.update(extra)
    return out


def dispatch(name, args):
    if name == "braincube_info": return api.info()
    if name == "braincube_live": return api.bc("live", 15)
    if name == "braincube_law": return api.bc("law", 15)
    if name == "braincube_export": return api.bc("export", 45)
    if name == "braincell_status": return api.braincell_status()
    if name == "cubalc_run": return api.cubalc_run(str(args.get("board") or ""), str(args.get("source") or ""))
    # Residual: bare {"error":"unknown tool X"} lacked dual-wire + isError false.
    out = _missing("unknown_tool")
    if name:
        out["tool"] = str(name)[:128]
    return out

def main():
    while True:
        msg = read_msg()
        if msg is None: break
        mid, method, params = msg.get("id"), msg.get("method"), msg.get("params") or {}
        if method == "initialize":
            write_msg({"jsonrpc": "2.0", "id": mid, "result": {"protocolVersion": "2024-11-05", "capabilities": {"tools": {}}, "serverInfo": {"name": "braincube-ecosystem", "version": "0.1.0"}}})
        elif method in ("notifications/initialized", "initialized"):
            if mid is not None: write_msg({"jsonrpc": "2.0", "id": mid, "result": {}})
        elif method == "tools/list":
            write_msg({"jsonrpc": "2.0", "id": mid, "result": {"tools": TOOLS}})
        elif method == "tools/call":
            name = params.get("name") or ""
            args = params.get("arguments") or {}
            out = dispatch(name, args)
            text = json.dumps(out, ensure_ascii=False, indent=2)[:50000]
            err = bool(out.get("error")) and not out.get("ok", True)
            write_msg({"jsonrpc": "2.0", "id": mid, "result": {"content": [{"type": "text", "text": text}], "isError": err}})
        elif method == "ping":
            write_msg({"jsonrpc": "2.0", "id": mid, "result": {}})
        elif mid is not None:
            write_msg({"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": f"Method not found: {method}"}})

if __name__ == "__main__":
    main()
