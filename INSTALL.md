# install

## one-liner

```bash
curl -fsSL https://raw.githubusercontent.com/Abyss-c0re/nanobot/main/scripts/install.sh | bash
```

### prerequisites
| path | need |
|------|------|
| prebuilt available | `curl` or `wget` |
| build from source | `git`, `cmake`, `make`, C11 compiler |

### what the script does
1. Tries a GitHub release binary named `nanobot-$OS-$ARCH`
2. Else clones and runs `make host` on **this** machine
3. Installs to `/opt/nanobot` (root) or `~/.local` + data in `~/.nanobot`
4. **Keeps** existing `peer_token` and `session` on re-run
5. Starts the peer in the background unless `--skip-start`

### after install
```bash
export PATH="$HOME/.local/bin:$PATH"    # or /opt/nanobot/bin
nanobot --version
curl -s http://127.0.0.1:8787/peer/v1/health
```

### options
```bash
curl -fsSL https://raw.githubusercontent.com/Abyss-c0re/nanobot/main/scripts/install.sh | bash -s -- \
  --prefix /opt/nanobot --port 8787

curl -fsSL …/install.sh | bash -s -- --from-source
curl -fsSL …/install.sh | bash -s -- --skip-start --home /var/lib/nanobot

BINARY_URL=https://example.com/nanobot-linux-x86_64 \
  curl -fsSL …/install.sh | bash
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
