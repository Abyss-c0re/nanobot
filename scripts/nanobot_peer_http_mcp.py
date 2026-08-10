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


def _nonempty(s: object) -> str | None:
    """Residual: empty/whitespace MCP args still POSTed; peer 400 after RTT."""
    t = str(s or "").strip()
    return t if t else None


def _missing(err: str) -> dict:
    """Dual-wire fail plate (align peer http_peer_err action=error)."""
    return {
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


def peer_json(method: str, path: str, payload: dict | None = None, timeout: float = 120) -> dict:
    data = None if payload is None else json.dumps(payload).encode()
    req = urllib.request.Request(f"{PEER}{path}", data=data, method=method)
    req.add_header("Content-Type", "application/json")
    tok = peer_token()
    if tok:
        req.add_header("X-Nanobot-Peer-Token", tok)
        req.add_header("Authorization", f"Bearer {tok}")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode() or "{}")
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", "replace")
        try:
            return json.loads(body)
        except Exception:
            # Residual: bare {"error": body, "http_status": N} lacked dual-wire;
            # tools/call isError stayed false (ok defaulted True).
            out = _missing("peer_http_error")
            out["http_status"] = e.code
            if body:
                out["detail"] = body[:512]
            return out
    except Exception as e:
        # Residual: connection/timeout bare {"error": str(e)} → isError false.
        out = _missing("peer_unreachable")
        out["detail"] = str(e)[:512]
        return out


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
            prompt = _nonempty(args.get("prompt"))
            if not prompt:
                out = _missing("missing_prompt")
            else:
                out = peer_json(
                    "POST",
                    "/peer/v1/prompt",
                    {"prompt": prompt},
                    timeout=180,
                )
        elif name == "blackcube_nanobot_shell":
            command = _nonempty(args.get("command"))
            if not command:
                out = _missing("missing_command")
            else:
                out = peer_json(
                    "POST",
                    "/peer/v1/shell",
                    {"command": command},
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
            # Residual: bare {"error":"unknown tool X"} lacked dual-wire + isError
            # stayed false (ok defaulted True). Align _missing action=error.
            out = _missing("unknown_tool")
            if name:
                out["tool"] = str(name)[:128]
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
        """Return (obj, err). Residual: bad JSON used to become {} → Method not found: None."""
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b"{}"
        try:
            return json.loads(raw.decode() or "{}"), None
        except Exception:
            return None, "parse_error"

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
        # Mesh probes often hit /peer/v1/* on the HTTP MCP port by mistake —
        # answer them instead of {"error":"not_found"} so focus loops stay quiet.
        # Residual: /ready|/peer/v1/ready|/api/ready|/api/health were 404 while
        # peer :18787 answers them (action health|ready).
        # Residual: trailing slash + /api/info + jobs index still 404 on :18790
        # after peer gained those aliases — mesh hit MCP port and saw not_found.
        path = self.path.split("?", 1)[0]
        if path != "/" and path.endswith("/"):
            path = path.rstrip("/") or "/"
        ready_paths = ("/ready", "/peer/v1/ready", "/api/ready")
        # Residual: k8s-style probes after peer gained livez/readyz/healthz/alive.
        readyz_paths = ("/readyz", "/api/readyz", "/peer/v1/readyz")
        livez_paths = (
            "/livez",
            "/api/livez",
            "/peer/v1/livez",
            "/alive",
            "/api/alive",
            "/peer/v1/alive",
        )
        healthz_paths = ("/healthz", "/api/healthz", "/peer/v1/healthz")
        # Residual: GET /ping dual-wire after peer gained ping plate.
        ping_paths = ("/ping", "/api/ping", "/peer/v1/ping")
        # Residual: GET /api|/api/v1|/peer/v1 index after peer namespace plates.
        index_paths = ("/api", "/api/v1", "/peer/v1")
        health_paths = (
            "/peer/v1/health",
            "/health",
            "/api/health",
            "/api/v1/health",
            "/.well-known/health",
            "/",
            "/mcp",
        )
        # Residual: bare /hello after peer gained bare hello → info alias.
        info_paths = (
            "/peer/v1/info",
            "/api/info",
            "/peer/v1/hello",
            "/api/hello",
            "/hello",
        )
        jobs_coll = (
            "/peer/v1/jobs",
            "/peer/v1/job",
            "/api/jobs",
            "/api/job",
        )
        # Residual: peer control/task/models already dual-wire; mesh probes on
        # :18790 still not_found after info/jobs aliases landed.
        # Residual: GET subagents list on peer/API but :18790 returned not_found.
        # Residual: GET braincube status/live on peer but :18790 not_found (FOCUS #2).
        # Residual: GET resources on peer (/peer/v1 + /api/v1) but :18790 not_found.
        # Residual: GET /api/auth|/api/status on peer but :18790 not_found (signed_in probes).
        # Residual: GET /activate on peer (302|login_not_ready) but :18790 not_found.
        # Residual: GET /api/settings after peer gained GET settings plate.
        # Residual: GET /version dual-wire after peer gained version plate.
        # Residual: GET /api|/peer/v1/mcp/servers after peer slash aliases.
        # Residual: GET /api/log dual-wire after peer slash + /peer/v1/log.
        control_paths = ("/peer/v1/control", "/api/control")
        task_paths = ("/peer/v1/task", "/api/task")
        models_paths = ("/peer/v1/models", "/api/models")
        subagents_paths = ("/peer/v1/subagents", "/api/subagents")
        braincube_paths = ("/peer/v1/braincube", "/api/braincube")
        braincube_live_paths = ("/peer/v1/braincube/live", "/api/braincube/live")
        resources_paths = (
            "/peer/v1/resources",
            "/api/v1/resources",
            "/api/resources",
        )
        # Residual: GET /metrics dual-wire after peer gained metrics plate.
        metrics_paths = ("/metrics", "/api/metrics", "/peer/v1/metrics")
        # Residual: GET /whoami dual-wire after peer gained whoami plate.
        # Residual: bare /status after peer gained bare status → auth plate.
        auth_paths = (
            "/api/auth",
            "/api/status",
            "/peer/v1/auth",
            "/peer/v1/status",
            "/status",
        )
        whoami_paths = ("/whoami", "/api/whoami", "/peer/v1/whoami")
        activate_paths = ("/activate",)
        # Residual: bare /settings after peer gained bare settings plate.
        settings_paths = ("/api/settings", "/peer/v1/settings", "/settings")
        version_paths = ("/version", "/api/version", "/peer/v1/version")
        # Residual: GET uptime dual-wire after peer gained uptime plate.
        uptime_paths = ("/uptime", "/api/uptime", "/peer/v1/uptime")
        mcp_servers_paths = ("/api/mcp/servers", "/peer/v1/mcp/servers")
        log_paths = ("/api/log", "/peer/v1/log")
        # Residual: GET /api|/peer/v1/backend after peer gained GET backend plate.
        backend_paths = ("/api/backend", "/peer/v1/backend")
        # Residual: GET openapi/favicon after peer gained those plates.
        # Residual: swagger/docs aliases after peer gained those openapi aliases.
        openapi_paths = (
            "/openapi.json",
            "/openapi",
            "/api/openapi.json",
            "/api/openapi",
            "/peer/v1/openapi.json",
            "/peer/v1/openapi",
            "/openapi.yaml",
            "/api/openapi.yaml",
            "/peer/v1/openapi.yaml",
            "/swagger.json",
            "/swagger",
            "/api/swagger",
            "/api/swagger.json",
            "/api/v1/swagger",
            "/peer/v1/swagger",
            "/peer/v1/swagger.json",
            "/docs",
            "/api/docs",
            "/peer/v1/docs",
        )
        favicon_paths = ("/favicon.ico", "/favicon")
        # Residual: GET robots.txt after peer gained robots plate.
        robots_paths = ("/robots.txt",)
        # Residual: GET security.txt after peer gained RFC 9116 plate.
        security_txt_paths = (
            "/security.txt",
            "/.well-known/security.txt",
            "/api/security.txt",
            "/peer/v1/security.txt",
        )
        # Residual: GET manifest after peer gained web app manifest plate.
        manifest_paths = (
            "/manifest.json",
            "/manifest.webmanifest",
            "/site.webmanifest",
            "/api/manifest.json",
            "/peer/v1/manifest.json",
        )
        # Residual: GET capabilities dual-wire after peer gained capabilities plate.
        capabilities_paths = (
            "/capabilities",
            "/api/capabilities",
            "/peer/v1/capabilities",
        )
        if path in info_paths:
            info = peer_json("GET", "/peer/v1/info", timeout=5)
            self._send(200, info if isinstance(info, dict) else {"ok": False, "info": info})
            return
        if path in jobs_coll:
            jobs = peer_json("GET", "/peer/v1/jobs", timeout=8)
            self._send(200, jobs if isinstance(jobs, dict) else {"ok": False, "jobs": jobs})
            return
        if path in control_paths:
            ctl = peer_json("GET", "/peer/v1/control", timeout=5)
            self._send(200, ctl if isinstance(ctl, dict) else {"ok": False, "control": ctl})
            return
        if path in task_paths:
            task = peer_json("GET", "/peer/v1/task", timeout=5)
            self._send(200, task if isinstance(task, dict) else {"ok": False, "task": task})
            return
        if path in models_paths:
            models = peer_json("GET", "/peer/v1/models", timeout=15)
            self._send(
                200, models if isinstance(models, dict) else {"ok": False, "models": models}
            )
            return
        if path in subagents_paths:
            sa = peer_json("GET", "/peer/v1/subagents", timeout=8)
            self._send(
                200, sa if isinstance(sa, dict) else {"ok": False, "subagents": sa}
            )
            return
        if path in braincube_live_paths:
            live = peer_json("GET", "/peer/v1/braincube/live", timeout=8)
            self._send(
                200, live if isinstance(live, dict) else {"ok": False, "braincube": live}
            )
            return
        if path in braincube_paths:
            bc = peer_json("GET", "/peer/v1/braincube", timeout=8)
            self._send(
                200, bc if isinstance(bc, dict) else {"ok": False, "braincube": bc}
            )
            return
        if path in resources_paths:
            res = peer_json("GET", "/peer/v1/resources", timeout=5)
            self._send(
                200, res if isinstance(res, dict) else {"ok": False, "resources": res}
            )
            return
        if path in metrics_paths:
            met = peer_json("GET", "/api/metrics", timeout=5)
            self._send(
                200, met if isinstance(met, dict) else {"ok": False, "metrics": met}
            )
            return
        if path in auth_paths:
            # Peer auth plate is /api/auth (not under /peer/v1 historically).
            auth = peer_json("GET", "/api/auth", timeout=5)
            self._send(
                200, auth if isinstance(auth, dict) else {"ok": False, "auth": auth}
            )
            return
        if path in whoami_paths:
            who = peer_json("GET", "/api/whoami", timeout=5)
            self._send(
                200, who if isinstance(who, dict) else {"ok": False, "whoami": who}
            )
            return
        if path in activate_paths:
            # Prefer dual-wire auth plate over raw 302 HTML follow.
            auth = peer_json("GET", "/api/auth", timeout=5)
            if not isinstance(auth, dict):
                self._send(503, _missing("login_not_ready"))
                return
            if auth.get("signed_in"):
                # Align peer when no device-login pending: 503 login_not_ready.
                self._send(503, _missing("login_not_ready"))
                return
            vuc = (auth.get("verification_uri_complete") or auth.get("verification_uri") or "").strip()
            if auth.get("login_pending") and vuc:
                out = dict(auth)
                out["action"] = "activate"
                out["redirect"] = vuc[:512]
                self._send(200, out)
                return
            self._send(503, _missing("login_not_ready"))
            return
        if path in settings_paths:
            stg = peer_json("GET", "/api/settings", timeout=5)
            self._send(
                200, stg if isinstance(stg, dict) else {"ok": False, "settings": stg}
            )
            return
        if path in version_paths:
            ver = peer_json("GET", "/api/version", timeout=5)
            self._send(
                200, ver if isinstance(ver, dict) else {"ok": False, "version": ver}
            )
            return
        if path in uptime_paths:
            up = peer_json("GET", "/api/uptime", timeout=5)
            self._send(
                200, up if isinstance(up, dict) else {"ok": False, "uptime": up}
            )
            return
        if path in mcp_servers_paths:
            srv = peer_json("GET", "/api/mcp/servers", timeout=5)
            self._send(
                200, srv if isinstance(srv, dict) else {"ok": False, "servers": srv}
            )
            return
        if path in log_paths:
            logb = peer_json("GET", "/api/log", timeout=5)
            self._send(
                200, logb if isinstance(logb, dict) else {"ok": False, "log": logb}
            )
            return
        if path in backend_paths:
            be = peer_json("GET", "/api/backend", timeout=5)
            self._send(
                200, be if isinstance(be, dict) else {"ok": False, "backend": be}
            )
            return
        if path in openapi_paths:
            # Peer YAML is not JSON; always proxy the JSON OpenAPI plate.
            doc = peer_json("GET", "/openapi.json", timeout=5)
            self._send(
                200, doc if isinstance(doc, dict) else {"ok": False, "openapi": doc}
            )
            return
        if path in capabilities_paths:
            cap = peer_json("GET", "/api/capabilities", timeout=5)
            self._send(
                200, cap if isinstance(cap, dict) else {"ok": False, "capabilities": cap}
            )
            return
        if path in favicon_paths:
            # Peer serves image/svg+xml; MCP mesh probes want dual-wire JSON.
            self._send(
                200,
                {
                    "schema": "nanobot.peer_http.v1",
                    "ok": True,
                    "action": "favicon",
                    "service": "blackcube-nanobot-http-mcp",
                    "content_type": "image/svg+xml",
                    "peer_path": "/favicon.ico",
                    "product_wire": "smx2",
                    "peer_http": "lab_ops_only",
                    "peer_http_is_product_bus": False,
                    "share": "state_matrix_only",
                    "hold_flash": 1,
                    "llm_is_commander": False,
                    "python": 0,
                },
            )
            return
        if path in robots_paths:
            # Peer serves text/plain; MCP mesh probes want dual-wire JSON.
            self._send(
                200,
                {
                    "schema": "nanobot.peer_http.v1",
                    "ok": True,
                    "action": "robots",
                    "service": "blackcube-nanobot-http-mcp",
                    "content_type": "text/plain",
                    "peer_path": "/robots.txt",
                    "disallow": "/",
                    "product_wire": "smx2",
                    "peer_http": "lab_ops_only",
                    "peer_http_is_product_bus": False,
                    "share": "state_matrix_only",
                    "hold_flash": 1,
                    "llm_is_commander": False,
                    "python": 0,
                },
            )
            return
        if path in security_txt_paths:
            self._send(
                200,
                {
                    "schema": "nanobot.peer_http.v1",
                    "ok": True,
                    "action": "security_txt",
                    "service": "blackcube-nanobot-http-mcp",
                    "content_type": "text/plain",
                    "peer_path": "/.well-known/security.txt",
                    "contact": "https://github.com/Abyss-c0re/nanobot/security/advisories/new",
                    "policy": "https://github.com/Abyss-c0re/nanobot/blob/main/SECURITY.md",
                    "product_wire": "smx2",
                    "peer_http": "lab_ops_only",
                    "peer_http_is_product_bus": False,
                    "share": "state_matrix_only",
                    "hold_flash": 1,
                    "llm_is_commander": False,
                    "python": 0,
                },
            )
            return
        if path in manifest_paths:
            man = peer_json("GET", "/manifest.json", timeout=5)
            self._send(
                200, man if isinstance(man, dict) else {"ok": False, "manifest": man}
            )
            return
        # Poll-by-id: /peer/v1/jobs/{id} or /api/jobs/{id} (+ optional trailing slash)
        for pref in ("/peer/v1/jobs/", "/peer/v1/job/", "/api/jobs/", "/api/job/"):
            if path.startswith(pref):
                jid = path[len(pref) :]
                if jid.endswith("/"):
                    jid = jid.rstrip("/")
                if jid and jid.isdigit():
                    st = peer_json("GET", f"/peer/v1/jobs/{jid}", timeout=8)
                    self._send(
                        200,
                        st if isinstance(st, dict) else {"ok": False, "job": st},
                    )
                    return
                # Residual: hand-rolled bad_id plate lacked action=error while
                # peer GET /jobs/{id} and _missing dual-wire both set it.
                self._send(400, _missing("bad_id"))
                return
        if path in index_paths:
            idx = peer_json("GET", path, timeout=5)
            self._send(
                200, idx if isinstance(idx, dict) else {"ok": False, "index": idx}
            )
            return
        if (
            path in ready_paths
            or path in readyz_paths
            or path in livez_paths
            or path in healthz_paths
            or path in health_paths
            or path in ping_paths
        ):
            info = peer_json("GET", "/peer/v1/info", timeout=5)
            if path in readyz_paths:
                act = "readyz"
            elif path in ready_paths:
                act = "ready"
            elif path in ping_paths:
                act = "ping"
            elif path in livez_paths:
                act = "livez"
            elif path in healthz_paths:
                act = "healthz"
            else:
                act = "health"
            body = {
                "schema": "nanobot.peer_http.v1",
                "ok": True,
                "action": act,
                "service": "blackcube-nanobot-http-mcp",
                "port": PORT,
                "peer": PEER,
                "braincube": bool(_BC),
                "info": info,
            }
            if path in ("/", "/mcp", "/health"):
                body["tools"] = [t["name"] for t in TOOLS]
            self._send(200, body)
            return
        # Residual: bare {"error":"not_found"} lacked dual-wire action/schema;
        # mesh could not classify MCP 404 like peer error plates.
        self._send(404, _missing("not_found"))

    def do_POST(self):
        # Accept /mcp, /sse, / for Grok remote MCP
        if not (self.path.startswith("/mcp") or self.path in ("/", "/message")):
            self._send(404, _missing("not_found"))
            return
        msg, perr = self._read_json()
        if perr is not None:
            # JSON-RPC 2.0 Parse error (HTTP 200 body is standard for MCP-over-HTTP).
            self._send(
                200,
                {
                    "jsonrpc": "2.0",
                    "id": None,
                    "error": {"code": -32700, "message": "Parse error"},
                },
            )
            return
        if isinstance(msg, list):
            out = [handle_rpc(m) if isinstance(m, dict) else {
                "jsonrpc": "2.0",
                "id": None,
                "error": {"code": -32600, "message": "Invalid Request"},
            } for m in msg]
            # single-response clients: return last with id
            self._send(200, out if len(out) != 1 else out[0])
            return
        if not isinstance(msg, dict):
            # Residual: JSON null/number/string fell into handle_rpc → Method not found.
            self._send(
                200,
                {
                    "jsonrpc": "2.0",
                    "id": None,
                    "error": {"code": -32600, "message": "Invalid Request"},
                },
            )
            return
        self._send(200, handle_rpc(msg))


def main():
    srv = ThreadingHTTPServer(("0.0.0.0", PORT), H)
    print(f"blackcube-nanobot-http-mcp peer={PEER} port={PORT} braincube={bool(_BC)} tools={len(TOOLS)}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
