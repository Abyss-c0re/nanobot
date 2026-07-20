#!/usr/bin/env python3
"""Authenticated TCP shell (password or token). Fallback when OpenSSH is absent."""
from __future__ import annotations
import os, sys, socket, select, pty, threading

HOST = os.environ.get("SHELL_SERVER_BIND", "0.0.0.0")
PORT = int(os.environ.get("SSH_PORT", os.environ.get("SHELL_SERVER_PORT", "22")))
PASSWORD = os.environ.get("SSH_PASSWORD", "")
AUTH_KEY = os.environ.get("SSH_SHELL_KEY", os.environ.get("SSH_KEY_TOKEN", ""))
SHELL = os.environ.get("SHELL_SERVER_SHELL", "/bin/bash")

def log(msg: str) -> None:
    print(f"shell_server: {msg}", file=sys.stderr, flush=True)

def check_auth(line: str) -> bool:
    line = line.strip()
    if line.startswith("PASS ") and PASSWORD and line[5:] == PASSWORD:
        return True
    if line.startswith("KEY ") and AUTH_KEY and line[4:] == AUTH_KEY:
        return True
    if PASSWORD and line == PASSWORD:
        return True
    if AUTH_KEY and line == AUTH_KEY:
        return True
    return False

def handle(conn: socket.socket, addr) -> None:
    try:
        conn.sendall(b"nanobot-shell auth: PASS <password> | KEY <token>\n")
        conn.settimeout(60)
        data = b""
        while b"\n" not in data and b"\r" not in data:
            chunk = conn.recv(256)
            if not chunk:
                return
            data += chunk
            if len(data) > 4096:
                return
        line = data.decode("utf-8", "replace").splitlines()[0] if data else ""
        if not check_auth(line):
            conn.sendall(b"auth failed\n")
            conn.close()
            return
        conn.sendall(b"ok\n")
        pid, fd = pty.fork()
        if pid == 0:
            env = os.environ.copy()
            env["HOME"] = env.get("HOME", "/home/nanobot")
            env["TERM"] = env.get("TERM", "xterm")
            os.chdir(env["HOME"])
            os.execvpe(SHELL, [SHELL, "-l"], env)
        while True:
            r, _, _ = select.select([conn, fd], [], [], 120)
            if not r:
                continue
            if conn in r:
                b = conn.recv(4096)
                if not b:
                    break
                os.write(fd, b)
            if fd in r:
                try:
                    b = os.read(fd, 4096)
                except OSError:
                    break
                if not b:
                    break
                conn.sendall(b)
    finally:
        try:
            conn.close()
        except Exception:
            pass

def main() -> int:
    global PASSWORD
    if not PASSWORD and not AUTH_KEY:
        import secrets
        PASSWORD = secrets.token_urlsafe(12)
        home = os.environ.get("NANOBOT_HOME", "/home/nanobot/.nanobot")
        os.makedirs(home, exist_ok=True)
        path = os.path.join(home, "ssh_ephemeral_password")
        with open(path, "w") as f:
            f.write(PASSWORD + "\n")
        os.chmod(path, 0o600)
        log(f"ephemeral password: {PASSWORD} (saved {path})")
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((HOST, PORT))
    srv.listen(16)
    log(f"listen {HOST}:{PORT}")
    while True:
        conn, addr = srv.accept()
        threading.Thread(target=handle, args=(conn, addr), daemon=True).start()

if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(0)
