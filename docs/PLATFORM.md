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
