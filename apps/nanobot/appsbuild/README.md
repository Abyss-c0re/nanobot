# appsbuild seed — nanobot app

**Follow-up seed** after robot deploy baseline (Clanker lab, 2026-07-22).

## Deploy (robot)

```bash
export NANOBOT_REMOTE_HOST=192.168.8.209
export NANOBOT_REMOTE_DIR=/mnt/data/nanobot
export REMOTE_ARCH=armv7
# optional: NANOBOT_REMOTE_BIN=build/armv7/nanobot
./scripts/deploy_binary_safe.sh
```

Wrapper co-deploy:

```bash
export CLANKER_HOST=192.168.8.209
../nanobot-wrapper/scripts/install_to_robot.sh
```

## Policy on tiny board

- One `nanobot` listener on **:8787**
- **Subagents allowed but tiny:** max **2**, `LLM_SERIAL=1`, short prompt/reply, 45s timeout
- Never wipe `peer_token` / `session`
- See `clanker/docs/PROCESS_STACK.md`

## Build on this seed

Read `SEED.json` → implement `follow_up` items in priority order without expanding process count on-device.
