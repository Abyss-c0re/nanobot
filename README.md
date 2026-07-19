# nanobot

**Tiny, standalone Grok agent** for constrained devices and the lab desktop.

| | |
|--|--|
| **Language** | C (static-friendly) |
| **Size** | ~80KB host / stripped armv7 |
| **Auth** | Browser device-code only (no API keys in the binary) |
| **Status** | **Unpublished** private project — not a public product release |

Same *idea* as Grok Build headless (HTTPS chat proxy + tool loop), **not** the full TUI.


## Disclaimer

**Not affiliated** with Grok, xAI, SpaceX, SpaceXAI, or any related company or product.
This is an independent hobby/lab tool that uses publicly available browser/device auth and HTTP APIs at the user's own risk. Names of third parties are used only for interoperability description.

## Features

- One-line **HTTP UI** (`:8787`) for hardware keyboards
- **Shell tool** on the machine that runs nanobot
- **MCP** stdio server (`nanobot --mcp`)
- **Peer bus** for lab agents (optional LAN)
- **Multiplatform builds:** host Linux + static **armv7** (Roborock / )

## Quick start (this machine)

```bash
cd ~/Dev/nanobot
make host
./build/host/nanobot --version
./build/host/nanobot --port 8787
# open http://127.0.0.1:8787/activate  then UI at http://127.0.0.1:8787/
```

One-shot (needs existing session):

```bash
./build/host/nanobot -p 'uname -a'
```

## Robot ( / Roborock)

```bash
make arm                 # needs toolchain/armv7l-linux-musleabihf-cross
make install-robot       # scp → root@192.168.1.88
# on robot:
export PATH=/mnt/data/nanobot/bin:$PATH
nanobot
```

## Build targets

```bash
make host        # native (lab PC) — always
make arm         # static armv7 musl
make all         # host + arm if toolchain present
make clean
make test        # smoke: version + help
```

See [docs/PLATFORM.md](docs/PLATFORM.md).

## Auth

RFC 8628 device-code via `https://auth.x.ai` (same family as `grok login --device-auth`):

```bash
nanobot --login
# open printed /activate URL on a browser already logged into Grok
```

Session: `$NANOBOT_HOME/session` (default under cwd or install home). **Never commit** session or peer tokens.

## UI shortcuts

| Input | Action |
|-------|--------|
| text + Enter | Agent turn |
| `/s` | Status |
| `/log` | Tail log |
| `/clear` | Clear transcript |

## MCP

```toml
[mcp_servers.nanobot]
command = "/path/to/build/host/nanobot"
args = ["--mcp"]
```

## Layout

```
src/           C sources
www/           UI (embedded at build)
build/host/    native binary
build/armv7/   robot binary
scripts/       install + bridges
docs/          platform, peer bus
toolchain/     arm cross (gitignored)
```

## Safety

Blocks crude destructive patterns (`rm -rf /`, `mkfs`, …). On a rooted vacuum you are still root — prefer allowlisted ops.


## Offline outer level (llama.cpp)

The **outer agent** (UI, shell tool, MCP, peer) is designed to be **reusable offline**:

- Default backend: Grok (browser session)
- Planned/parallel: **llama.cpp** local server — same message envelope, no xAI auth

See [docs/PLATFORM.md](docs/PLATFORM.md) § Offline.

## Version control

This tree is meant as a **private / unpublished** GitHub repo (or local-only git):

```bash
git status
# optional private remote:
# gh repo create nanobot --private --source=. --remote=origin --push
```

Do **not** publish session files, peer tokens, or the arm toolchain tarball.

## License

MIT — see [LICENSE](LICENSE).
