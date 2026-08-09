"""BrainCube ecosystem API helpers (shared)."""
from __future__ import annotations
import json, os, subprocess, urllib.error, urllib.request
from pathlib import Path

def peer_base() -> str:
    b = (os.environ.get("NANOBOT_PEER_URL") or "").strip()
    if b: return b.rstrip("/")
    for p in (Path.home()/'.nanobot'/'peer_url',):
        try:
            for line in p.read_text().splitlines():
                line=line.strip()
                if line and not line.startswith('#'): return line.rstrip('/')
        except OSError: pass
    return 'http://127.0.0.1:18787'

def peer_token() -> str:
    tok = (os.environ.get('NANOBOT_PEER_TOKEN') or '').strip()
    if tok: return tok[6:] if tok.startswith('token=') else tok
    for p in (Path.home()/'.nanobot'/'peer_token',):
        try:
            line = p.read_text().strip().splitlines()[0].strip()
            return line[6:] if line.startswith('token=') else line
        except OSError: pass
    return ''

def http_json(method, path, payload=None, timeout=30):
    data = None if payload is None else json.dumps(payload).encode()
    req = urllib.request.Request(f"{peer_base()}{path}", data=data, method=method)
    req.add_header('Content-Type', 'application/json')
    tok = peer_token()
    if tok:
        req.add_header('X-Nanobot-Peer-Token', tok)
        req.add_header('Authorization', f'Bearer {tok}')
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode() or '{}')
    except urllib.error.HTTPError as e:
        body = e.read().decode('utf-8', 'replace')
        try: return json.loads(body)
        except Exception: return {'error': body or str(e), 'http_status': e.code}
    except Exception as e:
        return {'error': str(e)}

def bc(action, timeout=20):
    return http_json('POST', '/api/braincube', {'action': action}, timeout=timeout)

ALLOW = ['instinct_queen','external_contract','manager_motivate','smx_filter','nexus_heartbeat']

def cubalc_run(board='', source=''):
    bin_p = Path(os.environ.get('CUBALC_BIN') or str(Path.home()/'Dev/cubalc/out/cubalc'))
    root = Path(os.environ.get('CUBALC_ROOT') or str(Path.home()/'Dev/cubalc'))
    if not bin_p.is_file(): return {'ok': False, 'error': f'missing {bin_p}'}
    try:
        if board:
            name = board.replace('.cubalc','').strip()
            if name not in ALLOW: return {'ok': False, 'error': f'not allowlisted: {name}', 'allow': ALLOW}
            path = root/'programs'/'hive_mind'/f'{name}.cubalc'
            if not path.is_file(): return {'ok': False, 'error': f'missing {path}'}
            p = subprocess.run([str(bin_p),'run',str(path)], cwd=str(root), capture_output=True, text=True, timeout=20)
        elif source:
            if len(source)>4000: return {'ok': False, 'error': 'source too long'}
            p = subprocess.run([str(bin_p),'run','-'], input=source, cwd=str(root), capture_output=True, text=True, timeout=20)
        else:
            return {'ok': False, 'error': 'need board or source'}
        return {'ok': p.returncode==0, 'exit': p.returncode, 'stdout': (p.stdout or '')[-12000:], 'stderr': (p.stderr or '')[-3000:], 'schema': 'cubalc.run.v1', 'wire': 'smx2', 'hold_flash': 1}
    except Exception as e:
        return {'ok': False, 'error': str(e)}

def braincell_status():
    home = Path(os.environ.get('NANOBOT_HOME') or str(Path.home()/'.nanobot'))
    d = home/'braincells'
    cells = []
    if d.is_dir():
        for p in sorted(d.glob('*.json')):
            try: cells.append({'file': p.name, 'body': json.loads(p.read_text())})
            except Exception as e: cells.append({'file': p.name, 'error': str(e)})
    live = bc('live', 10)
    return {'ok': True, 'schema': 'braincell.status.v1', 'scope': 'ecosystem', 'cells': cells, 'seq': live.get('seq'), 'law': live.get('law'), 'live': live.get('live')}

def info():
    live = bc('live', 10)
    return {'ok': True, 'schema': 'braincube.bridge.v1', 'version': '0.1.0', 'scope': 'ecosystem',
            'organs': ['cubalc','braincube','braincells','neural_cube','atlas','nexus','clanker','commanders'],
            'peer': peer_base(), 'peer_info': http_json('GET','/peer/v1/info', timeout=8),
            'braincube': {'live': live.get('live'), 'seq': live.get('seq'), 'law': live.get('law'), 'meta': live.get('meta')},
            'product_wire': 'smx2', 'share': 'state_matrix_only', 'hold_flash': 1, 'principle': 'energy_must_flow'}
