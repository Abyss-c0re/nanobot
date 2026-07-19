# Platforms

| Target | Triple / env | Binary | Notes |
|--------|----------------|--------|-------|
| **Host (lab PC)** | native `gcc` | `build/host/nanobot` | BlackCube / any Linux x86_64 |
| **Roborock / ** | `armv7l-linux-musleabihf` static | `build/armv7/nanobot` | musl.cc toolchain under `toolchain/` |
| **Future aarch64** | optional | `build/aarch64/` | `make aarch64` if cross-gcc present |

## Principles

- **Standalone:** one binary + system `curl`; no Titanus/PVE required for chat
- **Simple:** plain C, no heavy frameworks
- **llama.cpp-friendly** message shapes when bridging
- Secrets only under `$NANOBOT_HOME` (gitignored)

## Test matrix (minimal)

```bash
make host
./build/host/nanobot --version
./build/host/nanobot --help
# optional MCP smoke
printf '' | timeout 1 ./build/host/nanobot --mcp || true
```

Robot:

```bash
make arm
make install-robot   # scp to REMOTE_HOST
```

## Offline / llama.cpp outer level

The **outer** process (HTTP UI, shell tool, MCP, peer bus, session files) is reusable **without** Grok cloud:

| Layer | Online Grok | Offline llama.cpp |
|-------|-------------|-------------------|
| UI :8787 / CLI `-p` | yes | yes (same) |
| Shell / MCP tools | yes | yes |
| Model backend | cli-chat-proxy / api.x.ai | **local llama.cpp server** (OpenAI-compatible or raw completion) |
| Auth browser | required | **not** required |

Intent: keep nanobot **simple** — swap or add a backend flag, e.g.:

```bash
# future / design
nanobot --backend llama --llama-url http://127.0.0.1:8080/v1
nanobot --backend grok   # default (browser session)
```

Message shape stays **llama.cpp / OpenAI-chat compatible** (`messages[{role,content}]`) so offline and online share the same outer loop.
