# Security policy

## Reporting

If you find a vulnerability in **nanobot itself** (this repository’s code),
please report it privately to the maintainers if contact is available, or via
the host’s private vulnerability reporting feature. Do not open a public issue
with exploit details until a fix is available when practical.

## Out of scope

- Misconfiguration of third-party APIs or accounts
- Issues solely in llama.cpp, curl, OS, or remote model providers
- Social engineering against your browser login session

## Safe handling of secrets

nanobot may store OAuth/device tokens under `NANOBOT_HOME` (default `~/.nanobot`):

- Never commit `session`, `peer_token`, or `env` with keys
- Treat peer tokens as secrets if you enable them
- `@!` and tool shell run with the privileges of the nanobot process—do not
  expose the UI/peer ports to untrusted networks without authentication

## Hardening tips

- Bind only on localhost or private nets when possible
- Use peer token for `/peer/v1/*` if exposed on a LAN
- Prefer local backends when you do not need cloud features
