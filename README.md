# nanobot

Standalone C agent host: **CLI**, **peer HTTP**, **optional MCP**.  
No vacuum /  / rockctl dependency. Auth is self-contained.

```
NANOBOT_HOME/          # default ~/.nanobot
  peer_token           # LAN secret (auto-create; do not rotate casually)
  session              # sealed provider tokens (nbenc1)
  settings             # PORT SHELL UI WWW …
```

## install (any machine)

```bash
curl -fsSL https://raw.githubusercontent.com/Abyss-c0re/nanobot/main/scripts/install.sh | bash
```

Linux, macOS, *BSD — armv7 / arm64 / x86_64 / riscv64 / …. See [INSTALL.md](INSTALL.md).

## build
```bash
make host              # all features ON
make arm
make test
# features: docs/BUILD.md  (-DNANOBOT_ENABLE_MCP=OFF …)
```

## run (auth without any other product)
```bash
./build/host/nanobot --port 8787 --offline          # local llama/OpenAI-compat
./build/host/nanobot --port 8787                    # peer up; cloud needs login
./build/host/nanobot --login                        # device-code → /activate
# open http://HOST:8787/activate  or use printed user_code URL
./build/host/nanobot --mcp                          # stdio MCP
./build/host/nanobot -p 'hello'                     # stream
```

## peer
| route | auth |
|-------|------|
| GET /peer/v1/health /info | open |
| GET /activate | open (device login UI trigger) |
| POST /peer/v1/shell prompt jobs control | `X-Nanobot-Peer-Token` |

## MCP bridge (any host)
```bash
export NANOBOT_PEER_URL=http://127.0.0.1:8787
# token: ~/.nanobot/peer_token or NANOBOT_PEER_TOKEN
python3 scripts/peer_mcp_bridge.py   # tools nanobot_*
```

## safe remote binary update
```bash
export NANOBOT_REMOTE_HOST=…   # or NANOBOT_REMOTE_HOST for remote host
make arm && ./scripts/deploy_binary_safe.sh
# never deletes peer_token/session
```

## docs
BUILD.md · PEER_BUS.md · HUB.md · SECURITY.md · AUDIT_20260720.md
