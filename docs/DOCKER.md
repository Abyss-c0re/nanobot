# Docker & deploy

## project targets (equal)

| target | command |
|--------|---------|
| local | `./scripts/deploy.sh local` |
| ssh | `./scripts/deploy.sh ssh --host user@host [--arch armv7]` |
| docker | `./Docker/wizard default` or `./scripts/deploy.sh docker …` |

## docker wizard

```bash
./Docker/wizard help
./Docker/wizard config init
./Docker/wizard default
./Docker/wizard clone https://github.com/org/repo.git --prompt '…'
```

Details: [Docker/README.md](../Docker/README.md).
