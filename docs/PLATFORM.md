# Platform

| Build | Output | Notes |
|-------|--------|-------|
| **Host** | `build/host/nanobot` | Linux (and similar) with `gcc` + `curl` |
| **armv7 static** | `build/armv7/nanobot` | Optional embedded Linux via musl cross under `toolchain/` |

nanobot is a **standalone tool**. Architecture-specific builds do not imply
affiliation with any OEM, phone, or appliance product.

## Smoke

```bash
./build/host/nanobot --version
./build/host/nanobot --help
./build/host/nanobot --offline --port 8787
```
