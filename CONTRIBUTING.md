# contributing

## what we want
- clear C, small diffs, working builds
- tests when you change crypto or auth
- docs that match the code

## how
```bash
make host && make test
# optional features: docs/BUILD.md
```

## secrets
Do not commit tokens, session files, or private keys.  
`.gitignore` already excludes typical paths under `$NANOBOT_HOME`.

## style
- Prefer boring, readable C11
- Prefer fixing root causes over papering over bugs
- No drive-by refactors unrelated to the change

## security reports
See SECURITY.md. Prefer private disclosure for real vulnerabilities.
