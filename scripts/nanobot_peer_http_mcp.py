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
        # Residual: OPTIONS lacked Allow; post-only dual-wire paths should
        # advertise POST, OPTIONS only (match peer :18787).
        path = self.path.split("?", 1)[0]
        if path != "/" and path.endswith("/"):
            path = path.rstrip("/") or "/"
        post_only = {
            "/api/shell",
            "/peer/v1/shell",
            "/api/shell/gate",
            "/peer/v1/shell/gate",
            "/api/shell/approve",
            "/peer/v1/shell/approve",
            "/api/prompt",
            "/peer/v1/prompt",
            "/api/chat",
            "/peer/v1/chat",
            "/api/auth/start",
            "/peer/v1/auth/start",
            "/api/mcp/probe",
            "/peer/v1/mcp/probe",
        }
        allow = "POST, OPTIONS" if path in post_only else "GET, POST, OPTIONS"
        self.send_response(204)
        self.send_header("Allow", allow)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", allow)
        self.send_header("Access-Control-Allow-Headers", "*")
        self.send_header("Access-Control-Max-Age", "600")
        self.end_headers()

    def _post_only_paths(self) -> set[str]:
        return {
            "/api/shell",
            "/peer/v1/shell",
            "/api/shell/gate",
            "/peer/v1/shell/gate",
            "/api/shell/approve",
            "/peer/v1/shell/approve",
            "/api/prompt",
            "/peer/v1/prompt",
            "/api/chat",
            "/peer/v1/chat",
            "/api/auth/start",
            "/peer/v1/auth/start",
            "/api/mcp/probe",
            "/peer/v1/mcp/probe",
        }

    def _method_not_allowed(self):
        # Residual: PUT/DELETE/PATCH on post_only fell to BaseHTTP 501; dual-wire 405.
        path = self.path.split("?", 1)[0]
        if path != "/" and path.endswith("/"):
            path = path.rstrip("/") or "/"
        allow = (
            "GET, POST, OPTIONS"
            if path in self._post_only_paths()
            else "GET, POST, OPTIONS"
        )
        body = {
            "schema": "nanobot.peer_http.v1",
            "ok": False,
            "action": "error",
            "error": "method_not_allowed",
            "allow": allow,
            "product_wire": "smx2",
            "peer_http": "lab_ops_only",
            "peer_http_is_product_bus": False,
            "share": "state_matrix_only",
            "hold_flash": 1,
            "llm_is_commander": False,
            "python": 0,
        }
        raw = json.dumps(body).encode()
        self.send_response(405)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Allow", allow)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", allow)
        self.send_header("Access-Control-Allow-Headers", "*")
        self.end_headers()
        self.wfile.write(raw)

    def do_PUT(self):
        self._method_not_allowed()

    def do_DELETE(self):
        self._method_not_allowed()

    def do_PATCH(self):
        self._method_not_allowed()

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
        # Residual: nested /control/shell|watcher|ui + /watcher on peer after
        # control plate; mesh on :18790 still not_found.
        control_paths = (
            "/peer/v1/control",
            "/api/control",
            "/peer/v1/control/shell",
            "/api/control/shell",
            "/peer/v1/control/watcher",
            "/api/control/watcher",
            "/peer/v1/control/ui",
            "/api/control/ui",
            "/peer/v1/watcher",
            "/api/watcher",
        )
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
        # Residual: /me|/session|/auth/status after peer identity aliases.
        auth_paths = (
            "/api/auth",
            "/api/auth/status",
            "/peer/v1/auth/status",
            "/api/status",
            "/peer/v1/auth",
            "/peer/v1/status",
            "/status",
        )
        whoami_paths = (
            "/whoami",
            "/api/whoami",
            "/peer/v1/whoami",
            "/me",
            "/api/me",
            "/peer/v1/me",
        )
        session_paths = (
            "/session",
            "/api/session",
            "/peer/v1/session",
        )
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
        # Residual: /favicon.svg dual-wire after peer gained SVG alias plate.
        # Residual: apple-touch-icon after peer gained Safari touch-icon plate.
        favicon_paths = (
            "/favicon.ico",
            "/favicon",
            "/favicon.svg",
            "/api/favicon.svg",
            "/peer/v1/favicon.svg",
            "/api/favicon.ico",
            "/peer/v1/favicon.ico",
            "/apple-touch-icon.png",
            "/apple-touch-icon-precomposed.png",
            "/apple-touch-icon",
            "/api/apple-touch-icon.png",
            "/peer/v1/apple-touch-icon.png",
        )
        # Residual: GET service-worker.js after peer gained no-PWA plate.
        service_worker_paths = (
            "/service-worker.js",
            "/sw.js",
            "/api/service-worker.js",
            "/peer/v1/service-worker.js",
            "/api/sw.js",
            "/peer/v1/sw.js",
        )
        # Residual: GET ads.txt after peer gained no-sellers plate.
        ads_paths = (
            "/ads.txt",
            "/app-ads.txt",
            "/api/ads.txt",
            "/peer/v1/ads.txt",
            "/api/app-ads.txt",
            "/peer/v1/app-ads.txt",
        )
        # Residual: GET sellers.json after peer gained empty IAB sellers plate.
        sellers_paths = (
            "/sellers.json",
            "/api/sellers.json",
            "/peer/v1/sellers.json",
        )
        # Residual: GET ai-plugin.json after peer gained OpenAI plugin plate.
        ai_plugin_paths = (
            "/.well-known/ai-plugin.json",
            "/.well-known/ai-plugin",
            "/ai-plugin.json",
            "/ai-plugin",
            "/api/ai-plugin.json",
            "/peer/v1/ai-plugin.json",
        )
        # Residual: GET agent-card after peer gained private A2A card plate.
        agent_card_paths = (
            "/.well-known/agent-card.json",
            "/.well-known/agent.json",
            "/agent-card.json",
            "/agent.json",
            "/api/agent-card.json",
            "/peer/v1/agent-card.json",
        )
        # Residual: GET assetlinks.json after peer gained empty DAL plate.
        assetlinks_paths = (
            "/.well-known/assetlinks.json",
            "/.well-known/assetlinks",
            "/assetlinks.json",
            "/assetlinks",
            "/api/assetlinks.json",
            "/peer/v1/assetlinks.json",
        )
        # Residual: GET AASA after peer gained empty Universal Links plate.
        aasa_paths = (
            "/.well-known/apple-app-site-association",
            "/.well-known/apple-app-site-association.json",
            "/apple-app-site-association",
            "/apple-app-site-association.json",
            "/api/apple-app-site-association",
            "/peer/v1/apple-app-site-association",
        )
        # Residual: GET gpc.json after peer gained GPC well-known plate.
        gpc_paths = (
            "/.well-known/gpc.json",
            "/.well-known/gpc",
            "/gpc.json",
            "/gpc",
            "/api/gpc.json",
            "/peer/v1/gpc.json",
        )
        # Residual: GET tdmrep.json after peer gained TDM reservation plate.
        tdmrep_paths = (
            "/.well-known/tdmrep.json",
            "/.well-known/tdmrep",
            "/tdmrep.json",
            "/tdmrep",
            "/api/tdmrep.json",
            "/peer/v1/tdmrep.json",
        )
        # Residual: GET mta-sts.txt after peer gained RFC 8461 mode=none plate.
        mta_sts_paths = (
            "/.well-known/mta-sts.txt",
            "/.well-known/mta-sts",
            "/mta-sts.txt",
            "/mta-sts",
            "/api/mta-sts.txt",
            "/api/mta-sts",
            "/peer/v1/mta-sts.txt",
            "/peer/v1/mta-sts",
            "/.well-known/mta-sts.json",
            "/.well-known/enterprise-transport-security",
        )
        # Residual: GET caldav after peer gained empty CalDAV discovery plate.
        caldav_paths = (
            "/.well-known/caldav",
            "/.well-known/caldav.json",
            "/caldav",
            "/caldav.json",
            "/api/caldav",
            "/peer/v1/caldav",
        )
        # Residual: GET carddav after peer gained empty CardDAV discovery plate.
        carddav_paths = (
            "/.well-known/carddav",
            "/.well-known/carddav.json",
            "/carddav",
            "/carddav.json",
            "/api/carddav",
            "/peer/v1/carddav",
        )
        # Residual: GET api-catalog after peer gained RFC 9727 linkset plate.
        api_catalog_paths = (
            "/.well-known/api-catalog",
            "/.well-known/api-catalog.json",
            "/api-catalog",
            "/api-catalog.json",
            "/api/api-catalog",
            "/peer/v1/api-catalog",
        )
        # Residual: GET dnt-policy.txt after peer gained DNT honor plate.
        dnt_policy_paths = (
            "/.well-known/dnt-policy.txt",
            "/dnt-policy.txt",
            "/api/dnt-policy.txt",
            "/peer/v1/dnt-policy.txt",
        )
        # Residual: GET passkey-endpoints after peer gained empty passkey plate.
        passkey_paths = (
            "/.well-known/passkey-endpoints",
            "/.well-known/passkey-endpoints.json",
            "/.well-known/passkey-endpoints/enroll",
            "/.well-known/passkey-endpoints/enroll.json",
            "/.well-known/passkey-endpoints/manage",
            "/.well-known/passkey-endpoints/manage.json",
            "/passkey-endpoints",
            "/passkey-endpoints.json",
            "/api/passkey-endpoints",
            "/peer/v1/passkey-endpoints",
        )
        # Residual: GET webfinger after peer gained empty JRD plate.
        webfinger_paths = (
            "/.well-known/webfinger",
            "/.well-known/webfinger.json",
            "/.well-known/webfinger.xml",
            "/.well-known/webfinger.txt",
            "/webfinger",
            "/webfinger.json",
            "/webfinger.xml",
            "/webfinger.txt",
            "/api/webfinger",
            "/peer/v1/webfinger",
        )
        # Residual: GET nodeinfo after peer gained empty NodeInfo plate.
        nodeinfo_paths = (
            "/.well-known/nodeinfo",
            "/.well-known/nodeinfo.json",
            "/.well-known/nodeinfo/2.0",
            "/.well-known/nodeinfo/2.0.json",
            "/.well-known/nodeinfo/2.1",
            "/.well-known/nodeinfo/2.1.json",
            "/.well-known/nodeinfo/2.2",
            "/.well-known/nodeinfo/2.2.json",
            "/.well-known/nodeinfo/2.3",
            "/.well-known/nodeinfo/2.3.json",
            "/nodeinfo",
            "/nodeinfo.json",
            "/nodeinfo/2.0",
            "/nodeinfo/2.0.json",
            "/nodeinfo/2.1",
            "/nodeinfo/2.1.json",
            "/nodeinfo/2.2",
            "/nodeinfo/2.2.json",
            "/nodeinfo/2.3",
            "/nodeinfo/2.3.json",
            "/api/nodeinfo",
            "/peer/v1/nodeinfo",
        )
        # Residual: GET host-meta after peer gained empty RFC 6415 JRD plate.
        host_meta_paths = (
            "/.well-known/host-meta",
            "/.well-known/host-meta.json",
            "/.well-known/host-meta.xml",
            "/.well-known/host-meta.xrd",
            "/host-meta",
            "/host-meta.json",
            "/host-meta.xml",
            "/host-meta.xrd",
            "/api/host-meta",
            "/peer/v1/host-meta",
        )
        # Residual: GET matrix client discovery after peer gained empty plate.
        matrix_client_paths = (
            "/.well-known/matrix",
            "/.well-known/matrix.json",
            "/.well-known/matrix/client",
            "/.well-known/matrix/client.json",
            "/.well-known/matrix/support",
            "/.well-known/matrix/support.json",
            "/matrix",
            "/matrix.json",
            "/matrix/client",
            "/matrix/client.json",
            "/matrix/support",
            "/matrix/support.json",
            "/api/matrix/client",
            "/peer/v1/matrix/client",
        )
        # Residual: GET matrix server discovery after peer gained empty plate.
        matrix_server_paths = (
            "/.well-known/matrix/server",
            "/.well-known/matrix/server.json",
            "/matrix/server",
            "/matrix/server.json",
            "/api/matrix/server",
            "/peer/v1/matrix/server",
        )
        # Residual: GET openid-configuration after peer gained non-OP plate.
        openid_paths = (
            "/.well-known/openid-configuration",
            "/.well-known/openid-configuration.json",
            "/.well-known/oauth-authorization-server/openid-configuration",
            "/.well-known/oauth-authorization-server/.well-known/openid-configuration",
            "/openid-configuration",
            "/openid-configuration.json",
            "/api/openid-configuration",
            "/peer/v1/openid-configuration",
        )
        # Residual: GET openid-federation after peer gained non-entity plate.
        openid_fed_paths = (
            "/.well-known/openid-federation",
            "/.well-known/openid-federation.json",
            "/.well-known/openid-federation/entity-statement",
            "/.well-known/openid-federation/list",
            "/.well-known/openid-federation/resolve",
            "/.well-known/openid-federation/fetch",
            "/.well-known/openid-federation/trust_mark_status",
            "/.well-known/openid-federation/trust_marked_entities",
            "/.well-known/openid-federation/historical_keys",
            "/.well-known/entity-statement",
            "/.well-known/trust-chain",
            "/openid-federation",
            "/openid-federation.json",
            "/api/openid-federation",
            "/peer/v1/openid-federation",
        )
        # Residual: GET uma2-configuration after peer gained non-UMA plate.
        uma2_paths = (
            "/.well-known/uma2-configuration",
            "/.well-known/uma2-configuration.json",
            "/.well-known/uma2-configuration/resource_set",
            "/.well-known/uma2-configuration/permission",
            "/.well-known/uma2-configuration/resource_registration",
            "/.well-known/uma2-configuration/permission_endpoint",
            "/.well-known/uma2-configuration/introspection",
            "/.well-known/uma2-configuration/claims_interaction",
            "/.well-known/uma2-configuration/claims_interaction_endpoint",
            "/uma2-configuration",
            "/uma2-configuration.json",
            "/api/uma2-configuration",
            "/peer/v1/uma2-configuration",
        )
        # Residual: GET openid-credential-issuer after peer gained non-OID4VCI plate.
        oid4vci_paths = (
            "/.well-known/openid-credential-issuer",
            "/.well-known/openid-credential-issuer.json",
            "/.well-known/openid-credential-issuer/credential_endpoint",
            "/.well-known/openid-credential-issuer/batch_credential",
            "/.well-known/openid-credential-issuer/deferred_credential",
            "/.well-known/credential-issuer",
            "/.well-known/vc-configuration",
            "/openid-credential-issuer",
            "/openid-credential-issuer.json",
            "/api/openid-credential-issuer",
            "/peer/v1/openid-credential-issuer",
        )
        # Residual: GET fido2-configuration after peer gained non-FIDO2 plate.
        fido2_paths = (
            "/.well-known/fido2-configuration",
            "/.well-known/fido2-configuration.json",
            "/.well-known/fido2-configuration/origins",
            "/.well-known/fido2-configuration/trusted-facets",
            "/.well-known/fido2-configuration/trustedFacets",
            "/.well-known/fido-u2f-facets",
            "/.well-known/u2f",
            "/fido2-configuration",
            "/fido2-configuration.json",
            "/api/fido2-configuration",
            "/peer/v1/fido2-configuration",
        )
        # Residual: GET webauthn after peer gained empty related-origins plate.
        webauthn_paths = (
            "/.well-known/webauthn",
            "/.well-known/webauthn.json",
            "/.well-known/webauthn/origins",
            "/.well-known/webauthn/origins.json",
            "/.well-known/webauthn/related-origins",
            "/.well-known/webauthn/configuration",
            "/.well-known/webauthn/attestation",
            "/.well-known/passkey-endpoints/related-origins",
            "/.well-known/passkey-endpoints/origins",
            "/webauthn",
            "/webauthn.json",
            "/api/webauthn",
            "/peer/v1/webauthn",
        )
        # Residual: GET did.json after peer gained empty did:web plate.
        did_json_paths = (
            "/.well-known/did.json",
            "/.well-known/did",
            "/.well-known/did-web",
            "/.well-known/web-did",
            "/.well-known/identity",
            "/did.json",
            "/did",
            "/api/did.json",
            "/peer/v1/did.json",
        )
        # Residual: GET did-configuration after peer gained empty linkage plate.
        did_cfg_paths = (
            "/.well-known/did-configuration",
            "/.well-known/did-configuration.json",
            "/.well-known/did-configuration/resources",
            "/.well-known/did-configuration/resources.json",
            "/did-configuration",
            "/did-configuration.json",
            "/api/did-configuration",
            "/peer/v1/did-configuration",
        )
        # Residual: GET oauth-authorization-server after peer gained non-AS plate.
        oauth_as_paths = (
            "/.well-known/oauth-authorization-server",
            "/.well-known/oauth-authorization-server.json",
            "/oauth-authorization-server",
            "/oauth-authorization-server.json",
            "/api/oauth-authorization-server",
            "/peer/v1/oauth-authorization-server",
        )
        # Residual: GET oauth-client-registration after peer gained non-DCR plate.
        oauth_reg_paths = (
            "/.well-known/oauth-client-registration",
            "/.well-known/oauth-client-registration.json",
            "/.well-known/oauth-client-registration/register",
            "/.well-known/oauth-authorization-server/register",
            "/.well-known/oauth-authorization-server/registration",
            "/.well-known/openid-configuration/registration",
            "/.well-known/openid-configuration/register",
            "/.well-known/oauth/register",
            "/.well-known/oauth/register.json",
            "/.well-known/oauth/registration",
            "/.well-known/oauth/registration.json",
            "/.well-known/openid/register",
            "/.well-known/openid/registration",
            "/.well-known/openid-configuration/registration.json",
            "/.well-known/oauth-authorization-server/registration_endpoint",
            "/.well-known/openid-configuration/registration_endpoint",
            "/.well-known/oauth-client-metadata",
            "/.well-known/oauth-client",
            "/.well-known/oauth-client.json",
            "/.well-known/client-metadata",
            "/.well-known/client-metadata.json",
            "/.well-known/oauth-client-registration/metadata",
            "/.well-known/openid-configuration/client_registration",
            "/.well-known/oauth-client-id-metadata-document",
            "/.well-known/oauth-client-id",
            "/.well-known/client-id-metadata",
            "/oauth-client-registration",
            "/oauth-client-registration.json",
            "/api/oauth-client-registration",
            "/api/oauth-client-registration.json",
            "/peer/v1/oauth-client-registration",
        )
        # Residual: GET oauth-protected-resource after peer gained RFC 9728 plate.
        oauth_pr_paths = (
            "/.well-known/oauth-protected-resource",
            "/.well-known/oauth-protected-resource.json",
            "/.well-known/oauth-protected-resource/metadata",
            "/oauth-protected-resource",
            "/oauth-protected-resource.json",
            "/api/oauth-protected-resource",
            "/peer/v1/oauth-protected-resource",
        )
        # Residual: GET crossdomain.xml after peer gained deny-all plate.
        crossdomain_paths = (
            "/crossdomain.xml",
            "/api/crossdomain.xml",
            "/peer/v1/crossdomain.xml",
            "/clientaccesspolicy.xml",
        )
        # Residual: GET browserconfig.xml after peer gained MS tile plate.
        browserconfig_paths = (
            "/browserconfig.xml",
            "/.well-known/browserconfig.xml",
            "/api/browserconfig.xml",
            "/peer/v1/browserconfig.xml",
        )
        # Residual: GET change-password after peer gained W3C well-known plate.
        change_password_paths = (
            "/.well-known/change-password",
            "/.well-known/change-password.json",
            "/.well-known/change-password.txt",
            "/api/change-password",
            "/peer/v1/change-password",
            "/change-password",
        )
        # Residual: GET robots.txt after peer gained robots plate.

        # Residual: GET shell/prompt dual-wire method plate after peer POST-only.
        shell_meta_paths = (
            "/peer/v1/shell",
            "/peer/v1/shell/",
            "/api/shell",
            "/api/shell/",
        )
        prompt_meta_paths = (
            "/peer/v1/prompt",
            "/peer/v1/prompt/",
            "/api/prompt",
            "/api/prompt/",
        )
        chat_meta_paths = (
            "/api/chat",
            "/api/chat/",
            "/peer/v1/chat",
            "/peer/v1/chat/",
        )
        auth_start_meta_paths = (
            "/api/auth/start",
            "/api/auth/start/",
            "/peer/v1/auth/start",
            "/peer/v1/auth/start/",
        )
        mcp_probe_meta_paths = (
            "/api/mcp/probe",
            "/api/mcp/probe/",
            "/peer/v1/mcp/probe",
            "/peer/v1/mcp/probe/",
        )
        shell_gate_meta_paths = (
            "/api/shell/gate",
            "/api/shell/gate/",
            "/peer/v1/shell/gate",
            "/peer/v1/shell/gate/",
        )
        shell_approve_meta_paths = (
            "/api/shell/approve",
            "/api/shell/approve/",
            "/peer/v1/shell/approve",
            "/peer/v1/shell/approve/",
        )
        # Residual: GET shell/approvals 404 on MCP while peer lists pending gate.
        shell_approvals_paths = (
            "/api/shell/approvals",
            "/api/shell/approvals/",
            "/peer/v1/shell/approvals",
            "/peer/v1/shell/approvals/",
        )

        robots_paths = (
            "/robots.txt",
            "/.well-known/robots.txt",
            "/api/robots.txt",
            "/peer/v1/robots.txt",
        )
        # Residual: GET security.txt after peer gained RFC 9116 plate.
        security_txt_paths = (
            "/security.txt",
            "/.well-known/security.txt",
            "/.well-known/security.json",
            "/.well-known/security.txt.json",
            "/api/security.txt",
            "/peer/v1/security.txt",
        )
        # Residual: GET trust.txt after peer gained empty trust plate.
        trust_txt_paths = (
            "/trust.txt",
            "/.well-known/trust.txt",
            "/.well-known/trust.json",
            "/api/trust.txt",
            "/peer/v1/trust.txt",
        )
        # Residual: GET keybase.txt after peer gained empty proofs plate.
        keybase_txt_paths = (
            "/keybase.txt",
            "/.well-known/keybase.txt",
            "/.well-known/keybase.json",
            "/api/keybase.txt",
            "/peer/v1/keybase.txt",
        )
        # Residual: GET pgp-key.txt after peer gained empty OpenPGP plate.
        pgp_key_txt_paths = (
            "/pgp-key.txt",
            "/.well-known/pgp-key.txt",
            "/.well-known/pgp-key",
            "/.well-known/pgp-key.json",
            "/pgp-key",
            "/pgp-key.json",
            "/api/pgp-key.txt",
            "/peer/v1/pgp-key.txt",
        )
        # Residual: GET openpgpkey after peer gained non-WKD plate.
        openpgpkey_paths = (
            "/.well-known/openpgpkey",
            "/.well-known/openpgpkey.json",
            "/openpgpkey",
            "/openpgpkey.json",
            "/api/openpgpkey",
            "/peer/v1/openpgpkey",
            "/.well-known/openpgpkey/policy",
            "/openpgpkey/policy",
        )
        # Residual: GET sshfp after peer gained empty SSHFP plate.
        sshfp_paths = (
            "/.well-known/sshfp",
            "/.well-known/sshfp.json",
            "/.well-known/sshfp.txt",
            "/sshfp",
            "/sshfp.json",
            "/sshfp.txt",
            "/api/sshfp",
            "/api/sshfp.json",
            "/peer/v1/sshfp",
            "/peer/v1/sshfp.json",
        )
        # Residual: GET jwks after peer gained empty JWKS plate.
        jwks_paths = (
            "/.well-known/jwks.json",
            "/.well-known/jwks",
            "/jwks.json",
            "/jwks",
            "/api/jwks.json",
            "/api/jwks",
            "/peer/v1/jwks.json",
            "/peer/v1/jwks",
            "/.well-known/oauth-authorization-server/jwks",
            "/.well-known/oauth-authorization-server/jwks.json",
            "/.well-known/openid-configuration/jwks",
            "/.well-known/openid-configuration/jwks.json",
            "/.well-known/openid-credential-issuer/jwks",
            "/.well-known/openid-credential-issuer/jwks.json",
            "/.well-known/oauth-protected-resource/jwks",
            "/.well-known/oauth-protected-resource/jwks.json",
            "/.well-known/uma2-configuration/jwks",
            "/.well-known/uma2-configuration/jwks.json",
            "/.well-known/fido2-configuration/jwks",
            "/.well-known/fido2-configuration/jwks.json",
            "/.well-known/webauthn/jwks",
            "/.well-known/webauthn/jwks.json",
            "/.well-known/openid-federation/jwks",
            "/.well-known/openid-federation/jwks.json",
            "/.well-known/passkey-endpoints/jwks",
            "/.well-known/passkey-endpoints/jwks.json",
            "/.well-known/did-configuration/jwks",
            "/.well-known/did-configuration/jwks.json",
            "/.well-known/oauth/jwks",
            "/.well-known/oauth/jwks.json",
            "/.well-known/openid/jwks",
            "/.well-known/openid/jwks.json",
        )
        # Residual: GET nested oauth/openid endpoint empties after peer plate.
        oauth_endpoint_paths = (
            "/.well-known/oauth/token",
            "/.well-known/oauth/authorize",
            "/.well-known/oauth/userinfo",
            "/.well-known/oauth/revoke",
            "/.well-known/oauth/introspect",
            "/.well-known/oauth/device",
            "/.well-known/oauth/device_authorization",
            "/.well-known/openid/token",
            "/.well-known/openid/authorize",
            "/.well-known/openid/userinfo",
            "/.well-known/openid/revoke",
            "/.well-known/openid/introspect",
            "/.well-known/openid-configuration/token",
            "/.well-known/openid-configuration/userinfo",
            "/.well-known/openid-configuration/revoke",
            "/.well-known/openid-configuration/introspect",
            "/.well-known/openid-configuration/metadata",
            "/.well-known/oauth-authorization-server/token",
            "/.well-known/oauth-authorization-server/userinfo",
            "/.well-known/oauth-authorization-server/revoke",
            "/.well-known/oauth-authorization-server/introspect",
            "/.well-known/oauth-authorization-server/device",
            "/.well-known/oauth-authorization-server/device_authorization",
            "/.well-known/oauth-authorization-server/metadata",
            "/.well-known/openid-configuration/authorize",
            "/.well-known/openid-configuration/device",
            "/.well-known/openid-configuration/device_authorization",
            "/.well-known/oauth-authorization-server/authorize",
            "/.well-known/oauth/par",
            "/.well-known/oauth/pushed_authorization_request",
            "/.well-known/openid-configuration/par",
            "/.well-known/oauth-authorization-server/par",
            "/.well-known/openid-configuration/pushed_authorization_request_endpoint",
            "/.well-known/oauth-authorization-server/pushed_authorization_request_endpoint",
            "/.well-known/openid-credential-issuer/credential",
            "/.well-known/openid-credential-issuer/token",
            "/.well-known/openid-credential-issuer/nonce",
            "/.well-known/oauth-protected-resource/resource",
            "/.well-known/openid-configuration/end_session",
            "/.well-known/openid-configuration/logout",
            "/.well-known/openid-configuration/check_session",
            "/.well-known/openid-configuration/rp_logout",
            "/.well-known/openid-configuration/device_authorization_endpoint",
            "/.well-known/openid-configuration/bc_authorize",
            "/.well-known/openid-configuration/backchannel_authentication",
            "/.well-known/openid-configuration/mtls_endpoint_aliases",
            "/.well-known/oauth-authorization-server/end_session",
            "/.well-known/oauth-authorization-server/device_authorization_endpoint",
            "/.well-known/oauth-authorization-server/bc_authorize",
            "/.well-known/oauth-authorization-server/mtls_endpoint_aliases",
            "/.well-known/oauth/logout",
            "/.well-known/oauth/end_session",
            "/.well-known/oauth/device_code",
            "/.well-known/oauth/bc_authorize",
            "/.well-known/oauth/mtls",
            "/.well-known/ciba",
            "/.well-known/openid-configuration/frontchannel_logout",
            "/.well-known/openid-configuration/frontchannel_logout_uri",
            "/.well-known/openid-configuration/backchannel_logout",
            "/.well-known/openid-configuration/revocation_endpoint",
            "/.well-known/openid-configuration/introspection_endpoint",
            "/.well-known/openid-configuration/jwks_uri",
            "/.well-known/openid-configuration/issuer",
            "/.well-known/oauth-authorization-server/revocation_endpoint",
            "/.well-known/oauth-authorization-server/introspection_endpoint",
            "/.well-known/oauth-authorization-server/jwks_uri",
            "/.well-known/oauth-authorization-server/issuer",
            "/.well-known/gnap-as-rs/access_token",
            "/.well-known/oauth-protected-resource/resource_registration",
            "/.well-known/openid-credential-issuer/credential_configurations_supported",
            "/.well-known/openid-credential-issuer/authorization_servers",
        )
        # Residual: GET mail/identity/org discovery empties after peer plate.
        discovery_paths = (
            "/.well-known/autoconfig",
            "/.well-known/autoconfig/mail",
            "/.well-known/auto-config",
            "/.well-known/mail-v1.xml",
            "/.well-known/thunderbird/autoconfig",
            "/mail/config-v1.1.xml",
            "/.well-known/simple-web-discovery",
            "/.well-known/identity-hub",
            "/.well-known/dwn",
            "/.well-known/dwn-server",
            "/.well-known/decentralized-web-node",
            "/.well-known/ion",
            "/.well-known/openorg",
            "/.well-known/organization",
            "/.well-known/organization.json",
            "/.well-known/org.json",
            "/.well-known/swid",
        )
        # Residual: GET related-website-set after empty RWS plate.
        related_website_set_paths = (
            "/.well-known/related-website-set",
            "/.well-known/related-website-set.json",
            "/related-website-set",
            "/related-website-set.json",
            "/api/related-website-set",
            "/api/related-website-set.json",
            "/peer/v1/related-website-set",
            "/peer/v1/related-website-set.json",
        )
        # Residual: GET microsoft-identity-association after empty plate.
        microsoft_identity_association_paths = (
            "/.well-known/microsoft-identity-association",
            "/.well-known/microsoft-identity-association.json",
            "/.well-known/ms-identity-association",
            "/.well-known/ms-identity-association.json",
            "/microsoft-identity-association",
            "/microsoft-identity-association.json",
            "/api/microsoft-identity-association",
            "/api/microsoft-identity-association.json",
            "/peer/v1/microsoft-identity-association",
            "/peer/v1/microsoft-identity-association.json",
        )
        # Residual: GET apple merchantid domain association after empty plate.
        apple_merchantid_domain_association_paths = (
            "/.well-known/apple-developer-merchantid-domain-association",
            "/.well-known/apple-developer-merchantid-domain-association.json",
            "/.well-known/apple-merchantid-domain-association",
            "/.well-known/apple-merchantid-domain-association.json",
            "/apple-developer-merchantid-domain-association",
            "/apple-developer-merchantid-domain-association.json",
            "/apple-merchantid-domain-association",
            "/apple-merchantid-domain-association.json",
            "/api/apple-developer-merchantid-domain-association",
            "/api/apple-merchantid-domain-association",
            "/peer/v1/apple-developer-merchantid-domain-association",
            "/peer/v1/apple-merchantid-domain-association",
        )
        # Residual: GET nostr.json after empty NIP-05 plate.
        nostr_paths = (
            "/.well-known/nostr.json",
            "/.well-known/nostr",
            "/.well-known/nostr/nip05",
            "/nostr.json",
            "/nostr",
            "/api/nostr.json",
            "/api/nostr",
            "/peer/v1/nostr.json",
            "/peer/v1/nostr",
        )
        # Residual: GET atproto-did after empty ATProto plate.
        atproto_did_paths = (
            "/.well-known/atproto-did",
            "/.well-known/atproto-did.json",
            "/atproto-did",
            "/atproto-did.json",
            "/api/atproto-did",
            "/peer/v1/atproto-did",
        )
        # Residual: GET stellar.toml after empty SEP-0001 plate.
        stellar_toml_paths = (
            "/.well-known/stellar.toml",
            "/.well-known/stellar.toml.json",
            "/stellar.toml",
            "/stellar.toml.json",
            "/api/stellar.toml",
            "/peer/v1/stellar.toml",
        )
        # Residual: GET web-identity after empty FedCM plate.
        web_identity_paths = (
            "/.well-known/web-identity",
            "/.well-known/web-identity.json",
            "/web-identity",
            "/web-identity.json",
            "/api/web-identity",
            "/peer/v1/web-identity",
        )
        # Residual: GET posh after empty Microsoft POSH plate.
        posh_paths = (
            "/.well-known/posh",
            "/.well-known/posh/v1",
            "/.well-known/posh.json",
            "/posh",
            "/posh/v1",
            "/posh.json",
            "/api/posh",
            "/api/posh/v1",
            "/peer/v1/posh",
            "/peer/v1/posh/v1",
        )
        # Residual: GET traffic-advice after empty Chrome prefetch plate.
        traffic_advice_paths = (
            "/.well-known/traffic-advice",
            "/.well-known/traffic-advice.json",
            "/traffic-advice",
            "/traffic-advice.json",
            "/api/traffic-advice",
            "/peer/v1/traffic-advice",
        )
        # Residual: GET privacy-sandbox-attestations after empty Chrome plate.
        privacy_sandbox_paths = (
            "/.well-known/privacy-sandbox-attestations.json",
            "/.well-known/privacy-sandbox-attestations",
            "/privacy-sandbox-attestations.json",
            "/privacy-sandbox-attestations",
            "/api/privacy-sandbox-attestations.json",
            "/api/privacy-sandbox-attestations",
            "/peer/v1/privacy-sandbox-attestations.json",
            "/peer/v1/privacy-sandbox-attestations",
        )
        # Residual: GET no-federation resource after empty ActivityPub plate.
        no_federation_paths = (
            "/.well-known/resource-that-should-not-be-used-for-federation",
            "/.well-known/resource-that-should-not-exist-for-your-servers",
            "/resource-that-should-not-be-used-for-federation",
            "/resource-that-should-not-exist-for-your-servers",
            "/api/resource-that-should-not-be-used-for-federation",
            "/peer/v1/resource-that-should-not-be-used-for-federation",
        )
        # Residual: GET Chrome DevTools appspecific after empty workspace plate.
        chrome_devtools_paths = (
            "/.well-known/appspecific/com.chrome.devtools.json",
            "/.well-known/appspecific/com.chrome.devtools",
            "/com.chrome.devtools.json",
            "/api/com.chrome.devtools.json",
            "/api/chrome-devtools",
            "/peer/v1/com.chrome.devtools.json",
            "/peer/v1/chrome-devtools",
        )
        # Residual: GET http-opportunistic after empty RFC 8164 plate.
        http_opportunistic_paths = (
            "/.well-known/http-opportunistic",
            "/.well-known/http-opportunistic.json",
            "/http-opportunistic",
            "/http-opportunistic.json",
            "/api/http-opportunistic",
            "/peer/v1/http-opportunistic",
        )
        # Residual: GET /.well-known/core after empty CoRE/RFC 6690 plate.
        core_paths = (
            "/.well-known/core",
            "/.well-known/core.json",
            "/core",
            "/core.json",
            "/api/core",
            "/peer/v1/core",
        )
        # Residual: GET mercure after empty Mercure hub discovery plate.
        mercure_paths = (
            "/.well-known/mercure",
            "/.well-known/mercure.json",
            "/.well-known/mercure/subscriptions",
            "/mercure",
            "/mercure.json",
            "/mercure/subscriptions",
            "/api/mercure",
            "/peer/v1/mercure",
        )
        # Residual: GET gnap-as-rs after empty GNAP discovery plate.
        gnap_as_rs_paths = (
            "/.well-known/gnap-as-rs",
            "/.well-known/gnap-as-rs.json",
            "/.well-known/gnap",
            "/.well-known/gnap.json",
            "/gnap-as-rs",
            "/gnap-as-rs.json",
            "/gnap",
            "/gnap.json",
            "/api/gnap-as-rs",
            "/peer/v1/gnap-as-rs",
        )
        # Residual: GET csaf provider-metadata after empty CSAF plate.
        csaf_paths = (
            "/.well-known/csaf/provider-metadata.json",
            "/.well-known/csaf",
            "/.well-known/csaf.json",
            "/csaf/provider-metadata.json",
            "/csaf.json",
            "/api/csaf/provider-metadata.json",
            "/api/csaf",
            "/peer/v1/csaf/provider-metadata.json",
            "/peer/v1/csaf",
        )
        # Residual: GET discord after empty Discord domain verification plate.
        discord_paths = (
            "/.well-known/discord",
            "/.well-known/discord.json",
            "/discord",
            "/discord.json",
            "/api/discord",
            "/peer/v1/discord",
        )
        # Residual: GET jmap after empty JMAP session plate (RFC 8620).
        jmap_paths = (
            "/.well-known/jmap",
            "/.well-known/jmap.json",
            "/jmap",
            "/jmap.json",
            "/api/jmap",
            "/peer/v1/jmap",
        )
        # Residual: GET stun-key after empty STUN TLS key plate.
        stun_key_paths = (
            "/.well-known/stun-key",
            "/.well-known/stun-key.json",
            "/stun-key",
            "/stun-key.json",
            "/api/stun-key",
            "/peer/v1/stun-key",
        )
        # Residual: GET thread after empty Thread mesh plate.
        thread_paths = (
            "/.well-known/thread",
            "/.well-known/thread.json",
            "/thread",
            "/thread.json",
            "/api/thread",
            "/peer/v1/thread",
        )
        # Residual: GET coap after empty CoAP endpoints plate.
        coap_paths = (
            "/.well-known/coap",
            "/.well-known/coap.json",
            "/coap",
            "/coap.json",
            "/api/coap",
            "/peer/v1/coap",
        )
        # Residual: GET time after empty time-service plate.
        time_paths = (
            "/.well-known/time",
            "/.well-known/time.json",
            "/time",
            "/time.json",
            "/api/time",
            "/peer/v1/time",
        )
        # Residual: GET timezone after empty timezone plate.
        timezone_paths = (
            "/.well-known/timezone",
            "/.well-known/timezone.json",
            "/timezone",
            "/timezone.json",
            "/api/timezone",
            "/peer/v1/timezone",
        )
        # Residual: GET est after empty EST enrollment plate (RFC 7030).
        est_paths = (
            "/.well-known/est",
            "/.well-known/est.json",
            "/est",
            "/est.json",
            "/api/est",
            "/peer/v1/est",
        )
        # Residual: GET pki-validation after empty domain-validation plate.
        pki_validation_paths = (
            "/.well-known/pki-validation",
            "/.well-known/pki-validation.json",
            "/pki-validation",
            "/pki-validation.json",
            "/api/pki-validation",
            "/peer/v1/pki-validation",
        )
        # Residual: GET looking-glass after empty ISP LG plate.
        looking_glass_paths = (
            "/.well-known/looking-glass",
            "/.well-known/looking-glass.json",
            "/looking-glass",
            "/looking-glass.json",
            "/api/looking-glass",
            "/peer/v1/looking-glass",
        )
        # Residual: GET genid after empty named-information plate.
        genid_paths = (
            "/.well-known/genid",
            "/.well-known/genid.json",
            "/genid",
            "/genid.json",
            "/api/genid",
            "/peer/v1/genid",
        )
        # Residual: GET acme-challenge after empty ACME HTTP-01 plate.
        acme_challenge_paths = (
            "/.well-known/acme-challenge",
            "/.well-known/acme-challenge.json",
            "/acme-challenge",
            "/acme-challenge.json",
            "/api/acme-challenge",
            "/peer/v1/acme-challenge",
        )
        # Residual: GET ni after empty Named Information plate.
        ni_paths = (
            "/.well-known/ni",
            "/.well-known/ni.json",
            "/ni",
            "/ni.json",
            "/api/ni",
            "/peer/v1/ni",
        )
        # Residual: GET vapid after empty Web Push VAPID plate.
        vapid_paths = (
            "/.well-known/vapid",
            "/.well-known/vapid.json",
            "/vapid",
            "/vapid.json",
            "/api/vapid",
            "/peer/v1/vapid",
        )
        # Residual: GET hoba after empty HOBA (RFC 7486) plate.
        hoba_paths = (
            "/.well-known/hoba",
            "/.well-known/hoba.json",
            "/hoba",
            "/hoba.json",
            "/api/hoba",
            "/peer/v1/hoba",
        )
        # Residual: GET smime-aia after empty S/MIME AIA plate.
        smime_aia_paths = (
            "/.well-known/smime-aia",
            "/.well-known/smime-aia.json",
            "/smime-aia",
            "/smime-aia.json",
            "/api/smime-aia",
            "/peer/v1/smime-aia",
        )
        # Residual: GET browserid after empty BrowserID/Persona plate.
        browserid_paths = (
            "/.well-known/browserid",
            "/.well-known/browserid.json",
            "/browserid",
            "/browserid.json",
            "/api/browserid",
            "/peer/v1/browserid",
        )
        # Residual: GET idp-proxy after empty IdP proxy plate.
        idp_proxy_paths = (
            "/.well-known/idp-proxy",
            "/.well-known/idp-proxy.json",
            "/idp-proxy",
            "/idp-proxy.json",
            "/api/idp-proxy",
            "/peer/v1/idp-proxy",
        )
        # Residual: GET dnt after DNT companion plate (with dnt-policy.txt).
        # Name must not collide with dnt_policy_paths (policy txt plate).
        dnt_signal_paths = (
            "/.well-known/dnt",
            "/.well-known/dnt.json",
            "/dnt",
            "/dnt.json",
            "/api/dnt",
            "/peer/v1/dnt",
        )
        # Residual: GET funding-manifest-urls after empty OSS funding plate.
        funding_manifest_urls_paths = (
            "/.well-known/funding-manifest-urls",
            "/.well-known/funding-manifest-urls.json",
            "/funding-manifest-urls",
            "/funding-manifest-urls.json",
            "/api/funding-manifest-urls",
            "/peer/v1/funding-manifest-urls",
        )
        # Residual: GET xrpc-server-did after empty ATProto XRPC plate.
        xrpc_server_did_paths = (
            "/.well-known/xrpc-server-did",
            "/.well-known/xrpc-server-did.json",
            "/xrpc-server-did",
            "/xrpc-server-did.json",
            "/api/xrpc-server-did",
            "/peer/v1/xrpc-server-did",
        )
        # Residual: GET mcp.json after empty MCP discovery plate.
        mcp_json_paths = (
            "/.well-known/mcp.json",
            "/.well-known/mcp",
            "/mcp.json",
            "/api/mcp.json",
            "/peer/v1/mcp.json",
        )
        # Residual: GET web-bot-auth after empty Web Bot Auth plate.
        web_bot_auth_paths = (
            "/.well-known/web-bot-auth",
            "/.well-known/web-bot-auth.json",
            "/web-bot-auth",
            "/web-bot-auth.json",
            "/api/web-bot-auth",
            "/peer/v1/web-bot-auth",
        )
        # Residual: GET sbom after empty supply-chain plate.
        sbom_paths = (
            "/.well-known/sbom",
            "/.well-known/sbom.json",
            "/.well-known/supply-chain",
            "/sbom",
            "/sbom.json",
            "/api/sbom",
            "/peer/v1/sbom",
        )
        # Residual: GET privacy-pass after empty Privacy Pass plate.
        privacy_pass_paths = (
            "/.well-known/privacy-pass",
            "/.well-known/privacy-pass.json",
            "/privacy-pass",
            "/privacy-pass.json",
            "/api/privacy-pass",
            "/peer/v1/privacy-pass",
        )
        # Residual: GET ohttp-gateway after empty Oblivious HTTP plate.
        ohttp_gateway_paths = (
            "/.well-known/ohttp-gateway",
            "/.well-known/ohttp-gateway.json",
            "/.well-known/ohttp-config",
            "/.well-known/ohttp-config.json",
            "/ohttp-gateway",
            "/ohttp-config",
            "/api/ohttp-gateway",
            "/peer/v1/ohttp-gateway",
        )
        # Residual: GET masque after empty MASQUE proxy plate.
        masque_paths = (
            "/.well-known/masque",
            "/.well-known/masque.json",
            "/masque",
            "/masque.json",
            "/api/masque",
            "/peer/v1/masque",
        )
        # Residual: GET doh|dot after empty DoH/DoT plate.
        doh_paths = (
            "/.well-known/doh",
            "/.well-known/doh.json",
            "/.well-known/dot",
            "/.well-known/dot.json",
            "/doh",
            "/dot",
            "/api/doh",
            "/peer/v1/doh",
            "/api/dot",
            "/peer/v1/dot",
        )
        # Residual: GET bluesky after empty Bluesky/AppView plate.
        bluesky_paths = (
            "/.well-known/bluesky",
            "/.well-known/bluesky.json",
            "/bluesky",
            "/bluesky.json",
            "/api/bluesky",
            "/peer/v1/bluesky",
        )
        # Residual: GET solid after empty Solid Pod plate.
        solid_paths = (
            "/.well-known/solid",
            "/.well-known/solid.json",
            "/solid",
            "/solid.json",
            "/api/solid",
            "/peer/v1/solid",
        )
        # Residual: GET web-app-origin-association after empty PWA association plate.
        web_app_origin_association_paths = (
            "/.well-known/web-app-origin-association",
            "/.well-known/web-app-origin-association.json",
            "/web-app-origin-association",
            "/web-app-origin-association.json",
            "/api/web-app-origin-association",
            "/peer/v1/web-app-origin-association",
        )
        # Residual: GET doq|dns-query after empty DoQ plate (RFC 9250).
        doq_paths = (
            "/.well-known/doq",
            "/.well-known/doq.json",
            "/.well-known/dns-query",
            "/.well-known/dns-query.json",
            "/doq",
            "/doq.json",
            "/dns-query",
            "/dns-query.json",
            "/api/doq",
            "/peer/v1/doq",
            "/api/dns-query",
            "/peer/v1/dns-query",
        )
        # Residual: GET activitypub after empty ActivityPub/fediverse plate.
        activitypub_paths = (
            "/.well-known/activitypub",
            "/.well-known/activitypub.json",
            "/activitypub",
            "/activitypub.json",
            "/api/activitypub",
            "/peer/v1/activitypub",
        )
        # Residual: GET a2a after empty Agent2Agent discovery plate.
        a2a_paths = (
            "/.well-known/a2a",
            "/.well-known/a2a.json",
            "/.well-known/a2a-agent-card.json",
            "/.well-known/agent-card",
            "/a2a",
            "/a2a.json",
            "/api/a2a",
            "/peer/v1/a2a",
        )
        # Residual: GET token-issuer-directory after empty Privacy Pass issuer plate.
        token_issuer_directory_paths = (
            "/.well-known/token-issuer-directory",
            "/.well-known/token-issuer-directory.json",
            "/.well-known/private-token-issuer-directory",
            "/token-issuer-directory",
            "/private-token-issuer-directory",
            "/api/token-issuer-directory",
            "/peer/v1/token-issuer-directory",
            "/api/private-token-issuer-directory",
            "/peer/v1/private-token-issuer-directory",
        )
        # Residual: GET tls-rpt after empty TLS reporting plate (RFC 8460).
        tls_rpt_paths = (
            "/.well-known/tls-rpt",
            "/.well-known/tls-rpt.json",
            "/tls-rpt",
            "/tls-rpt.json",
            "/api/tls-rpt",
            "/peer/v1/tls-rpt",
        )
        # Residual: GET bimi after empty BIMI brand-indicator plate.
        bimi_paths = (
            "/.well-known/bimi",
            "/.well-known/bimi.json",
            "/bimi",
            "/bimi.json",
            "/api/bimi",
            "/peer/v1/bimi",
        )
        # Residual: GET statements.json after empty Twitter/X app association plate.
        statements_paths = (
            "/.well-known/statements.json",
            "/.well-known/statements",
            "/statements.json",
            "/api/statements.json",
            "/peer/v1/statements.json",
            "/api/statements",
            "/peer/v1/statements",
        )
        # Residual: GET humans.txt after peer gained humans plate.
        humans_paths = (
            "/humans.txt",
            "/.well-known/humans.txt",
            "/api/humans.txt",
            "/peer/v1/humans.txt",
        )
        # Residual: GET sitemap after peer gained empty lab-ops sitemap plate.
        sitemap_paths = (
            "/sitemap.xml",
            "/.well-known/sitemap.xml",
            "/sitemap_index.xml",
            "/api/sitemap.xml",
            "/peer/v1/sitemap.xml",
        )
        # Residual: GET llms.txt after peer gained llmstxt plate.
        llms_paths = (
            "/llms.txt",
            "/.well-known/llms.txt",
            "/.well-known/llm.txt",
            "/ai.txt",
            "/.well-known/ai.txt",
            "/.well-known/no-ai",
            "/.well-known/noai",
            "/.well-known/ai-policy",
            "/api/llms.txt",
            "/peer/v1/llms.txt",
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
        # Residual: GET schema dual-wire after peer gained schema plate.
        schema_paths = ("/schema", "/api/schema", "/peer/v1/schema")
        if path in info_paths:
            info = peer_json("GET", "/peer/v1/info", timeout=5)
            self._send(200, info if isinstance(info, dict) else {"ok": False, "info": info})
            return
        if path in jobs_coll:
            jobs = peer_json("GET", "/peer/v1/jobs", timeout=8)
            self._send(200, jobs if isinstance(jobs, dict) else {"ok": False, "jobs": jobs})
            return
        if path in control_paths:
            # Nested paths proxy to matching peer plate (not always root control).
            peer_path = path if path.startswith("/peer/") else path.replace(
                "/api/", "/peer/v1/", 1
            )
            if peer_path in ("/peer/v1/control", "/peer/v1/control/"):
                peer_path = "/peer/v1/control"
            ctl = peer_json("GET", peer_path, timeout=5)
            if not isinstance(ctl, dict):
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
            # Nested /auth/status keeps action=auth_status via matching path.
            if path.endswith("/auth/status"):
                peer_auth = path if path.startswith("/peer/") else "/api/auth/status"
            else:
                peer_auth = "/api/auth"
            auth = peer_json("GET", peer_auth, timeout=5)
            if not isinstance(auth, dict):
                auth = peer_json("GET", "/api/auth", timeout=5)
            self._send(
                200, auth if isinstance(auth, dict) else {"ok": False, "auth": auth}
            )
            return
        if path in whoami_paths:
            # Prefer matching peer path so action leaf is whoami|me.
            peer_path = (
                path
                if path.startswith("/peer/") or path in ("/whoami", "/me")
                else path.replace("/api/", "/peer/v1/", 1)
            )
            if peer_path in ("/whoami", "/me"):
                peer_path = f"/peer/v1{peer_path}"
            who = peer_json("GET", peer_path, timeout=5)
            if not isinstance(who, dict):
                who = peer_json("GET", "/api/whoami", timeout=5)
            self._send(
                200, who if isinstance(who, dict) else {"ok": False, "whoami": who}
            )
            return
        if path in session_paths:
            peer_path = (
                "/peer/v1/session"
                if path in ("/session", "/api/session", "/peer/v1/session")
                else path
            )
            sess = peer_json("GET", peer_path, timeout=5)
            if not isinstance(sess, dict):
                sess = peer_json("GET", "/api/auth", timeout=5)
            self._send(
                200, sess if isinstance(sess, dict) else {"ok": False, "session": sess}
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
        if path in schema_paths:
            sch = peer_json("GET", "/api/schema", timeout=5)
            self._send(
                200, sch if isinstance(sch, dict) else {"ok": False, "schema_plate": sch}
            )
            return
        if path in favicon_paths:
            # Peer serves image/svg+xml; MCP mesh probes want dual-wire JSON.
            is_apple = "apple-touch-icon" in path
            if is_apple:
                peer_path = "/apple-touch-icon.png"
                action = "apple_touch_icon"
            elif path.endswith(".svg") or path.endswith("favicon.svg"):
                peer_path = "/favicon.svg"
                action = "favicon"
            else:
                peer_path = "/favicon.ico"
                action = "favicon"
            self._send(
                200,
                {
                    "schema": "nanobot.peer_http.v1",
                    "ok": True,
                    "action": action,
                    "service": "blackcube-nanobot-http-mcp",
                    "content_type": "image/svg+xml",
                    "peer_path": peer_path,
                    "theme_color": "#00e5ff",
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
        if path in service_worker_paths:
            # Peer serves application/javascript unregister SW; MCP dual-wire JSON.
            self._send(
                200,
                {
                    "schema": "nanobot.peer_http.v1",
                    "ok": True,
                    "action": "service_worker",
                    "service": "blackcube-nanobot-http-mcp",
                    "content_type": "application/javascript",
                    "peer_path": "/service-worker.js",
                    "pwa": False,
                    "behavior": "unregister_on_activate",
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
        if path in ads_paths:
            # Peer serves text/plain no-sellers; MCP dual-wire JSON.
            peer_path = "/app-ads.txt" if "app-ads" in path else "/ads.txt"
            self._send(
                200,
                {
                    "schema": "nanobot.peer_http.v1",
                    "ok": True,
                    "action": "ads",
                    "service": "blackcube-nanobot-http-mcp",
                    "content_type": "text/plain",
                    "peer_path": peer_path,
                    "authorized_sellers": 0,
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
        if path in sellers_paths:
            # Peer serves IAB sellers.json; proxy dual-wire JSON.
            doc = peer_json("GET", "/sellers.json", timeout=5)
            self._send(
                200, doc if isinstance(doc, dict) else {"ok": False, "sellers": doc}
            )
            return
        if path in ai_plugin_paths:
            # Peer serves OpenAI ai-plugin.json; proxy dual-wire JSON.
            doc = peer_json("GET", "/.well-known/ai-plugin.json", timeout=5)
            self._send(
                200, doc if isinstance(doc, dict) else {"ok": False, "ai_plugin": doc}
            )
            return
        if path in agent_card_paths:
            # Peer serves private agent-card; proxy dual-wire JSON.
            doc = peer_json("GET", "/.well-known/agent-card.json", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "agent_card": doc},
            )
            return
        if path in assetlinks_paths:
            # Peer serves empty DAL []; MCP dual-wire JSON (not a raw array).
            self._send(
                200,
                {
                    "schema": "nanobot.peer_http.v1",
                    "ok": True,
                    "action": "assetlinks",
                    "service": "blackcube-nanobot-http-mcp",
                    "content_type": "application/json",
                    "peer_path": "/.well-known/assetlinks.json",
                    "statements": [],
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
        if path in aasa_paths:
            # Peer serves empty AASA; proxy dual-wire JSON.
            doc = peer_json(
                "GET", "/.well-known/apple-app-site-association", timeout=5
            )
            self._send(
                200, doc if isinstance(doc, dict) else {"ok": False, "aasa": doc}
            )
            return
        if path in gpc_paths:
            # Peer serves GPC honor plate; proxy dual-wire JSON.
            doc = peer_json("GET", "/.well-known/gpc.json", timeout=5)
            self._send(
                200, doc if isinstance(doc, dict) else {"ok": False, "gpc": doc}
            )
            return
        if path in tdmrep_paths:
            # Peer serves TDM reservation plate; proxy dual-wire JSON.
            doc = peer_json("GET", "/.well-known/tdmrep.json", timeout=5)
            self._send(
                200, doc if isinstance(doc, dict) else {"ok": False, "tdmrep": doc}
            )
            return
        if path in mta_sts_paths:
            # Peer serves text/plain MTA-STS mode=none; MCP dual-wire JSON.
            self._send(
                200,
                {
                    "schema": "nanobot.peer_http.v1",
                    "ok": True,
                    "action": "mta_sts",
                    "service": "blackcube-nanobot-http-mcp",
                    "content_type": "text/plain",
                    "peer_path": "/.well-known/mta-sts.txt",
                    "version": "STSv1",
                    "mode": "none",
                    "max_age": 86400,
                    "mta_sts": False,
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
        if path in caldav_paths:
            # Peer serves empty CalDAV discovery; proxy dual-wire JSON.
            doc = peer_json("GET", "/.well-known/caldav", timeout=5)
            self._send(
                200, doc if isinstance(doc, dict) else {"ok": False, "caldav": doc}
            )
            return
        if path in carddav_paths:
            # Peer serves empty CardDAV discovery; proxy dual-wire JSON.
            doc = peer_json("GET", "/.well-known/carddav", timeout=5)
            self._send(
                200, doc if isinstance(doc, dict) else {"ok": False, "carddav": doc}
            )
            return
        if path in api_catalog_paths:
            # Peer serves API catalog linkset; proxy dual-wire JSON.
            doc = peer_json("GET", "/.well-known/api-catalog", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "api_catalog": doc},
            )
            return
        if path in dnt_policy_paths:
            # Peer serves text/plain DNT policy; MCP dual-wire JSON.
            self._send(
                200,
                {
                    "schema": "nanobot.peer_http.v1",
                    "ok": True,
                    "action": "dnt_policy",
                    "service": "blackcube-nanobot-http-mcp",
                    "content_type": "text/plain",
                    "peer_path": "/.well-known/dnt-policy.txt",
                    "dnt_honored": True,
                    "gpc_companion": "/.well-known/gpc.json",
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
        if path in passkey_paths:
            # Peer serves empty passkey-endpoints; proxy dual-wire JSON.
            doc = peer_json(
                "GET", "/.well-known/passkey-endpoints", timeout=5
            )
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "passkeys": doc},
            )
            return
        if path in webfinger_paths:
            # Peer serves empty WebFinger JRD; proxy dual-wire JSON.
            doc = peer_json("GET", "/.well-known/webfinger", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "webfinger": doc},
            )
            return
        if path in nodeinfo_paths:
            # Peer serves empty NodeInfo discovery; proxy dual-wire JSON.
            doc = peer_json("GET", "/.well-known/nodeinfo", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "nodeinfo": doc},
            )
            return
        if path in host_meta_paths:
            # Peer serves empty host-meta JRD; proxy dual-wire JSON.
            doc = peer_json("GET", "/.well-known/host-meta", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "host_meta": doc},
            )
            return
        if path in matrix_client_paths:
            # Peer serves empty Matrix client discovery; proxy dual-wire JSON.
            doc = peer_json("GET", "/.well-known/matrix/client", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "matrix_client": doc},
            )
            return
        if path in matrix_server_paths:
            # Peer serves empty Matrix server discovery; proxy dual-wire JSON.
            doc = peer_json("GET", "/.well-known/matrix/server", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "matrix_server": doc},
            )
            return
        if path in openid_paths:
            # Peer serves non-OP openid-configuration; proxy dual-wire JSON.
            doc = peer_json(
                "GET", "/.well-known/openid-configuration", timeout=5
            )
            self._send(
                200, doc if isinstance(doc, dict) else {"ok": False, "openid": doc}
            )
            return
        if path in openid_fed_paths:
            # Peer serves non-entity openid-federation; proxy dual-wire JSON.
            doc = peer_json(
                "GET", "/.well-known/openid-federation", timeout=5
            )
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "openid_federation": doc},
            )
            return
        if path in uma2_paths:
            # Peer serves non-UMA uma2-configuration; proxy dual-wire JSON.
            doc = peer_json(
                "GET", "/.well-known/uma2-configuration", timeout=5
            )
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "uma2": doc},
            )
            return
        if path in oid4vci_paths:
            # Peer serves non-issuer openid-credential-issuer; proxy dual-wire JSON.
            doc = peer_json(
                "GET", "/.well-known/openid-credential-issuer", timeout=5
            )
            self._send(
                200,
                doc
                if isinstance(doc, dict)
                else {"ok": False, "openid_credential_issuer": doc},
            )
            return
        if path in fido2_paths:
            # Peer serves non-FIDO2 fido2-configuration; proxy dual-wire JSON.
            doc = peer_json(
                "GET", "/.well-known/fido2-configuration", timeout=5
            )
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "fido2": doc},
            )
            return
        if path in webauthn_paths:
            # Peer serves empty WebAuthn related-origins; proxy dual-wire JSON.
            doc = peer_json("GET", "/.well-known/webauthn", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "webauthn": doc},
            )
            return
        if path in did_json_paths:
            # Peer serves empty did:web document; proxy dual-wire JSON.
            doc = peer_json("GET", "/.well-known/did.json", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "did_json": doc},
            )
            return
        if path in did_cfg_paths:
            # Peer serves empty DID domain linkage; proxy dual-wire JSON.
            doc = peer_json(
                "GET", "/.well-known/did-configuration", timeout=5
            )
            self._send(
                200,
                doc
                if isinstance(doc, dict)
                else {"ok": False, "did_configuration": doc},
            )
            return
        if path in oauth_as_paths:
            # Peer serves non-AS RFC 8414 plate; proxy dual-wire JSON.
            doc = peer_json(
                "GET", "/.well-known/oauth-authorization-server", timeout=5
            )
            self._send(
                200, doc if isinstance(doc, dict) else {"ok": False, "oauth_as": doc}
            )
            return
        if path in oauth_reg_paths:
            # Peer serves non-DCR registration plate; proxy dual-wire JSON.
            doc = peer_json(
                "GET", "/.well-known/oauth-client-registration", timeout=5
            )
            self._send(
                200,
                doc
                if isinstance(doc, dict)
                else {"ok": False, "oauth_client_registration": doc},
            )
            return
        if path in oauth_pr_paths:
            # Peer serves RFC 9728 protected-resource plate; proxy dual-wire JSON.
            doc = peer_json(
                "GET", "/.well-known/oauth-protected-resource", timeout=5
            )
            self._send(
                200, doc if isinstance(doc, dict) else {"ok": False, "oauth_pr": doc}
            )
            return
        if path in crossdomain_paths:
            # Peer serves deny-all policy XML; MCP dual-wire JSON.
            self._send(
                200,
                {
                    "schema": "nanobot.peer_http.v1",
                    "ok": True,
                    "action": "crossdomain",
                    "service": "blackcube-nanobot-http-mcp",
                    "content_type": "text/x-cross-domain-policy",
                    "peer_path": "/crossdomain.xml",
                    "permitted_cross_domain_policies": "none",
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
        if path in browserconfig_paths:
            # Peer serves MS tile XML; MCP dual-wire JSON.
            self._send(
                200,
                {
                    "schema": "nanobot.peer_http.v1",
                    "ok": True,
                    "action": "browserconfig",
                    "service": "blackcube-nanobot-http-mcp",
                    "content_type": "application/xml",
                    "peer_path": "/browserconfig.xml",
                    "tile_color": "#0a0a0a",
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
        if path in change_password_paths:
            # Peer 302s to /api/auth; MCP dual-wire JSON (no local password store).
            self._send(
                200,
                {
                    "schema": "nanobot.peer_http.v1",
                    "ok": True,
                    "action": "change_password",
                    "service": "blackcube-nanobot-http-mcp",
                    "peer_path": "/.well-known/change-password",
                    "redirect": "/api/auth",
                    "http_status": 302,
                    "local_password": False,
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
        if path in prompt_meta_paths:
            doc = peer_json("GET", path.rstrip("/") or path, timeout=5)
            if not isinstance(doc, dict):
                doc = peer_json("GET", "/peer/v1/prompt", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "prompt": doc},
            )
            return
        if path in chat_meta_paths:
            doc = peer_json("GET", "/api/chat", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "chat": doc},
            )
            return
        if path in auth_start_meta_paths:
            doc = peer_json("GET", "/api/auth/start", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "auth_start": doc},
            )
            return
        if path in mcp_probe_meta_paths:
            doc = peer_json("GET", "/api/mcp/probe", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "mcp_probe": doc},
            )
            return
        if path in shell_gate_meta_paths:
            doc = peer_json("GET", "/peer/v1/shell/gate", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "shell_gate": doc},
            )
            return
        if path in shell_approve_meta_paths:
            doc = peer_json("GET", "/peer/v1/shell/approve", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "shell_approve": doc},
            )
            return
        if path in shell_approvals_paths:
            doc = peer_json("GET", "/peer/v1/shell/approvals", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "shell_approvals": doc},
            )
            return
        if path in shell_meta_paths:
            doc = peer_json("GET", path.rstrip("/") or path, timeout=5)
            if not isinstance(doc, dict):
                doc = peer_json("GET", "/peer/v1/shell", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "shell": doc},
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
        if path in trust_txt_paths:
            # Peer serves text/plain empty trust plate; MCP dual-wire JSON.
            self._send(
                200,
                {
                    "schema": "nanobot.peer_http.v1",
                    "ok": True,
                    "action": "trust_txt",
                    "service": "blackcube-nanobot-http-mcp",
                    "content_type": "text/plain",
                    "peer_path": "/.well-known/trust.txt",
                    "trust_txt": True,
                    "memberships": [],
                    "control": [],
                    "security_txt": "/.well-known/security.txt",
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
        if path in keybase_txt_paths:
            # Peer serves text/plain empty Keybase proofs; MCP dual-wire JSON.
            self._send(
                200,
                {
                    "schema": "nanobot.peer_http.v1",
                    "ok": True,
                    "action": "keybase_txt",
                    "service": "blackcube-nanobot-http-mcp",
                    "content_type": "text/plain",
                    "peer_path": "/.well-known/keybase.txt",
                    "keybase_txt": True,
                    "proofs": [],
                    "username": "",
                    "security_txt": "/.well-known/security.txt",
                    "trust_txt": "/.well-known/trust.txt",
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
        if path in pgp_key_txt_paths:
            # Peer serves text/plain empty OpenPGP plate; MCP dual-wire JSON.
            self._send(
                200,
                {
                    "schema": "nanobot.peer_http.v1",
                    "ok": True,
                    "action": "pgp_key_txt",
                    "service": "blackcube-nanobot-http-mcp",
                    "content_type": "text/plain",
                    "peer_path": "/.well-known/pgp-key.txt",
                    "pgp_key_txt": True,
                    "openpgp_published": False,
                    "armored_key": "",
                    "security_txt": "/.well-known/security.txt",
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
        if path in openpgpkey_paths:
            # Peer WKD plate / policy; dual-wire JSON for mesh probes.
            if path.endswith("/policy"):
                self._send(
                    200,
                    {
                        "schema": "nanobot.peer_http.v1",
                        "ok": True,
                        "action": "openpgpkey",
                        "service": "blackcube-nanobot-http-mcp",
                        "content_type": "text/plain",
                        "peer_path": "/.well-known/openpgpkey/policy",
                        "wkd": False,
                        "openpgpkey": False,
                        "policy": True,
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
            doc = peer_json("GET", "/.well-known/openpgpkey", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "openpgpkey": doc},
            )
            return
        if path in sshfp_paths:
            doc = peer_json("GET", "/.well-known/sshfp", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "sshfp": doc},
            )
            return
        if path in jwks_paths:
            doc = peer_json("GET", "/.well-known/jwks.json", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "jwks": doc},
            )
            return
        if path in oauth_endpoint_paths:
            doc = peer_json("GET", path, timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "oauth_endpoint": doc},
            )
            return
        if path in discovery_paths:
            doc = peer_json("GET", path, timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "discovery": doc},
            )
            return
        if path in related_website_set_paths:
            doc = peer_json("GET", "/.well-known/related-website-set.json", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "related_website_set": doc},
            )
            return
        if path in microsoft_identity_association_paths:
            doc = peer_json(
                "GET", "/.well-known/microsoft-identity-association.json", timeout=5
            )
            self._send(
                200,
                doc
                if isinstance(doc, dict)
                else {"ok": False, "microsoft_identity_association": doc},
            )
            return
        if path in apple_merchantid_domain_association_paths:
            doc = peer_json(
                "GET",
                "/.well-known/apple-developer-merchantid-domain-association",
                timeout=5,
            )
            self._send(
                200,
                doc
                if isinstance(doc, dict)
                else {"ok": False, "apple_merchantid_domain_association": doc},
            )
            return
        if path in nostr_paths:
            doc = peer_json("GET", "/.well-known/nostr.json", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "nostr": doc},
            )
            return
        if path in atproto_did_paths:
            doc = peer_json("GET", "/.well-known/atproto-did", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "atproto_did": doc},
            )
            return
        if path in stellar_toml_paths:
            # Peer serves text/plain empty SEP-0001; MCP dual-wire JSON.
            self._send(
                200,
                {
                    "schema": "nanobot.peer_http.v1",
                    "ok": True,
                    "action": "stellar_toml",
                    "service": "blackcube-nanobot-http-mcp",
                    "content_type": "text/plain",
                    "peer_path": "/.well-known/stellar.toml",
                    "stellar_toml": True,
                    "sep0001": False,
                    "security_txt": "/.well-known/security.txt",
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
        if path in web_identity_paths:
            doc = peer_json("GET", "/.well-known/web-identity", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "web_identity": doc},
            )
            return
        if path in posh_paths:
            doc = peer_json("GET", "/.well-known/posh", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "posh": doc},
            )
            return
        if path in traffic_advice_paths:
            doc = peer_json("GET", "/.well-known/traffic-advice", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "traffic_advice": doc},
            )
            return
        if path in privacy_sandbox_paths:
            doc = peer_json(
                "GET", "/.well-known/privacy-sandbox-attestations.json", timeout=5
            )
            self._send(
                200,
                doc
                if isinstance(doc, dict)
                else {"ok": False, "privacy_sandbox_attestations": doc},
            )
            return
        if path in no_federation_paths:
            doc = peer_json(
                "GET",
                "/.well-known/resource-that-should-not-be-used-for-federation",
                timeout=5,
            )
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "no_federation": doc},
            )
            return
        if path in chrome_devtools_paths:
            doc = peer_json(
                "GET",
                "/.well-known/appspecific/com.chrome.devtools.json",
                timeout=5,
            )
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "chrome_devtools": doc},
            )
            return
        if path in http_opportunistic_paths:
            doc = peer_json("GET", "/.well-known/http-opportunistic", timeout=5)
            self._send(
                200,
                doc
                if isinstance(doc, dict)
                else {"ok": False, "http_opportunistic": doc},
            )
            return
        if path in core_paths:
            doc = peer_json("GET", "/.well-known/core", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "core": doc},
            )
            return
        if path in mercure_paths:
            doc = peer_json("GET", "/.well-known/mercure", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "mercure": doc},
            )
            return
        if path in gnap_as_rs_paths:
            doc = peer_json("GET", "/.well-known/gnap-as-rs", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "gnap_as_rs": doc},
            )
            return
        if path in csaf_paths:
            doc = peer_json(
                "GET", "/.well-known/csaf/provider-metadata.json", timeout=5
            )
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "csaf": doc},
            )
            return
        if path in discord_paths:
            doc = peer_json("GET", "/.well-known/discord", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "discord": doc},
            )
            return
        if path in jmap_paths:
            doc = peer_json("GET", "/.well-known/jmap", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "jmap": doc},
            )
            return
        if path in stun_key_paths:
            doc = peer_json("GET", "/.well-known/stun-key", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "stun_key": doc},
            )
            return
        if path in thread_paths:
            doc = peer_json("GET", "/.well-known/thread", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "thread": doc},
            )
            return
        if path in coap_paths:
            doc = peer_json("GET", "/.well-known/coap", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "coap": doc},
            )
            return
        if path in time_paths:
            doc = peer_json("GET", "/.well-known/time", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "time": doc},
            )
            return
        if path in timezone_paths:
            doc = peer_json("GET", "/.well-known/timezone", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "timezone": doc},
            )
            return
        if path in est_paths:
            doc = peer_json("GET", "/.well-known/est", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "est": doc},
            )
            return
        if path in pki_validation_paths:
            doc = peer_json("GET", "/.well-known/pki-validation", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "pki_validation": doc},
            )
            return
        if path in looking_glass_paths:
            doc = peer_json("GET", "/.well-known/looking-glass", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "looking_glass": doc},
            )
            return
        if path in genid_paths:
            doc = peer_json("GET", "/.well-known/genid", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "genid": doc},
            )
            return
        if path in acme_challenge_paths:
            doc = peer_json("GET", "/.well-known/acme-challenge", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "acme_challenge": doc},
            )
            return
        if path in ni_paths:
            doc = peer_json("GET", "/.well-known/ni", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "ni": doc},
            )
            return
        if path in vapid_paths:
            doc = peer_json("GET", "/.well-known/vapid", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "vapid": doc},
            )
            return
        if path in hoba_paths:
            doc = peer_json("GET", "/.well-known/hoba", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "hoba": doc},
            )
            return
        if path in smime_aia_paths:
            doc = peer_json("GET", "/.well-known/smime-aia", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "smime_aia": doc},
            )
            return
        if path in browserid_paths:
            doc = peer_json("GET", "/.well-known/browserid", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "browserid": doc},
            )
            return
        if path in idp_proxy_paths:
            doc = peer_json("GET", "/.well-known/idp-proxy", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "idp_proxy": doc},
            )
            return
        if path in dnt_signal_paths:
            doc = peer_json("GET", "/.well-known/dnt", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "dnt": doc},
            )
            return
        if path in funding_manifest_urls_paths:
            doc = peer_json("GET", "/.well-known/funding-manifest-urls", timeout=5)
            self._send(
                200,
                doc
                if isinstance(doc, dict)
                else {"ok": False, "funding_manifest_urls": doc},
            )
            return
        if path in xrpc_server_did_paths:
            doc = peer_json("GET", "/.well-known/xrpc-server-did", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "xrpc_server_did": doc},
            )
            return
        if path in mcp_json_paths:
            doc = peer_json("GET", "/.well-known/mcp.json", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "mcp_json": doc},
            )
            return
        if path in web_bot_auth_paths:
            doc = peer_json("GET", "/.well-known/web-bot-auth", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "web_bot_auth": doc},
            )
            return
        if path in sbom_paths:
            doc = peer_json("GET", "/.well-known/sbom", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "sbom": doc},
            )
            return
        if path in privacy_pass_paths:
            doc = peer_json("GET", "/.well-known/privacy-pass", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "privacy_pass": doc},
            )
            return
        if path in ohttp_gateway_paths:
            doc = peer_json("GET", "/.well-known/ohttp-gateway", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "ohttp_gateway": doc},
            )
            return
        if path in masque_paths:
            doc = peer_json("GET", "/.well-known/masque", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "masque": doc},
            )
            return
        if path in doh_paths:
            doc = peer_json("GET", "/.well-known/doh", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "doh": doc},
            )
            return
        if path in bluesky_paths:
            doc = peer_json("GET", "/.well-known/bluesky", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "bluesky": doc},
            )
            return
        if path in solid_paths:
            doc = peer_json("GET", "/.well-known/solid", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "solid": doc},
            )
            return
        if path in web_app_origin_association_paths:
            doc = peer_json(
                "GET", "/.well-known/web-app-origin-association", timeout=5
            )
            self._send(
                200,
                doc
                if isinstance(doc, dict)
                else {"ok": False, "web_app_origin_association": doc},
            )
            return
        if path in doq_paths:
            doc = peer_json("GET", "/.well-known/doq", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "doq": doc},
            )
            return
        if path in activitypub_paths:
            doc = peer_json("GET", "/.well-known/activitypub", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "activitypub": doc},
            )
            return
        if path in a2a_paths:
            doc = peer_json("GET", "/.well-known/a2a", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "a2a": doc},
            )
            return
        if path in token_issuer_directory_paths:
            doc = peer_json(
                "GET", "/.well-known/token-issuer-directory", timeout=5
            )
            self._send(
                200,
                doc
                if isinstance(doc, dict)
                else {"ok": False, "token_issuer_directory": doc},
            )
            return
        if path in tls_rpt_paths:
            doc = peer_json("GET", "/.well-known/tls-rpt", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "tls_rpt": doc},
            )
            return
        if path in bimi_paths:
            doc = peer_json("GET", "/.well-known/bimi", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "bimi": doc},
            )
            return
        if path in statements_paths:
            doc = peer_json("GET", "/.well-known/statements.json", timeout=5)
            self._send(
                200,
                doc if isinstance(doc, dict) else {"ok": False, "statements": doc},
            )
            return
        if path in humans_paths:
            # Peer serves text/plain; MCP mesh probes want dual-wire JSON.
            self._send(
                200,
                {
                    "schema": "nanobot.peer_http.v1",
                    "ok": True,
                    "action": "humans",
                    "service": "blackcube-nanobot-http-mcp",
                    "content_type": "text/plain",
                    "peer_path": "/humans.txt",
                    "maintainer": "Abyss-c0re",
                    "site": "https://github.com/Abyss-c0re/nanobot",
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
        if path in sitemap_paths:
            # Peer serves empty urlset XML; MCP mesh probes want dual-wire JSON.
            self._send(
                200,
                {
                    "schema": "nanobot.peer_http.v1",
                    "ok": True,
                    "action": "sitemap",
                    "service": "blackcube-nanobot-http-mcp",
                    "content_type": "application/xml",
                    "peer_path": "/sitemap.xml",
                    "urls": 0,
                    "robots_disallow": "/",
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
        if path in llms_paths:
            # Peer serves text/plain; MCP mesh probes want dual-wire JSON.
            self._send(
                200,
                {
                    "schema": "nanobot.peer_http.v1",
                    "ok": True,
                    "action": "llms",
                    "service": "blackcube-nanobot-http-mcp",
                    "content_type": "text/plain",
                    "peer_path": "/llms.txt",
                    "site": "https://github.com/Abyss-c0re/nanobot",
                    "robots_disallow": "/",
                    "peer_http": "lab_ops_only",
                    "product_wire": "smx2",
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
