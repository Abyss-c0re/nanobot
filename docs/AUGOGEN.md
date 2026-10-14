# Augogen — next-step suggestion (Grok Build compatible)

Predicts the **next user line** after a turn. Tab-accept in Grok Build; here the
**mesh votes**, then **full auto** (default) applies and enqueues a prompt job.

`NANOBOT_AUGOGEN_AUTO=0` or settings `AUGOGEN_AUTO=0` restores vote-only
(no enqueue). Mesh vote `0` still vetoes. HOLD_FLASH=1. Suggestions that
name flash/wipe/cycle-stop are rejected.

## Wire

Grok Build ACP: `x.ai/suggestPrompt` `{ generation, sessionId?, model? }` → `{ suggestion, generation }`.

Nanobot dual-wire plate `nanobot.augogen.v1`:

```
POST /api/augogen
POST /peer/v1/augogen
{"action":"generate","transcript":"User: …\n\nAgent: …"}
{"action":"vote","role":"guide|oversee|mesh","vote":"1"}
{"action":"pending"}
{"action":"apply"}   # confirmed: expose next_prompt; auto mode also enqueues run
{"action":"auto"}    # generate + pair-confirm + apply + enqueue (same as generate when auto on)
{"action":"mode","auto":"1"}  # persist AUGOGEN_AUTO
{"action":"reject"}
{"action":"motive","text":"…","enqueue":"1"}  # hybrid rank; enqueue stores a forward line
```

## Motive

`action` `motive` (alias `rank`) answers `nanobot.motive.v1`.

The ranker holds eight concepts on the stack (192-byte norm, 24 tokens, no heap).
Score is token overlap plus a 64-bit sketch distance. A priority cascade names
the plate first. Plates `purge_intent` and `protect_flow` block storage and
auto-run. `hold_flash`, `forbid_live`, and `bake_rom` return a remedy line and
do not enqueue. Other plates are forward: with `enqueue` `1` the line is stored
as the pending suggestion. Full auto runs it only when the caller passed an
agent. Vote-only (`NANOBOT_AUGOGEN_AUTO=0`) stores the line and leaves it pending.

A peer sends that line to another peer with `POST /peer/v1/prompt`. The CubalC
board `programs/motive_rank.cubalc` is the same eight-step flow in the language.

`auto_execute` is **true** when full auto is on (default). `confirmed` is true
only when both pair votes are 1 and mesh did not veto (mesh `0`). Auto mode
then forks a `kind=prompt` job (`executed`, `job_id`). Vote-only never runs.

## Pair = one braincube

One nanobot **guides** (construct), the other **oversees** (deconstruct).
Together they are one BrainCube (`lhlam_cube_pair_confirm`). CubalC board:
`programs/hive_mind/augogen_confirm.cubalc`.

## CLI compat

`x-grok-client-version` floor is `1.0.52` (installed Grok Build). The public grok-build crate may lag that binary. Runtime still auto-bumps if the proxy says the client is outdated.

`x.ai/suggestPrompt` request and response stay `{ generation, sessionId?, model? }` → `{ suggestion?, generation }`. The transcript inside that chat call is packed to 24000 characters. Older turns cap at 1500 characters on a newline or whitespace. The newest agent reply is capped only by the remaining budget.
