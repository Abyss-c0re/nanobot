# Security

## Reporting
Report security issues privately to the repository owner (private repo).
Do not open public issues with exploit detail for private pre-release.

## Hardening notes
- Prefer LAN-only bind for the HTTP UI.
- Peer bus requires `X-Nanobot-Peer-Token`.
- Shell tool can run arbitrary commands as the process user — treat like a root shell if run as root.
- Device-code auth stores refresh material under `$NANOBOT_HOME` — protect that directory.
