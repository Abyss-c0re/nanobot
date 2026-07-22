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

Install location is recorded in **`~/.nanobot/install.env`** (and a copy under `$NANOBOT_HOME`).

### Re-run (update / reinstall / uninstall)

If a prior install is detected, the script offers:

| choice | action | behaviour |
|--------|--------|-----------|
| **1** (default) | **Update** | replace binary, **keep** `peer_token` / session / settings |
| **2** | **Clean reinstall** | wipe data dir, install fresh |
| **3** | **Uninstall** | remove binary; ask whether to wipe data |
| **4** | Cancel | exit |

```bash
# non-interactive lifecycle
curl -fsSL …/install.sh | bash -s -- --update
curl -fsSL …/install.sh | bash -s -- --reinstall
curl -fsSL …/install.sh | bash -s -- --uninstall --keep-data
curl -fsSL …/install.sh | bash -s -- --uninstall --wipe-data
```

Non-interactive / CI (no TTY): fresh install defaults to **user** (or **system** if root);
re-run defaults to **update**.

```bash
# explicit first install — no prompt
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
1. Detects existing install via `~/.nanobot/install.env` (or known binary paths)
2. If present: update / clean reinstall / uninstall (or flags)
3. Else asks user vs system install (`--user` / `--system` / `INSTALL_MODE`)
4. For system without root: re-fetches/re-runs under **sudo**
5. Tries a GitHub release binary named `nanobot-$OS-$ARCH`
6. Else clones and runs `make host` on **this** machine
7. Writes registry + installs under the chosen prefix
8. **Update** keeps `peer_token` / session; **reinstall** wipes data
9. Starts the peer in the background unless `--skip-start`

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
`$NANOBOT_HOME` (default `~/.nanobot` or `/opt/nanobot` for system):

- `peer_token` — do not publish
- `session` — encrypted cloud auth
- `settings`, `run.sh`, `nanobot.pid`, `nanobot.out`
- `install.env` — install registry copy (prefix, bin, mode; **no secrets**)

Operator registry (always under the human user’s home, even after sudo):

- `~/.nanobot/install.env` — used to detect re-runs

Script: [`scripts/install.sh`](scripts/install.sh)
