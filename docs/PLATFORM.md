# platform

| build | output | notes |
|-------|--------|--------|
| `make host` | `build/host/nanobot` | native for **this** OS/CPU (Linux, macOS, *BSD, …) |
| `make arm` | `build/armv7/nanobot` | optional Linux static armv7 (in-tree musl toolchain) |

Needs a C11 compiler and cmake for source builds.  
No OEM, phone, or appliance product is required.

## Portability rule

The core binary and its in-tree docs/comments stay **product-agnostic**:

- No hard-coded OS product paths (`/system`, `/sdcard`, `/data/local/tmp`, …)
- No host brand names in user-facing strings or system prompts
- Host-specific roots, gate mirrors, and secret modes are **env/settings only**
  (see `env.example`: `NANOBOT_PERSONAL_ROOTS`, `NANOBOT_GATE_MIRROR`,
  `NANOBOT_SHARED_SECRETS`, `TMPDIR`)

Wrappers, appliance deploy scripts, and optional companion UIs may set those
variables. They must not reintroduce product paths into `src/`.

## smoke
```bash
./build/host/nanobot --version
./build/host/nanobot --offline --port 8787
curl -s http://127.0.0.1:8787/peer/v1/health
```
