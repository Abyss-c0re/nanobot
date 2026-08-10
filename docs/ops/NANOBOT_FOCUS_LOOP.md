# Nanobot focus loop (BlackCube host)

**Role:** Watch local nanobots · restore focus · fix nanobot codebase · push  
**Cadence:** scheduled host loop (see Grok scheduler)  
**Wire:** SMX2 · HOLD_FLASH for Titan firmware · peer bus is lab_ops  

## Standing intent (re-assert if lost)

1. **Mesh assist Titan Atlas** — via **nanobot peer only** (no host-Grok adb thrash for report sync).
2. **BrainCube / PEER_LAN** — lattice peer URL, signature-safe cube_contact install path.
3. **Product quality** — peer HTTP, shell dual-wire, auth, no zombie thrash.
4. **Do not** flash Titan ROM or kill Atlas session without human gate.

## Focus sources (read each cycle)

| Source | Path |
|--------|------|
| Cube inbox | `~/.nanobot/reports/titan-loop/NEXT_FOR_BLACKCUBE.md` |
| Titan pack | `~/.nanobot/reports/titan-loop/pulls/` + CROSS_PULSE |
| Memory | `~/.nanobot/memory/summary.txt` · `core.txt` |
| Jobs | `~/.nanobot/jobs/` recent (GC keeps ≤48 finished) |
| Log | `~/.nanobot/nanobot.log` tail |
| Status | `~/.nanobot/reports/titan-loop/NANOBOT_LOOP_LAST.md` |
| Repo | `Dev/AI/nanobot` main |

## Peer health probes

```bash
curl -sS http://127.0.0.1:18787/health          # alias OK; jobs + jobs_keep
curl -sS http://127.0.0.1:18787/peer/v1/health  # pid + started + jobs (listener)
curl -sS http://127.0.0.1:18787/peer/v1/info    # same + signed_in
curl -sS http://127.0.0.1:18790/peer/v1/health  # HTTP MCP proxy
```

Settings `PORT=18787` often overrides CLI `--port 8787` when default.

## Cool restart (load new binary / clear deleted-bin)

```bash
cd ~/Dev/AI/nanobot
./scripts/cool_restart_peer.sh
```

- SIGTERM first (needs live stop-flag); reaps same-home orphans  
- Health-probes; rotates `peer-restart.log` past 512KiB  
- Double-start exits `already_listening` (rc=0) — no auth spam  

## Lost focus = when

- No peer shell/prompt activity >2 cycles while Titan pulses need replies  
- Memory/summary drifts to unrelated chit-chat only  
- Defunct nanobot / peer bind fail / signed_in false  
- NEXT_FOR_BLACKCUBE stale >30m with open seq  

**Action:** re-write `~/.nanobot/FOCUS.md` + one `nanobot_prompt` re-arm with standing intent (max 1/cycle).

## Code improve cycle

1. Collect issues from peer log + titan CROSS_PULSE + CHANGELOG backlog  
2. One bite-size fix in `Dev/AI/nanobot` (or scripts under titanus2 that are nanobot host bridges)  
3. Test smoke (compile / health curl / unit if any)  
4. Commit with clear message; **push origin main** when green  
5. Note in `~/.nanobot/reports/titan-loop/NANOBOT_LOOP.md`  
6. Overwrite `NANOBOT_LOOP_LAST.md` (UTC, peer ok, push sha, next)

## Product residuals already landed (2026-08-09)

| Fix | SHA / note |
|-----|------------|
| Zombie reap + SIGTERM stop flag | `b3ffd3b` · `53583af` |
| Shell no-reboot prose not 425 | `f56abfd` |
| `/health` `/ready` aliases | `abdefc0` |
| Health/info `pid`+`started` | `e5f08f0` · `cd1abc1` |
| GET `/peer/v1/jobs` index | `2cbc733` |
| Jobs GC keep 48 | `583bb38` |
| `already_listening` + orphan reap | `42824b8` |
| Mesh dir ensure on start | `f55a123` |
| Jobs GC after done + poll-by-id | `f699675` |
| already_listening connect-probe (not TIME_WAIT) | `f699675` |
| cool_restart retry if start pid exits pre-health | `eb51ed9` |
| peer_mcp_bridge settings PORT fallback | `3a49722` |
| peer_mcp_bridge prefer loopback if listening | `a4614d1` |
| GET /jobs list cap = NG_JOBS_KEEP (was 32) | `692ffd6` |
| Authorization: Bearer peer auth | `2006408` |
| MCP bridges dual-send Bearer + X-header | `b086bef` |
| hub events.jsonl rotate at 256KiB | `99660fa` |
| health/info jobs + jobs_keep leaves | `df0a8d4` |
| cool_restart installs newer build/host bin | `046a943` |
| `/ready` action=ready (was health) | `66d1217` |
| `/peer/v1/ready` alias (was 404) | `9a7f613` |
| jobs_keep on GET /jobs index | `f242007` |
| root discovery health/ready | `b26920b` |
| `/api/health` + `/api/ready` aliases | `5fe5633` |
| empty/whitespace peer shell → missing_command | `875f3b7` |
| MCP ready/health aliases on :18790 | `82a1c28` |
| cool_restart restages newer MCP | `b61c39b` |
| session_ensure on /peer/v1/info signed_in | `de7d51c` |
| empty prompt 400 + ensure on peer prompt | `3b0e259` |
| async jobs whitespace → need_prompt_or_command | `3127a38` |
| /api/chat + subagent whitespace + chat session_ensure | `ec66e08` |
| async prompt jobs session_ensure + need_login | `60642f4` |
| MCP empty/ws prompt+shell fail-fast | `3d47992` |
| subagent spawn session_ensure + need_login | `e06e2ad` |
| control blank/unknown action → need_service_action | `4c2a04f` |
| job_queued dual-wire kind leaf | `22f24f1` |
| peer_mcp_bridge control/job_status empty fail-fast | `ac50990` |
| empty watcher async jobs allowed | `579fc86` |
| async jobs unknown kind → unknown_kind | `9ac1baa` |
| orphan running/queued jobs → error on listen | `29d5e7b`  |
| async job kind whitespace strip | `415266b`  |
| MCP start_job watcher empty + kind/service fail-fast | `4498181`  |
| shell job/sync error token leaf (shell_disabled) | `a79295f`  |
| jobs index error leaf + fork_failed on queue | `896349c`  |
| async shell_disabled fail-fast 403 at queue | `99691c3`  |
| sync shell_disabled HTTP 403 align | `0c8a50a`  |
| async job kind case-fold (Shell→shell) | `4ed64ef`  |
| control service/action case-fold | `ffeab17`  |
| jobs index exit leaf | `fb48ea9`  |
| jobs index ok leaf (+ exit/error unified) | `71b2c0c`  |
| jobs path trailing slash + /api/jobs aliases | `7f1fabe`  |
| peer path /api + slash aliases (info/prompt/shell/control) | `bbbcd78`  |
| MCP :18790 info/jobs/health path aliases + slash | `79ef715`  |
| peer_mcp_bridge start_job running dual-wire plate | `2a7f98e`  |
| task/models trailing slash aliases | `eeabe10`  |
| MCP :18790 control/task/models proxy | `95c2c75`  |
| models plate action leaf | `d09d6e8`  |
| peer error plates action=error | `1ab6206`  |
| MCP 404 + bridge _missing action=error dual-wire | `2d2ae61`  |
| MCP unknown_tool dual-wire + isError true | `03174d0`  |
| MCP bad_id + braincube unknown_tool dual-wire | `bc975cb`  |
| MCP peer transport fail dual-wire (isError) | `71ee0be`  |
| MCP JSON-RPC parse/invalid body errors | `1e3e503`  |
| MCP GET subagents proxy on :18790 | `1fc99f3`  |
| MCP GET braincube + /live proxy on :18790 | `1944417`  |
| async shell job timeout 120s + pgroup kill | `70d7262`  |
| MCP GET resources proxy on :18790 | `cefa6e2`  |
| peer trailing-slash resources/braincube/health | `5159c69`  |
| MCP auth status proxy + peer auth aliases | `8854837`  |
| MCP /activate dual-wire + peer slash | `9d30a9e`  |
| GET settings plate + MCP proxy | `875fff5`  |
| GET version dual-wire + MCP proxy | `a60d122`  |
| GET mcp/servers slash + MCP proxy | `7f3dbbc`  |
| GET log dual-wire + MCP proxy | `cf08e49`  |
| GET bare /hello info alias + MCP | `831fc00`  |
| GET bare /settings alias + MCP | `28ee7d7`  |
| GET ping dual-wire + MCP | `d223473`  |
| GET backend dual-wire + MCP | `8e6bec9`  |
| GET api/peer namespace index + MCP | `81b80cd`  |
| GET whoami dual-wire + MCP | `f4d09f7`  |
| GET metrics dual-wire + MCP | `6f6c52d`  |
| GET openapi + favicon dual-wire + MCP | `eb401a6`  |
| GET capabilities dual-wire + MCP | `5e6aad6`  |
| GET swagger/docs openapi aliases + MCP | `07f7dc7`  |
| GET livez/readyz/healthz probe aliases + MCP | `65ea4f6`  |
| GET uptime dual-wire + MCP | `f1e5201`  |
| GET bare /status auth alias + MCP | `af20287`  |
| GET robots.txt lab-ops disallow + MCP | `bc36702`  |
| GET security.txt RFC 9116 + MCP | `a86d705`  |
| GET web app manifest dual-wire + MCP | `737d6c6`  |
| GET schema dual-wire + MCP | `27e3256`  |
| GET humans.txt + empty sitemap dual-wire + MCP | `16529dc`  |
| GET llms.txt dual-wire + MCP | `789e029`  |
| GET favicon.svg dual-wire + MCP | `7f505bb`  |
| GET service-worker.js no-PWA dual-wire + MCP | `65dcb8d`  |
| GET ads.txt/app-ads.txt no-sellers dual-wire + MCP | `0832f96`  |
| GET crossdomain.xml deny-all dual-wire + MCP | `28fa002`  |
| GET browserconfig.xml dual-wire + MCP | `475349f`  |
| GET well-known change-password dual-wire + MCP | `3871489`  |
| GET sellers.json empty dual-wire + MCP | `7ea4893`  |
| GET apple-touch-icon dual-wire + MCP | `dedf9cd`  |
| GET well-known ai-plugin.json dual-wire + MCP | `2476103`  |
| GET well-known assetlinks.json dual-wire + MCP | `20d9ea3`  |

| GET well-known apple-app-site-association dual-wire + MCP | `0a9d338`  |

| GET well-known gpc.json dual-wire + MCP | `c4e945f`  |
| GET well-known openid-configuration dual-wire + MCP | `966a75a`  |
| GET well-known oauth-authorization-server dual-wire + MCP | `8b46d0e`  |
| GET well-known oauth-protected-resource dual-wire + MCP | `ca2d05d`  |
| GET well-known dnt-policy.txt dual-wire + MCP | `f706d8d`  |
| GET well-known passkey-endpoints dual-wire + MCP | `2a18d8b`  |
| GET well-known webfinger dual-wire + MCP | `2b2ba80`  |
| GET well-known nodeinfo dual-wire + MCP | `78422ec`  |
| GET well-known host-meta dual-wire + MCP | `b51d99d`  |
| GET well-known matrix/{client,server} dual-wire + MCP | `4fdb149`  |
| GET well-known tdmrep.json dual-wire + MCP | `bf41126`  |
| GET well-known mta-sts.txt dual-wire + MCP | `d3a0587`  |
| GET well-known caldav|carddav dual-wire + MCP | `8712af6`  |
| GET well-known api-catalog dual-wire + MCP | `1944cf5`  |
| GET well-known agent-card.json dual-wire + MCP | `1d52f3f`  |
| GET well-known oauth-client-registration dual-wire + MCP | `d08e716`  |
| GET well-known openid-federation dual-wire + MCP | `4286247`  |
| GET well-known uma2-configuration dual-wire + MCP | `2db6635`  |
| GET well-known openid-credential-issuer dual-wire + MCP | `0d41e20`  |
| GET well-known fido2-configuration dual-wire + MCP | `48c0bdd`  |
| GET well-known webauthn dual-wire + MCP | `7a72967`  |
| GET well-known did.json dual-wire + MCP | `b976d6c`  |
| GET well-known did-configuration dual-wire + MCP | `d2efacd`  |
| GET well-known trust.txt dual-wire + MCP | `c9cecd8`  |
| GET well-known keybase.txt dual-wire + MCP | `3d48788`  |
| GET well-known pgp-key.txt dual-wire + MCP | `14e4a42`  |
| GET well-known openpgpkey dual-wire + MCP | `028b6af`  |
| GET well-known sshfp dual-wire + MCP | `90a2ad4`  |
| GET well-known jwks.json dual-wire + MCP | `7dd1a70`  |
| GET well-known related-website-set.json dual-wire + MCP | `9f0fc00`  |
| GET well-known microsoft-identity-association dual-wire + MCP | `d3f7ba2`  |
| GET well-known apple-merchantid-domain-association dual-wire + MCP | `b51df6d`  |
| GET well-known nostr.json dual-wire + MCP | `6217807`  |
| GET well-known atproto-did dual-wire + MCP | `8c9587d`  |
| GET well-known stellar.toml dual-wire + MCP | `c108cf0`  |
| GET well-known web-identity dual-wire + MCP | `49a463f`  |
| GET well-known posh dual-wire + MCP | `c3743c3`  |
| GET well-known traffic-advice dual-wire + MCP | `1ed0cc9`  |
| GET well-known privacy-sandbox-attestations dual-wire + MCP | `083e7bc`  |
| GET well-known no-federation resource dual-wire + MCP | `d20e371`  |
| GET well-known Chrome DevTools appspecific dual-wire + MCP | `d2f763b`  |
| GET well-known http-opportunistic dual-wire + MCP | `13c4cd9`  |
| GET well-known core (CoRE RFC 6690) dual-wire + MCP | `4caedbe`  |
| GET well-known mercure dual-wire + MCP | `4fab3ee`  |
| GET well-known gnap-as-rs dual-wire + MCP | `184cf0b`  |
| GET well-known csaf provider-metadata dual-wire + MCP | `b5a6da0`  |
| GET well-known discord dual-wire + MCP | `bbf8543`  |
| GET well-known jmap dual-wire + MCP | `9c1f5b3`  |
| GET well-known stun-key dual-wire + MCP | `92b119d`  |
| GET well-known thread dual-wire + MCP | `ea79a01`  |
| GET well-known coap dual-wire + MCP | `77eb866`  |
| GET well-known time dual-wire + MCP | `6c436ff`  |
| GET well-known timezone dual-wire + MCP | `d530205`  |
| GET well-known est dual-wire + MCP | `3862bcf`  |
| GET well-known pki-validation dual-wire + MCP | `4fee239`  |
| GET well-known looking-glass dual-wire + MCP | `0214266`  |
| GET well-known genid dual-wire + MCP | `43acc00`  |
| GET well-known acme-challenge dual-wire + MCP | `5a49c46`  |
| GET well-known ni dual-wire + MCP | `3c8146a`  |
| GET well-known vapid dual-wire + MCP | `4fd8dd8`  |
| GET well-known hoba dual-wire + MCP | `4547aa0`  |
| GET well-known smime-aia dual-wire + MCP | `d034b4e`  |
| GET well-known browserid dual-wire + MCP | `52fad8e`  |
| GET well-known idp-proxy dual-wire + MCP | `7d18a45`  |
| GET well-known dnt dual-wire + MCP | `bf70d85`  |
| GET well-known funding-manifest-urls dual-wire + MCP | `6da0b78`  |
| GET well-known xrpc-server-did dual-wire + MCP | `d3f15cd`  |
| GET well-known mcp.json dual-wire + MCP | `b1765bb`  |
| GET well-known web-bot-auth dual-wire + MCP | `58866a4`  |
| GET well-known sbom dual-wire + MCP | `9e6519f`  |
| GET well-known privacy-pass dual-wire + MCP | `6247397`  |
| GET well-known ohttp-gateway dual-wire + MCP | `cbec8cf`  |
| GET well-known masque dual-wire + MCP | `cec63a5`  |
| GET well-known doh|dot dual-wire + MCP | `2162c0c`  |
| GET well-known bluesky dual-wire + MCP | `2418999`  |
## Anti-chaos

- One product bite per cycle  
- No dual flash / no Atlas force-stop  
- Prefer nanobot MCP for device side effects  
- Titan human-gated queue (tip1.9 / product_fold) → no re-arm spam  
