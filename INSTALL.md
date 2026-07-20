# install — any machine that can run C

nanobot is a **standalone** C agent (POSIX). Not tied to robots, , or a single arch.

## one-liner

```bash
curl -fsSL https://raw.githubusercontent.com/Abyss-c0re/nanobot/main/scripts/install.sh | bash
```

Works on **Linux / macOS / *BSD** (and other POSIX).  
Architectures: **armv7, aarch64/arm64, x86_64, riscv64, …** (whatever `cc` targets).

## flow

1. Try prebuilt release asset `nanobot-$OS-$ARCH` (optional).
2. Else **build on this host** (`git` + `cmake` + `make` + `cc`/`gcc`/`clang`).
3. Install binary; create data dir; **never delete** existing `peer_token` / `session`.

## options

```bash
curl -fsSL …/install.sh | bash -s -- --prefix /opt/nanobot --port 8787
curl -fsSL …/install.sh | bash -s -- --from-source
curl -fsSL …/install.sh | bash -s -- --skip-start --home /var/lib/nanobot

# pin a binary you built/copied
BINARY_URL=https://example/nanobot-darwin-aarch64 \
  curl -fsSL …/install.sh | bash
```

## manual (developers)

```bash
git clone https://github.com/Abyss-c0re/nanobot.git && cd nanobot
make host          # native for THIS machine
./build/host/nanobot --port 8787 --offline
```

Optional Linux static armv7 (needs in-tree musl toolchain): `make arm`.

## after install

| | |
|--|--|
| binary | `$PREFIX/bin/nanobot` |
| data | `NANOBOT_HOME` |
| health | `http://127.0.0.1:8787/peer/v1/health` |
| cloud login | `http://HOST:8787/activate` or `nanobot --login` |
| local LLM | `nanobot --offline` / `--base-url` |

## Windows

Use **WSL**, **MSYS2**, or another POSIX environment. Plain Win32 is not the primary target (process model uses `fork` + BSD sockets).

## release assets (optional)

Publish as:

```text
nanobot-linux-armv7
nanobot-linux-aarch64
nanobot-linux-x86_64
nanobot-darwin-aarch64
nanobot-darwin-x86_64
nanobot-freebsd-amd64
…
```

Script: [`scripts/install.sh`](scripts/install.sh)
