# Platforms

| Target | Binary | Notes |
|--------|--------|-------|
| **Host** | `build/host/nanobot` | Linux (and similar) with `gcc` + `curl` |
| **armv7 static** | `build/armv7/nanobot` | Optional embedded Linux via musl cross under `toolchain/` |
| **aarch64** | optional | Add a cross recipe if needed |

nanobot is a **standalone tool**. Architecture-specific builds do not imply any product affiliation (robot, vacuum, phone, etc.).

## Principles

- One binary + system `curl`
- Plain C, no heavy frameworks
- Pluggable LLM backend (Grok or OpenAI-compatible / llama.cpp)
- Secrets only under `$NANOBOT_HOME` (gitignored)

## Test

```bash
make host
./build/host/nanobot --version
./build/host/nanobot --help
./build/host/nanobot --offline --port 8787
```

## Optional remote install

```bash
export NANOBOT_REMOTE_HOST=your.device.ip
make arm
make install-remote
```
