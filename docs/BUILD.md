# build features

```bash
cmake -S . -B build/host -DCMAKE_BUILD_TYPE=Release \
  -DNANOBOT_ENABLE_MCP=ON \
  -DNANOBOT_ENABLE_AUTH=ON \
  -DNANOBOT_ENABLE_PEER=ON \
  -DNANOBOT_ENABLE_HUB=ON \
  -DNANOBOT_ENABLE_SHELL=ON \
  -DNANOBOT_ENABLE_PROVIDERS=ON
cmake --build build/host -j
```

| option | default | effect |
|--------|---------|--------|
| NANOBOT_ENABLE_MCP | ON | `--mcp` stdio server |
| NANOBOT_ENABLE_AUTH | ON | device-code + sealed session |
| NANOBOT_ENABLE_PEER | ON | HTTP peer/API |
| NANOBOT_ENABLE_HUB | ON | `--hub` OUT port |
| NANOBOT_ENABLE_SHELL | ON | shell / @! |
| NANOBOT_ENABLE_PROVIDERS | ON | Grok + OpenAI-compatible backends |

Slim example (offline shell peer only — still links full objects; CLI gates features):
```bash
cmake -S . -B build/slim -DNANOBOT_ENABLE_MCP=OFF -DNANOBOT_ENABLE_HUB=OFF
```

`make host` / `make arm` use defaults (all ON).

Auth is self-contained: `peer_token` under `NANOBOT_HOME`, `/activate`, `--login`.  
No vacuum/ product is required.
