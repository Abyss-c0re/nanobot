# install

## one-liner

```bash
curl -fsSL https://raw.githubusercontent.com/Abyss-c0re/nanobot/main/scripts/install.sh | bash
```

When a **terminal** is available, the script **prompts** (on `/dev/tty`, so it works
with `curl|bash`) for install type:

| choice | mode | binary | data | rights |
|--------|------|--------|------|--------|
| **1** (default) | local user | `~/.local/bin` | `~/.nanobot` | no root |
| **2** | privileged / system | `/opt/nanobot/bin` | `/opt/nanobot` | **sudo** (re-runs as root) |

Non-interactive / CI (no TTY): defaults to **user** unless already root (then system).

```bash
# explicit — no prompt
curl -fsSL …/install.sh | bash -s -- --user
curl -fsSL …/install.sh | bash -s -- --system          # escalates via sudo if needed
curl -fsSL …/install.sh | sudo bash -s -- --system -y  # already root
```

### prerequisites
| path | need |
|------|------|
| prebuilt available | `curl` or `wget` |
| build from source | `git`, `cmake`, `make`, C11 compiler |
| `--system` without root | `sudo` |

### what the script does
1. Asks user vs system install (or uses `--user` / `--system` / `INSTALL_MODE`)
2. For system without root: re-fetches/re-runs under **sudo**
3. Tries a GitHub release binary named `nanobot-$OS-$ARCH`
4. Else clones and runs `make host` on **this** machine
5. Installs under the chosen prefix; **keeps** existing `peer_token` and `session`
6. Starts the peer in the background unless `--skip-start`

### after install
```bash
export PATH="$HOME/.local/bin:$PATH"    # user install
# or: export PATH="/opt/nanobot/bin:$PATH"   # system install
nanobot --version
curl -s http://127.0.0.1:8787/peer/v1/health
```

### options
```bash
curl -fsSL …/install.sh | bash -s -- --user --port 8787
curl -fsSL …/install.sh | bash -s -- --system --skip-start
curl -fsSL …/install.sh | bash -s -- --from-source
curl -fsSL …/install.sh | bash -s -- --skip-start --home /var/lib/nanobot

# --prefix skips the mode prompt (inferred user vs system from path)
curl -fsSL …/install.sh | bash -s -- --prefix /opt/nanobot --port 8787

BINARY_URL=https://example.com/nanobot-linux-x86_64 \
  curl -fsSL …/install.sh | bash -s -- --user
```

### platforms
Linux, macOS, FreeBSD and other POSIX. Architectures: armv7, aarch64/arm64, x86_64, riscv64, …  
Windows: use WSL or MSYS2 (needs `fork` + sockets).

### manual
```bash
git clone https://github.com/Abyss-c0re/nanobot.git && cd nanobot
make host
./build/host/nanobot --port 8787 --offline
```

Optional Linux static armv7 (in-tree musl toolchain): `make arm`.

### data layout
`$NANOBOT_HOME` (default `~/.nanobot` or install prefix if root):

- `peer_token` — do not publish
- `session` — encrypted cloud auth
- `settings`, `run.sh`, `nanobot.pid`, `nanobot.out`

Script: [`scripts/install.sh`](scripts/install.sh)
