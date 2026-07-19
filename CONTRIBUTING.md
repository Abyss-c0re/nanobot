# Contributing

Thanks for interest in nanobot.

## Ground rules

1. Keep the project **standalone** (not a product-specific robot/ROM tree).
2. Do not add proprietary blobs or third-party toolchains into git.
3. Do not commit secrets (`session`, tokens, API keys).
4. Preserve MIT license headers/spirit; no copyleft trap without discussion.
5. Respect [LEGAL.md](LEGAL.md): no claiming affiliation with xAI/Grok/etc.

## Dev loop

```bash
make host
make test
```

Regenerate embedded UI after editing `www/index.html` (see `scripts/embed_www.py`
if present, or the project’s embed step in docs).

## Pull requests

- Small, focused changes
- Update CHANGELOG.md under Unreleased or the next version
- Note any new network endpoints or auth behavior in README/LEGAL if relevant
