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
the plate first. The JSON reports that plate and a 2–12 word line. A forward
plate's line is the winning concept. Every other plate reports a remedy and
is not stored. With `enqueue` `1` only a forward line is stored. The act text
itself is never stored. Full auto runs a stored line only when the caller
passed an agent. With no agent the line stays pending: pair votes are not
invented, and generate, vote, and apply do not fork a job. Vote-only
(`NANOBOT_AUGOGEN_AUTO=0`) leaves that line pending.
When `generate` has no model line, the same ranker reports a forward concept in the response, plus the plate name, `rank`, `lexical`, and `hamming`.
`enqueue` `1` stores that concept line and keeps the plate name, `rank`, `lexical`, and `hamming` on the plate. Generate does not POST. Without `enqueue` the plate stays empty and omits those fields.
`NANOBOT_MOTIVE_LEX_W` sets the lexical weight from 0 to 255. Unset stays 64, so an exact concept still ranks 256. A value outside that range stays 64. The weight does not open a purge or protect plate. The motive JSON and a generate report name that weight as `lex_w`. The plate file omits `lex_w`. A motive request may pass a quoted `lex_w` from 0 to 255 for that call only. A missing or bad value keeps the process weight. Generate keeps the process weight.
A blank transcript reports `prompt a peer to move` with plate `observe` and rank 0. A purge or protect transcript stays empty. The act text is not stored.

When a motive store happens and the request has no `hop`, one optional IPv4 peer
receives the stored line. Set `NANOBOT_MOTIVE_PEER` (or settings `MOTIVE_PEER`)
to `http://A.B.C.D:port`. The token is `NANOBOT_MOTIVE_PEER_TOKEN` or settings
`MOTIVE_PEER_TOKEN`, or else an absolute path in `NANOBOT_MOTIVE_PEER_TOKEN_FILE`
or settings `MOTIVE_PEER_TOKEN_FILE`. That file must be a regular owner-only
file of at most 80 bytes. A leading `token=` is stripped. A group or other
permission bit skips the file. The POST is `/peer/v1/augogen` with `enqueue`
`1` and `hop` `1`, and the body text is the stored line. When that motive
request quoted `lex_w`, the POST quotes the same value. A missing or bad
value omits `lex_w`, so the peer keeps its process weight. The POST also
quotes the ranked plate name, `lexical`, `hamming`, and `concept`. A quoted plate,
`lexical`, `hamming`, or `concept` that disagrees with the peer rank is not stored.
A missing value keeps the store. A request that
already carries `hop` `1` stores locally and does not POST. An empty peer
does not POST. There is no second peer and no retry. The act text is not the body.
The CubalC boards are `programs/motive_rank.cubalc`, `programs/motive_hop.cubalc`, `programs/motive_silence.cubalc`, `programs/motive_plate.cubalc`, `programs/motive_score.cubalc`, `programs/motive_weight.cubalc`, `programs/motive_lex.cubalc`, `programs/motive_hop_lex.cubalc`, `programs/motive_hop_plate.cubalc`, `programs/motive_hop_score.cubalc`, `programs/motive_hop_concept.cubalc`, and `programs/motive_agent.cubalc`.

`auto_execute` is **true** when full auto is on (default). `confirmed` is true
only when both pair votes are 1 and mesh did not veto (mesh `0`). Auto mode
then forks a `kind=prompt` job (`executed`, `job_id`) when an agent is present.
No agent leaves the plate pending and unexecuted. Vote-only never runs.

## Pair = one braincube

One nanobot **guides** (construct), the other **oversees** (deconstruct).
Together they are one BrainCube (`lhlam_cube_pair_confirm`). CubalC board:
`programs/hive_mind/augogen_confirm.cubalc`.

## CLI compat

`x-grok-client-version` floor is `1.0.52` (installed Grok Build). The public grok-build crate may lag that binary. Runtime still auto-bumps if the proxy says the client is outdated.

`x.ai/suggestPrompt` request and response stay `{ generation, sessionId?, model? }` → `{ suggestion?, generation }`. The transcript inside that chat call is packed to 24000 characters. Older turns cap at 1500 characters on a newline or whitespace. The newest agent reply is capped only by the remaining budget.
