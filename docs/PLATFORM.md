# platform

| build | output | notes |
|-------|--------|--------|
| `make host` | `build/host/nanobot` | native for **this** OS/CPU (Linux, macOS, *BSD, …) |
| `make arm` | `build/armv7/nanobot` | optional Linux static armv7 (in-tree musl toolchain) |

Needs a C11 compiler and cmake for source builds.  
No OEM or appliance product is required.

## smoke
```bash
./build/host/nanobot --version
./build/host/nanobot --offline --port 8787
curl -s http://127.0.0.1:8787/peer/v1/health
```
