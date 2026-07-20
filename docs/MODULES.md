# nanobot modules

**Rule:** split and reuse. Intersecting code becomes a library.

## L0 (this PR)

| Library | Path | Role |
|---------|------|------|
| `nanobot_crypto` | `libs/crypto` | CSPRNG, ct_eq, wipe, hex |
| `nanobot_os` | `libs/os` | workdir, files, settings |
| `nanobot_json` | `libs/json` | minimal JSON helpers |

Public headers: `include/nanobot/*.h` (`nb_*` API).

## Temporary

| Target | Role |
|--------|------|
| `nanobot_legacy` | Remaining `src/*` domain code until split into peer/agent/mcp/providers |

## Hub (runtime shape)

See [HUB.md](HUB.md): **IN** (WRITE) + **OUT** (READ events) + workers. CLI streams final tokens live (`-p`).

## Planned

tokens, mcp, http_mini, oauth_device, providers/grok, providers/openai_compat, peer, shell, memory, agent, improve.

See session plan: weapon-grade modular nanobot.
