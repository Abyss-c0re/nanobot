#!/usr/bin/env python3
"""Legacy entrypoint — re-exec universal Clanker MCP bridge.

Old configs pointed here (peer-only). Full stack (SAM speak, vacuum, music, peer)
lives in Clanker/scripts/clanker_mcp_bridge.py.
"""
from __future__ import annotations

import os
import sys
from pathlib import Path

_univ = Path(__file__).resolve().parents[2] / "scripts" / "clanker_mcp_bridge.py"
if not _univ.is_file():
    # compat symlink layout: Dev/nanobot -> Clanker/nanobot
    _univ = Path("/home/voldemar/Dev/Clanker/scripts/clanker_mcp_bridge.py")
if not _univ.is_file():
    sys.stderr.write(f"clanker universal MCP missing: {_univ}\n")
    sys.exit(1)
os.execv(sys.executable, [sys.executable, str(_univ), *sys.argv[1:]])
