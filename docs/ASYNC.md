# Async runs

A run is one agent turn that keeps going after the HTTP request that started it has returned. The client watches code-change notes, tool calls, thinking, and tokens while the turn is in flight, and it can abort, pause, intercept, filter, or change limits at any later request.

The listener is fork-per-request. The request worker forks again for the agent, answers `202`, and exits. The grandchild is reparented and keeps running. Nothing about the run lives in the listener's memory. The control plane is a directory under `$NANOBOT_HOME/runs/<id>/`.

Three paths accept the same routes:

- `/api/runs`
- `/api/v1/runs`
- `/peer/v1/runs`

The `202` body copies the prefix the client used into `poll`, `events`, and `stream`.

Loopback may skip the peer token, the same way other `/api` routes do (`require_peer_auth` with loopback allowed). Any other bind needs `X-Nanobot-Peer-Token`.

## Files

Directory mode is `0700`. Ordinary files are written with `ng_write_file` (create/truncate, process umask).

| File | Role |
|------|------|
| `prompt.txt` | The user prompt. Required. Cap 256 KiB. |
| `control` | One word and a newline: `run`, `pause`, or `abort`. |
| `intercept.txt` | Next user line. Written to `intercept.txt.new`, then renamed. |
| `filter.json` | Output filter. Re-read when the bytes change. |
| `config.json` | Model, effort, token caps, compaction. Re-read every turn. |
| `events.jsonl` | Append-only event log. One JSON object per line. `O_APPEND` plus `flock`. |
| `meta.json` | `queued`, `running`, `done`, `aborted`, or `error`, plus pid and error token. |

The id charset is `^[A-Za-z0-9_-]{1,40}$`. `ng_run_create` builds `r<unix>-<pid>-<counter>`. The pid belongs to the process that created the run, so two forked request workers in the same second do not share a directory. If that directory already exists, create retries with the next counter, up to 16 times.

A late I/O failure after `mkdir` leaves the directory behind and returns NULL. The HTTP client sees `run_create_failed`. A directory with no `meta.json` shows up in the list as `run_not_found`.

## Quick start

```bash
d=$(mktemp -d)
./build/host/nanobot --home "$d" --port 39123 --offline
```

`--offline` selects the local OpenAI-compatible backend, so a direct shell prompt does not need a browser session. Shell is on unless `$NANOBOT_HOME` contains a `shell_enabled` file that turns it off.

```bash
BASE=http://127.0.0.1:39123
curl -sS -D- -H 'content-type: application/json' \
  -d '{"prompt":"@! echo hello-run"}' \
  "$BASE/api/runs"
```

`202` body:

```json
{
  "schema": "nanobot.run.v1",
  "ok": true,
  "action": "run_queued",
  "id": "r1792001606-51687-0",
  "status": "queued",
  "poll": "/api/runs/r1792001606-51687-0",
  "events": "/api/runs/r1792001606-51687-0/events",
  "stream": "/api/runs/r1792001606-51687-0/stream",
  "python": 0
}
```

Poll until `status` is `done`, `aborted`, or `error`:

```bash
curl -sS "$BASE/api/runs/RUN_ID"
curl -sS "$BASE/api/runs/RUN_ID/events"
curl -sS --max-time 25 "$BASE/api/runs/RUN_ID/stream"
```

`python` is always `0`. Run plates use schema `nanobot.run.v1`. Auth failures, method failures, and create-time validation use schema `nanobot.peer_http.v1` (`http_peer_err`).

Plate field `ok` means the status read succeeded. The run outcome is the `status` string. A finished error run is HTTP 200 with `"ok":true` and `"status":"error"`.

## Create

`POST /api/runs`, `POST /api/v1/runs`, or `POST /peer/v1/runs`.

```json
{
  "prompt": "@! echo hello-run",
  "message": "alias of prompt when prompt is absent",
  "model": "local-model",
  "reasoning_effort": "xHigh",
  "max_tokens": 1234,
  "context_tokens": 8000,
  "compact": {
    "max_refills": 4,
    "keep_tail": 3,
    "keep_head": 1,
    "shrink_at": 900,
    "preemptive": true
  },
  "filter": {
    "rules": [
      {"op": "redact", "match": "sekret", "with": "REDACTED"}
    ]
  }
}
```

`effort` is accepted as an alias of `reasoning_effort`. Flat keys `compact_max_refills`, `compact_keep_tail`, `compact_keep_head`, `compact_shrink_at`, `compact_preemptive`, and `compact_explicit` are accepted when there is no nested `compact` object. `preemptive` may be `true`, `false`, or an integer (`atoi("true")` is 0, so the boolean form is required for a true flag).

| Check | HTTP | `error` |
|-------|------|---------|
| Empty or whitespace prompt | 400 | `missing_prompt` |
| Prompt longer than 256 KiB | 400 | `prompt_too_long` |
| No agent on the listener | 503 | `no_agent` |
| Cloud backend, prompt is not `@!`, session missing | 401 | `need_login` (`need_login: true`) |
| Model longer than 95 bytes | 400 | `bad_model` |
| Model contains a quote, space, or newline | 400 | `bad_model` |
| Effort longer than 39 bytes, or `ng_effort_canon` rejects it | 400 | `bad_reasoning_effort` |
| `filter` present and not a JSON object, or the object will not parse | 400 | `bad_filter` |
| `ng_run_create` returns NULL | 400 | `run_create_failed` |
| `fork` fails | 503 | `fork_failed` (meta status `error`) |

An empty model field leaves the listener's model in place. `has_effort` with an empty effort clears `reasoning_effort`. `off`, `default`, and `none` canon to empty and set `has_effort`. `x-high` and `x_high` canon to `xhigh`. The stored effort is lowercase.

`@!` skips the browser-session gate on every backend. A local backend (`--offline`, `--llama`, or `ng_agent_set_local_backend`) also skips it. Any other prompt on the cloud backend calls `ng_session_ensure` when a session exists and is not yet valid and login is not pending, then requires `ng_session_valid`.

The child closes the client socket, calls `ng_run_execute`, frees the reply and the id, and `_exit(0)`. It does not call `ng_agent_save_env`. Model and effort changes stay inside that child. The listener's `ng_agent_cfg` is not freed in the request worker.

`GET` on the collection returns `ng_run_list_json` (at most 32 runs). `HEAD` is treated as GET. Any other method on the collection is `405` with `Allow: GET, HEAD, POST, OPTIONS`.

## Status

`GET /api/runs/<id>` and `HEAD` return:

```json
{
  "schema": "nanobot.run.v1",
  "ok": true,
  "action": "status",
  "id": "r1792001606-51764-0",
  "status": "done",
  "pid": 51765,
  "alive": false,
  "seq": 4,
  "error": "",
  "model": "local-model",
  "reasoning_effort": "xhigh",
  "max_tokens": 1234,
  "context_tokens": 8000,
  "compact_explicit": 1,
  "compact_max_refills": 4,
  "compact_keep_tail": 3,
  "compact_keep_head": 1,
  "compact_shrink_at": 900,
  "compact_preemptive": 1,
  "python": 0
}
```

`alive` is `kill(pid, 0) == 0`. That is true for a live worker, a zombie that has not been reaped, and a pid that the kernel has reused. `status` stays `running` until the worker writes `done`, `aborted`, or `error`. A dead pid does not rewrite the status to orphan.

Missing id: HTTP 404, `"error":"run_not_found"`. Illegal id: HTTP 400, `"error":"bad_id"`. Other methods: `405`, `Allow: GET, HEAD, OPTIONS`.

## Events

`GET /api/runs/<id>/events` and `GET /api/runs/<id>/events/<since>`.

`since` is a path suffix. The query string is stripped by `path_of` before routing, so `?since=` is not a cursor. `since` is exclusive: events with `seq` greater than `since` are returned. `since` omitted means the whole log (`-1` in the plate).

The HTTP handler asks for 200 events. `ng_run_events_json` clamps a non-positive limit to 200 and a larger limit to 500.

```json
{
  "schema": "nanobot.run.v1",
  "ok": true,
  "action": "events",
  "id": "RUN_ID",
  "since": 1,
  "events": [],
  "python": 0
}
```

A missing run is HTTP 404 from `run_reject_missing` before the log is read. `ng_run_events_json` by itself, given a legal id and no `events.jsonl`, returns an empty `events` array with `"ok":true`.

`fgets` uses an 8192-byte buffer. A line longer than that is split. The first piece still starts with `{` and is included, truncated. The continuation often does not start with `{` and is skipped. `ng_run_open` counts lines with a 1024-byte buffer; `seq` is near the start of the line, so a split line is still one event. Keep event text inside the 4000-byte cap and the escaped line usually fits.

## Stream

`GET /api/runs/<id>/stream` and `GET /api/runs/<id>/stream/<since>`.

Response headers:

```http
HTTP/1.1 200 OK
Content-Type: text/event-stream
Cache-Control: no-cache
Connection: close
Access-Control-Allow-Origin: *
```

There is no `Content-Length`. The first frame is:

```text
event: open
data: {"schema":"nanobot.run.v1","ok":true,"action":"stream_open","id":"RUN_ID","since":-1,"python":0}

```

Each later log line is `data: <json>\n\n` via `run_sse_line`. `ng_run_tail` polls every 50 ms, delivers lines with `seq` greater than the cursor, and returns when the run is `done`, `aborted`, or `error` and no new line was seen on that pass, or when 20000 ms have elapsed. The handler then writes:

```text
event: bye
data: {"schema":"nanobot.run.v1","ok":true,"action":"stream_window","python":0}

```

One HTTP worker stays pinned for up to 20 seconds. A client that wants a longer tail opens `/stream/<last_seq>` again.

`HEAD` on the stream path returns the status JSON plate and does not tail. Other methods: `405`, `Allow: GET, HEAD, OPTIONS`. A missing run is 404 before the tail starts.

## Abort, pause, resume

`POST /api/runs/<id>/abort`, `/pause`, `/resume`. The body is ignored. Success is the status plate, HTTP 200. Missing id is 404 `run_not_found`. Other methods: `405`, `Allow: POST, OPTIONS`.

| Action | `control` file |
|--------|----------------|
| abort | `abort\n` |
| pause | `pause\n` |
| resume | `run\n` |

The worker notices the file at the points in [Timing](#timing). The HTTP call returns as soon as the file is written. `status` may still say `running` in that response.

## Intercept

`POST /api/runs/<id>/intercept`

```json
{"text": "look at the parser instead"}
```

`prompt` is an alias of `text`. Empty or whitespace-only text is HTTP 400 `missing_text`. Text longer than 8000 bytes, or a missing run, is HTTP 400 `bad_intercept` when the write fails after the missing-run check has already passed; a missing run is 404 `run_not_found` from `run_reject_missing` first.

The line is stored until the worker calls `ng_run_take_intercept`, which reads the file and unlinks it. On a stream cut the partial assistant text is kept as an assistant message, the intercept is appended as a user message, and the turn loop continues. If `control` is still `pause` after the intercept is taken, the next turn waits.

Posting an intercept to a run that has already finished still writes `intercept.txt`. The worker has exited and will not read it.

## Filter

`POST /api/runs/<id>/filter`

Nested form:

```json
{"filter": {"rules": [{"op": "drop_kind", "kind": "thinking"}]}}
```

Raw form, when the body has no `filter` key:

```json
{"rules": [{"op": "redact", "match": "sekret", "with": "REDACTED"}]}
```

A `filter` key whose value is not an object (`{"filter":"nope"}`) is HTTP 400 `bad_filter`. A body `ng_stream_filter_parse` rejects, a body longer than 8000 bytes, and an empty body are the same token. Success returns the status plate.

Rules apply to event kinds `token`, `thinking`, `tool_result`, and `code_change`. They do not apply to `tool_call`, `status`, `compact`, `intercept`, `abort`, or `done`. The assistant transcript keeps the unfiltered text. While `g_live` is set, token chunks are emitted on the run and are not also passed to the chat `on_delta` callback.

`drop` discards the whole pending chunk once the needle completes, and clears the carry. `redact` replaces the needle and does not scan the replacement. `drop_kind` drops that kind and does not touch the carry. The first matching rule at a position wins. At most 16 rules. `match`, `with`, and `kind` are at most 128 bytes. A missing `with` on redact becomes `***`.

A needle split across chunks is held when it is a strict prefix of a drop or redact match. The carry buffer is 160 bytes, which holds a 127-byte prefix of a 128-byte needle. `ng_run_emit` of a non-streaming kind flushes that carry in the same call. `ng_run_flush` writes a held token prefix and a held thinking prefix as their own events at the end of a normal stream and after an intercept. Abort does not flush, so a partial needle stays held.

`filter.json` is compared to the bytes cached on the open run (`filter_raw`, 8192 bytes). A changed file is parsed again. A broken file falls back to a pass-through filter.

Create accepts the same filter object. `NULL`, `{}`, and `{"rules":[]}` pass every byte through.

## Config

`POST /api/runs/<id>/config` accepts the same limit fields as create, including nested `compact` and the flat `compact_*` keys. `ng_run_set_limits` replaces `config.json`. Omitted numeric fields are the cleared defaults (`max_tokens` 0, `context_tokens` 0, `compact_explicit` 0, refills 3, tail 2, head -1, shrink 1800, preemptive 0).

`ng_run_apply` copies that file onto the child at the start of each turn:

- A non-empty `model` replaces `c->model`. An empty model leaves the current pointer.
- `has_effort` 1 with an empty effort clears `reasoning_effort`. A non-empty effort goes through `ng_agent_set_reasoning_effort`. `has_effort` 0 leaves the current effort pointer.
- `max_tokens` and `context_tokens` are always copied. Negative values become 0. `max_tokens` above 128000 becomes 128000.
- Compact fields are always copied.

So a config post that omits `model` keeps the running model, and a config post that omits `max_tokens` sets the completion cap back to the prompt-length heuristic on the next turn.

`bad_model` and `bad_reasoning_effort` use the same checks as create. Any other `ng_run_set_limits` failure is HTTP 400 `bad_limits` (missing run is 404 first). `ng_run_set_limits` also rejects a model that contains a quote, space, or newline, and it rejects an effort `ng_effort_canon` rejects. `limits_json` stores the canonical effort.

## Limits and environment

`ng_agent_cfg_init` sets `compact_keep_head` -1, `compact_shrink_at` 1800, `compact_max_refills` 3, `compact_keep_tail` 2, and leaves `compact_explicit` 0. `ng_agent_cfg_free` frees `base_url`, `model`, and `reasoning_effort`, then zeroes the struct. It does not free `live`.

`ng_agent_load_env` reads these after the base URL, model, and effort. The env file is applied first. A non-empty process environment variable replaces it.

| Variable | Effect |
|----------|--------|
| `NANOBOT_MAX_TOKENS` | Completion cap. Clamped to 0..128000. 0 selects the prompt-length heuristic. |
| `NANOBOT_CONTEXT_TOKENS` | Preemptive compact budget. 0 disables it. Negative becomes 0. |
| `NANOBOT_COMPACT_REFILLS` | Sets `compact_explicit`. Clamped to 0..8. 0 disables another refill. |
| `NANOBOT_COMPACT_KEEP_TAIL` | Sets `compact_explicit`. Clamped to 1..8. If refills were not set and `compact_max_refills` is <= 0, refills become 3 so the tail is paired with a live compact. |

Per-run `config.json` is applied by `ng_run_apply` inside the child and does not rewrite `$NANOBOT_HOME/env`.

### Completion cap

`reply_max_tokens`:

- `max_tokens` > 0 is used, clamped to 128000.
- Otherwise 512 when the prompt is shorter than 48 bytes, 1024 when it is shorter than 200, and 2048 otherwise.
- Tool turns send `max_tokens` 768 unless `c->max_tokens` > 0, in which case they use `reply_max_tokens`.
- The final-answer body and the non-stream fallback both send `reply_max_tokens`.
- A live run with no tools sets `stream` true even when the HTTP chat caller did not ask for SSE.

### Compaction

`compact_limit`:

- `compact_explicit` 0 returns 3, including a zeroed `ng_agent_cfg` from `memset`.
- Explicit and `compact_max_refills` <= 0 returns 0. The first overflow emits `compact` / `stop` and returns the stop plate. No shrink runs.
- Otherwise the value is clamped to 1..8.

`compact_policy_fill` copies the explicit tail, head, and shrink fields. When the run is not explicit it uses max refills 3, tail 2, head -1, shrink 1800. When the limit is 0 it still fills `max_refills` 1 so a policy struct is well formed; the caller returns before compacting.

`maybe_preempt_compact` runs before each POST when `compact_preemptive` is set and `context_tokens` > 0. The estimate is `strlen(messages) / 4`. One successful shrink counts as a refill. If the estimate is over budget and the refill limit is already spent, the function emits `compact` text `stop` and the turn returns `compact_stop`. If `ng_messages_compact_ex` returns NULL, the POST still happens.

`try_compact_retry` runs when the provider body is a context overflow (`ng_resp_is_context_overflow`). It compacts, decrements the turn counter, and emits `compact` text `refill N`. Three historical refills in a row is the default. A shrink that cannot make the array shorter emits `stop` and the turn returns:

```json
{"schema":"nanobot.compact.v1","ok":false,"error":"compact_stop","refills":3,"python":0}
```

`refills` in that plate is clamped to 0..8.

`ng_messages_compact` calls `ng_messages_compact_ex` with refill as given, max refills 3, tail 2, head -1, shrink 1800. `compact_normalize` maps refill < 1 to 1, max refills <= 0 to 3 (else clamp 8), tail <= 0 to 2 (else clamp 8), shrink 0 to 1800, and passes `keep_head` through.

On refill R the tail kept is `keep_tail - (R - 1)`, at least 1. Head -1 drops the first message only when `refill >= max_refills`. Head 0 drops it on every refill. Head 1 keeps it on every refill. Messages that fall out of the ring are replaced by one user message whose content is a JSON string:

```json
{"schema":"nanobot.compact.v1","ok":true,"dropped":N}
```

When every object is already inside the kept set, objects other than the last one that are longer than `shrink_at` become:

```json
{"schema":"nanobot.compact.v1","ok":true,"truncated":1}
```

`shrink_at` < 0 skips that replacement. The result is returned only when it is shorter than the input. A lone prompt (`kept <= 0`) is left alone (NULL). Histories longer than the ring drop the middle.

## Event schema

```json
{
  "schema": "nanobot.run.v1",
  "seq": 1,
  "kind": "token",
  "name": "",
  "text": "hello",
  "ok": true,
  "python": 0
}
```

| `kind` | `name` | `text` |
|--------|--------|--------|
| `status` | empty | `running` when the worker starts |
| `thinking` | empty | Provider reasoning text, also when `NANOBOT_SHOW_THINKING` is unset. Chat SSE still hides it unless that variable is on. |
| `token` | empty | Assistant text, 240-byte pieces when a buffered reply is split by `live_emit_text` |
| `tool_call` | tool name | Arguments, or the shell command for `@!` |
| `tool_result` | tool name | Tool output. Filtered. |
| `code_change` | tool name | `tool=… args=… output=…`, args clipped to 240 bytes, output to 700. Filtered. |
| `compact` | empty | `refill N`, `preemptive N`, or `stop` |
| `intercept` | empty | The intercept line |
| `abort` | empty | `aborted` |
| `done` | empty | Empty. `done` is not filtered, so a summary cannot be hidden by a drop rule. |

`text` stored in the file is capped at 4000 bytes before JSON escaping. `seq` starts at 1 for the first appended line.

`ng_run_execute` classifies the agent reply after `ng_run_flush`:

- A reply that contains `"error":"aborted"` finishes `aborted`.
- Any other reply that contains `"ok":false` finishes `error`, with the `error` string from the plate or `agent_error`.
- A NULL reply finishes `error` / `no_reply`.
- Otherwise, if no token was streamed, the reply is split into token events. Then a `done` event is written and the meta status is `done`.

A model sentence that happens to contain those substrings is classified the same way.

### Code change heuristic

`ng_run_note_code` emits `code_change` when any of these hold:

- The tool name contains `edit`, `write`, `patch`, or `apply`.
- The args contain `"path"`, `"file"`, `"diff"`, ` > `, `sed -i`, `tee `, or `patch `.
- The output begins with `diff ` or contains a newline followed by `@@`, `+++ `, or `--- `.

This is a text heuristic, not a diff of the worktree. `@!` passes the shell plate from `ng_tool_result_body`, which JSON-escapes newlines, so a unified diff inside that plate often fails the newline marks. A command that contains `patch ` still matches on the args. The same helper runs for model tool calls, the cosplay tool path, and `@!`.

`@!` sets `g_live_streamed` after the shell events, so `ng_run_execute` does not also split the shell plate into token events. An early braincell return does not emit tokens itself; `ng_run_execute` chunks that reply when nothing was streamed.

## Timing

`ng_agent_live_cut` polls once. Pause (`2`) waits until the run is not paused. The returned codes the agent acts on are 1 (abort) and 3 (intercept). Poll order is abort, then intercept, then pause, then run.

Checked at:

- The start of `ng_agent_run_attachments`, via the cleanup attribute that restores `g_live` on every return. The saved pointer is the previous `g_live`. The function then sets `g_live` from `c->live`.
- The start of each turn, before tools, after `ng_run_apply`.
- After a non-stream provider body, before tool dispatch.
- Every SSE line inside `consume_sse_stream`.
- A direct `@!` prompt, before `ng_run_command`, and again after that command returns.

On an SSE cut the curl child is `SIGTERM`'d, then `waitpid` `WNOHANG` up to 25 times with 20 ms between, then `SIGKILL`. `g_stream_cut` is cleared at the start of `curl_post_json_stream`, so a stale cut does not affect the next request. A cut stream is not returned as the final answer. Abort returns:

```json
{"schema":"nanobot.run.v1","ok":false,"error":"aborted","python":0}
```

Intercept mid-stream flushes held tokens, keeps the partial assistant text, appends the intercept as a user message, clears the cut, and continues the loop.

Pause while an SSE line is being processed stops the reader inside `ng_run_poll` until resume or abort. Curl can block on a full pipe until the reader continues.

A shell command that has entered `ng_run_command` runs until it exits. Abort before that call returns the abort plate and does not start the command. Abort after the command has started is recorded when the command returns: the tool result is emitted, then the run finishes `aborted`. The same is true of a model `run_terminal_command` that is already inside the shell; the next turn boundary sees the control file.

`g_live_streamed` is cleared in `ng_run_execute` only. A nested `ng_agent_run` does not clear it, so a parent that already streamed tokens is not tokenized again at the end.

## Concurrency

```text
listener
  fork request worker          SIGCHLD -> SIG_DFL
    handle_client
      fork run worker
        close(client)
        ng_run_execute
        _exit(0)
      write 202
      return
  reap request workers
```

The listener's `SIGCHLD` reaps request workers. The run grandchild is reparented when the request worker exits. `_exit` does not signal the grandchild. `SIGPIPE` `SIG_IGN` is inherited from the listener.

Two control requests and the worker share the directory. Event appends take an exclusive `flock`. Control, intercept, filter, and config are whole-file replaces. Intercept uses a temp file plus `rename`.

In-memory filter carry and the open `ng_run` exist only in the worker. A second process opens its own `ng_run` to read status and does not see the worker's unflushed carry.

`ng_run_execute` sets `c->live`, runs the prompt, clears `c->live`, and closes the run. The cleanup attribute restores the previous `g_live` if a nested call returns.

## Error tokens

| Token | Where | HTTP |
|-------|--------|------|
| `missing_prompt` | create | 400 peer |
| `prompt_too_long` | create | 400 peer |
| `no_agent` | create | 503 peer |
| `need_login` | create, cloud session required | 401 peer |
| `bad_model` | create, config | 400 peer |
| `bad_reasoning_effort` | create, config | 400 peer |
| `bad_filter` | create, filter | 400 peer |
| `run_create_failed` | create | 400 peer |
| `fork_failed` | create, and meta `error` | 503 peer |
| `missing_text` | intercept | 400 peer |
| `bad_intercept` | intercept write failed | 400 peer |
| `bad_limits` | config write failed | 400 peer |
| `run_not_found` | status and `run_reject_missing` | 404 run plate |
| `bad_id` | status, events | 400 run plate |
| `oom` | plate allocation, path buffers | 500 run plate |
| `not_found` | unknown action after a real id | 404 peer |
| `method_not_allowed` | wrong verb | 405 peer |
| `need_prompt` | execute, empty `prompt.txt` | meta `error` |
| `meta_write` | execute, cannot mark running | meta `error` |
| `no_reply` | execute, agent returned NULL | meta `error` |
| `aborted` | execute | meta `aborted` |
| `compact_stop` | agent, refill budget spent | agent reply |

## Discovery

The listener root literal, the `/api` index, and the info endpoint list `/api/runs`, `/api/v1/runs`, and `/peer/v1/runs`. The collection is not in `path_is_post_only`, so `OPTIONS` uses the generic allow list and `PUT` falls through to `handle_runs`, which answers 405.

`http_response` reason text includes `202 Accepted`.

## Function reference

Public entry points are in `src/stream_filter.h`, `src/runctl.h`, `src/agent.h`, and `src/wire_compat.h`. The HTTP helpers below are `static` in `src/http.c`. They are the request path. Headers in this section match the declarations.

### `ng_stream_filter_parse`

```c
ng_stream_filter *ng_stream_filter_parse(const char *json);
```

File: `src/stream_filter.c`.

Allocates a filter. `NULL` json, `""`, `"{}"`, and a JSON object with no `"rules"` key return an empty pass-through filter. A non-object returns NULL (the allocation is freed). `"rules"` must be followed by `[`. Each object in the array may set `op` to `drop`, `redact`, or `drop_kind`, plus `match`, `with`, and `kind`. Strings longer than 128 bytes, unknown ops, and a `drop_kind` with no kind are skipped. Parsing stops at 16 kept rules. `hold` becomes the longest drop/redact needle minus one.

The caller frees the result with `ng_stream_filter_free`. `ng_run_create` and `ng_run_set_filter` parse once to validate, free that object, and store the raw JSON.

### `ng_stream_filter_free`

```c
void ng_stream_filter_free(ng_stream_filter *f);
```

Frees the struct. `NULL` is safe. Rules live inside the struct, so there is no second free.

### `ng_stream_filter_hold`

```c
size_t ng_stream_filter_hold(const ng_stream_filter *f);
```

Returns the longest drop/redact needle minus one, or 0 when `f` is NULL. The carry buffer must be at least this many bytes. 160 is enough for the 128-byte string cap. `ng_run` uses `RUN_CARRY` 160.

### `ng_stream_filter_push`

```c
int ng_stream_filter_push(ng_stream_filter *f, const char *kind, const char *text,
                          char *carry, size_t *carry_n, size_t carry_sz, char **out);
```

Pushes one chunk. A NULL or empty `kind` is treated as `token`. NULL `text` is empty. `drop_kind` returns 0 and does not read or write the carry.

Otherwise the carry prefix is concatenated with `text` and scanned. Return 1 and a malloc'd `*out` means pass. An empty `*out` means the chunk is held. Return 0 means a completed drop needle discarded the pending work and cleared the carry. Return -1 is an allocation failure. `*out` is NULL on 0 and -1.

The unmatched suffix is copied back into `carry` when it fits. A suffix that does not fit is emitted as literal text. Redact advances past the match. The replacement is not scanned. The first matching rule at an index wins.

`f` NULL is a pass-through that still honors the carry arguments.

### `ng_stream_filter_flush`

```c
int ng_stream_filter_flush(ng_stream_filter *f, const char *kind,
                           char *carry, size_t *carry_n, size_t carry_sz, char **out);
```

Same return codes as push. Flush is `push_common` with the flush flag set, which emits a held prefix instead of keeping it. `ng_run_flush` calls this for `token` and then for `thinking`, and appends an event only when the flushed text is non-empty.

### `ng_run_limits_clear`

```c
void ng_run_limits_clear(ng_run_limits *lim);
```

Zeroes the struct, then sets `compact_keep_head` -1, `compact_shrink_at` 1800, `compact_max_refills` 3, `compact_keep_tail` 2. `compact_explicit` stays 0. NULL `lim` returns immediately.

### `ng_run_create`

```c
char *ng_run_create(const char *prompt, const ng_run_limits *limits,
                    const char *filter_json);
```

Creates `$NANOBOT_HOME/runs/<id>/` and returns a malloc'd id. NULL on a NULL prompt, a whitespace-only prompt, a prompt longer than 256 KiB, a filter longer than 8000 bytes, a filter `ng_stream_filter_parse` rejects, an effort `ng_effort_canon` rejects, or an I/O error.

`limits` NULL is replaced with `ng_run_limits_clear` defaults. A caller-supplied struct is copied and then normalized: `compact_shrink_at` 0 becomes 1800, `compact_max_refills` 0 with `compact_explicit` 0 becomes 3, `compact_keep_tail` 0 becomes 2. Explicit 0 refills stay 0.

Writes `prompt.txt`, `control` (`run`), an empty `events.jsonl`, `config.json`, `filter.json` (`{"rules":[]}` when the argument is NULL or empty), and `meta.json` with status `queued` and pid 0.

The id is `r<unix>-<pid>-<counter>` and must pass `id_ok`.

### `ng_run_open`

```c
ng_run *ng_run_open(const char *id);
```

Allocates an `ng_run`, checks the directory with `stat`, loads the filter, and sets `seq` to the number of event lines that start with `{`. NULL when the id is illegal, the directory is missing, or allocation fails. A missing `events.jsonl` still returns a run with `seq` 0.

The open count uses a 1024-byte `fgets`. `seq` is at the front of the line, so a wrapped line still counts once.

### `ng_run_close`

```c
void ng_run_close(ng_run *run);
```

Frees the filter and the struct. NULL is safe. It does not delete the directory and it does not flush the carry. Call `ng_run_flush` first when held tokens should be visible.

### `ng_run_id`

```c
const char *ng_run_id(const ng_run *run);
```

Returns the id pointer inside the struct, or `""` when `run` is NULL. The pointer is valid until `ng_run_close`.

### `ng_run_mark_running`

```c
int ng_run_mark_running(ng_run *run);
```

Rewrites `meta.json` to status `running` and `pid` `getpid()`. Returns 0, or -1 when `run` is NULL or the write fails.

### `ng_run_emit`

```c
int ng_run_emit(ng_run *run, const char *kind, const char *name, const char *text);
```

Appends one event. `kind` must match `^[a-z_]{1,24}$`. Returns 1 when a line is written, 0 when a filter drops it, -1 on a bad argument or I/O error.

Kinds `token`, `thinking`, `tool_result`, and `code_change` go through `filter_text` first. A held token or thinking prefix returns 1 and writes nothing yet. Other filtered kinds flush their local carry before the write. A drop returns 0. Unfiltered kinds (`status`, `tool_call`, `compact`, `intercept`, `abort`, `done`) are appended as given.

`text` is capped at 4000 bytes. `name` and `text` are JSON-escaped. The line is written with `O_APPEND` and an exclusive `flock`. `seq` increments before the write and decrements if the write fails.

### `ng_run_flush`

```c
int ng_run_flush(ng_run *run);
```

Flushes the token carry, then the thinking carry. A non-empty result is appended as that kind with an empty name. Returns 0, or -1 when `run` is NULL. The return value does not report a short write on the append.

Called at the end of a normal SSE stream, after an intercept cut, at the end of `live_emit_text`, and at the end of `ng_run_execute`. Abort skips it (`g_stream_cut == 1` returns before the intercept flush, and `consume_sse_stream` flushes only when `g_stream_cut` is 0).

### `ng_run_poll`

```c
int ng_run_poll(ng_run *run, int wait_ms);
```

Reads `control` and `intercept.txt`.

| Return | Meaning |
|--------|---------|
| 0 | Continue (`run`, or any other word). |
| 1 | Abort. Also returned when `run` is NULL. |
| 2 | Still paused. |
| 3 | `intercept.txt` exists and is non-empty. |

`wait_ms` 0 checks once. `wait_ms` > 0 sleeps in 200 ms slices until the deadline or the state changes. `wait_ms` < 0 waits until the run is not paused. Abort is tested before intercept. Intercept is tested before pause. A missing control file is treated as `run`.

### `ng_run_take_intercept`

```c
char *ng_run_take_intercept(ng_run *run);
```

Reads `intercept.txt`, unlinks it, and returns a malloc'd string. NULL when the run is NULL, the file is missing, or the file is empty. Text longer than 8000 bytes is truncated in memory with a NUL at index 8000.

### `ng_run_prompt`

```c
char *ng_run_prompt(const ng_run *run);
```

Reads `prompt.txt`. NULL when the run is NULL or the file cannot be read. The caller frees the string.

### `ng_run_load_limits`

```c
int ng_run_load_limits(const ng_run *run, ng_run_limits *lim);
```

Clears `lim`, then fills it from `config.json`. Returns 0, or -1 when an argument is NULL, the path does not fit, or the file is missing. Integers use a `strstr` plus `atoi`, so `-1` works. A key that also appears inside a string can match; the config keys written by `limits_json` are unique.

### `ng_run_finish`

```c
int ng_run_finish(ng_run *run, const char *status, const char *error);
```

Rewrites `meta.json`. `status` must be `done`, `aborted`, or `error`. Any other string, including NULL, is stored as `error`. `error` NULL is stored as an empty string. The pid field is `getpid()` of the caller. Returns 0, or -1 on a NULL run or a failed write.

### `ng_run_note_code`

```c
int ng_run_note_code(ng_run *run, const char *tool, const char *args,
                     const char *output);
```

Emits `code_change` when `looks_like_edit` matches. Returns 1 when the event is written, 0 when the tool is not an edit or a filter drops the line. See [Code change heuristic](#code-change-heuristic). The formatted line is `tool=%s args=%.240s output=%.700s`.

### `ng_run_request_abort`

```c
int ng_run_request_abort(const char *id);
```

Writes `abort\n` to `control`. Returns 0, or -1 when the id is illegal, the path does not fit, or the write fails (the directory does not exist).

### `ng_run_request_pause`

```c
int ng_run_request_pause(const char *id);
```

Writes `pause\n`. Same return as abort.

### `ng_run_request_resume`

```c
int ng_run_request_resume(const char *id);
```

Writes `run\n`. Same return as abort.

### `ng_run_request_intercept`

```c
int ng_run_request_intercept(const char *id, const char *text);
```

Rejects NULL text, text that is empty after a leading-whitespace trim, and text longer than 8000 bytes. Writes `intercept.txt.new` and `rename`s it onto `intercept.txt`. Returns 0, or -1 on reject, a bad id, or I/O failure. A failed rename unlinks the temp file.

### `ng_run_set_filter`

```c
int ng_run_set_filter(const char *id, const char *filter_json);
```

Rejects NULL, empty, length above 8000, and a document `ng_stream_filter_parse` rejects. Writes `filter.json`. Returns 0, or -1.

### `ng_run_set_limits`

```c
int ng_run_set_limits(const char *id, const ng_run_limits *limits);
```

Rejects NULL `limits`, a model containing `"`, newline, CR, or space, and an effort `ng_effort_canon` rejects. Replaces `config.json` with `limits_json`. Returns 0, or -1.

### `ng_run_events_json`

```c
char *ng_run_events_json(const char *id, long since, int limit);
```

Malloc'd plate. `since < 0` returns the whole log. `limit <= 0` means 200. `limit > 500` means 500. An illegal id returns a plate with `"error":"bad_id"`. A missing file yields `"events":[]` and `"ok":true`. Lines that do not start with `{` are skipped. `seq` is read with `atol` after `"seq":`.

### `ng_run_status_json`

```c
char *ng_run_status_json(const char *id);
```

Malloc'd status plate described under [Status](#status). Illegal id: `bad_id`. Missing `meta.json`: `run_not_found`. Allocation failure: `oom`. Opens the run to fill `seq` and the limit fields, then closes it.

### `ng_run_list_json`

```c
char *ng_run_list_json(void);
```

Malloc'd plate `{"schema":"nanobot.run.v1","ok":true,"action":"list","runs":[...],"python":0}`. Reads `$NANOBOT_HOME/runs` and embeds up to 32 `ng_run_status_json` objects, skipping names that fail `id_ok`. A missing directory yields an empty `runs` array.

### `ng_run_is_route`

```c
int ng_run_is_route(const char *path, char *id, size_t idsz, char *action,
                    size_t asz, long *since);
```

Returns 1 when `path` is `/api/v1/runs`, `/peer/v1/runs`, or `/api/runs`, optionally followed by `/<id>`, `/<id>/<action>`, and for `events` and `stream` a `/<digits>` cursor.

Clears `id` and `action`, and sets `*since` to -1, before parsing. Returns 0 when the buffers are missing or smaller than 2 bytes, the id is illegal or does not fit, the action is not one of `events`, `stream`, `abort`, `pause`, `resume`, `intercept`, `filter`, `config`, or a non-event action carries a trailing cursor.

The longest matching prefix is tested in the order `/api/v1/runs`, `/peer/v1/runs`, `/api/runs`, so `/api/v1/runs` is not parsed as an id under `/api/runs`.

### `ng_run_line_fn`

```c
typedef void (*ng_run_line_fn)(void *ud, const char *line);
```

Callback for one event line with the trailing newline removed. `ng_run_tail` does not allocate the line. The pointer is valid only for the duration of the call.

### `ng_run_tail`

```c
int ng_run_tail(const char *id, long since, int timeout_ms, ng_run_line_fn fn,
                void *ud);
```

Delivers events with `seq` greater than `since`. `timeout_ms` 0 reads once. A positive timeout polls every 50 ms. Returns the number of lines delivered. Returns 0 when the id is illegal or `fn` is NULL.

Stops when a pass sees no new line and `ng_run_status_json` contains `"status":"done"`, `"status":"aborted"`, or `"status":"error"`. A missing run is not terminal, so the HTTP handler checks for `run_not_found` before calling tail.

### `ng_run_apply`

```c
int ng_run_apply(ng_agent_cfg *c, const ng_run *run);
```

File: `src/agent.c`. Declared on `src/agent.h` so `runctl.h` does not include the agent.

Loads `config.json` and copies it onto `c` as described under [Config](#config). Does not write the env file. Returns 0, or -1 when an argument is NULL, the config cannot be read, or `ng_agent_set_reasoning_effort` rejects the effort. A `strdup` failure for the model leaves the previous model in place and still returns 0.

### `ng_run_execute`

```c
char *ng_run_execute(ng_agent_cfg *c, const char *id);
```

File: `src/agent.c`.

Opens the run, reads `prompt.txt`, marks it running, sets `c->live`, clears `g_live_streamed`, applies limits, emits `status` / `running`, and calls `ng_agent_run`. Then flushes, classifies the reply (see [Event schema](#event-schema)), clears `c->live`, closes the run, and returns the malloc'd reply.

NULL `c` or `id` returns NULL. A missing run returns a `run_not_found` plate. An empty prompt finishes `error` / `need_prompt`. A failed running mark finishes `error` / `meta_write`.

Unit tests do not call this. It runs the agent and may start curl. `tests/run` covers `ng_run_apply` on a config written by `ng_run_set_limits`.

The HTTP child is the only production caller. It must not call `ng_agent_save_env`.

### `ng_agent_cfg` fields used by a live run

```c
ng_run *live;
int max_tokens;
int context_tokens;
int compact_max_refills;
int compact_keep_tail;
int compact_keep_head;
int compact_shrink_at;
int compact_preemptive;
int compact_explicit;
```

`live` is not owned. NULL means a normal chat turn: no event log, and stream deltas go to `on_delta` when the caller asked for them.

`g_live` is a process-global copy of `c->live` for the duration of `ng_agent_run_attachments`. A cleanup attribute writes the previous value back on every return, including the early `@!` return.

### `ng_compact_policy`

```c
typedef struct ng_compact_policy {
  int refill;
  int max_refills;
  int keep_tail;
  int keep_head;
  int shrink_at;
} ng_compact_policy;
```

File: `src/wire_compat.h`. `refill` is the 1-based attempt about to run. Field defaults are in [Compaction](#compaction).

### `ng_messages_compact_ex`

```c
char *ng_messages_compact_ex(const char *messages, const ng_compact_policy *policy);
```

Shrinks a chat `messages` JSON array. Returns a malloc'd shorter array, or NULL when the input is NULL, is not an array, cannot get smaller, or is a lone prompt. The historical schedule and the explicit schedule both go through this function. Details are under [Compaction](#compaction).

### `ng_messages_compact`

```c
char *ng_messages_compact(const char *messages, int refill);
```

Calls `ng_messages_compact_ex` with max refills 3, tail 2, head -1, and shrink 1800. `refill` 1 keeps the head and the last 2. `refill` 2 keeps the head and the last 1. `refill` >= 3 keeps the last message only (head dropped because refill has reached max refills).

### HTTP helpers

These are `static` in `src/http.c`. They exist so `handle_client` can answer a run route before static files.

#### `json_key_pos`

```c
static const char *json_key_pos(const char *json, const char *key);
```

Returns a pointer at the opening quote of `key` when that quote is outside a string. A prompt that contains the letters `"filter"` does not match. Keys longer than 60 bytes return NULL. The match requires the closing quote, so `"compact"` does not match `"compact_max_refills"`.

#### `json_has_key`

```c
static int json_has_key(const char *json, const char *key);
```

Returns 1 when `json_key_pos` finds the key.

#### `json_int_key`

```c
static int json_int_key(const char *json, const char *key, int def);
```

`atoi` of the value after the key's colon, or `def` when the key is absent. `-1` parses. Whitespace after the colon is accepted by `atoi`.

#### `json_boolish`

```c
static int json_boolish(const char *json, const char *key, int def);
```

`true` returns 1, `false` returns 0, otherwise `atoi`. Absent key returns `def`.

#### `json_object_slice`

```c
static char *json_object_slice(const char *json, const char *key);
```

Malloc'd copy of the object value of `key`, braces included. NULL when the key is absent, the value is not `{`, the braces do not balance, a string inside the object is unterminated, or the slice is empty or longer than 8000 bytes. Strings are skipped so a `}` inside a string does not end the object.

#### `limits_from_body`

```c
static int limits_from_body(const char *body, ng_run_limits *lim);
```

Fills `lim` from a create or config body. Returns 0, -1 when the model does not fit in `lim->model` (96 bytes including NUL), -2 when the effort does not fit in `lim->effort` (40 bytes). NULL `lim` returns -1. NULL `body` clears `lim` and returns 0.

An empty model does not set `lim->model`. Any present effort string, including empty, sets `has_effort`. Nested `compact` sets `compact_explicit`. Flat `compact_*` keys do the same when no object is present. `compact_explicit` itself may be a boolean.

#### `model_token_ok`

```c
static int model_token_ok(const char *model);
```

Returns 1 for NULL or a string with no `"`, CR, LF, or space. `ng_run_create` JSON-escapes the model; the HTTP layer rejects those characters before create so the error token stays `bad_model`.

#### `run_prefix_of`

```c
static const char *run_prefix_of(const char *path);
```

Returns `/api/v1/runs` when the path starts with that prefix, else `/peer/v1/runs` when it starts with that prefix, else `/api/runs`.

#### `run_plate_code`

```c
static int run_plate_code(const char *plate);
```

Maps a run plate to an HTTP status. NULL is 500. `"error":"run_not_found"` is 404. `"error":"bad_id"` is 400. `"error":"oom"` is 500. Any other plate that contains `"ok":false` is 400. Everything else is 200, including a status plate whose run `status` is `error` and whose plate `ok` is true.

#### `run_send_json`

```c
static void run_send_json(int cfd, const char *plate);
```

Sends `plate` with `run_plate_code`. A NULL plate sends `{"schema":"nanobot.run.v1","ok":false,"error":"oom","python":0}` at 500.

#### `run_reject_missing`

```c
static int run_reject_missing(int cfd, const char *id);
```

Loads the status plate. Returns 1 and sends it when the plate is NULL or contains `run_not_found` or `bad_id`. Returns 0 when the run exists. Used by events, stream, and every control POST so a missing id is 404 before a 20 second tail or a `bad_filter` write.

#### `run_sse_line`

```c
static void run_sse_line(void *ud, const char *line);
```

`ng_run_line_fn` adapter. Writes `data: `, the line, and `\n\n` to `ud->fd`. NULL `ud` or `line` returns. `send_all` stops on a write error, so a disconnected client ends the tail's writes without aborting the agent.

#### `handle_runs`

```c
static int handle_runs(int cfd, const char *req, const char *path, int is_get,
                       int is_post, int is_head, ng_agent_cfg *agent,
                       ng_session *session);
```

Returns 0 when `ng_run_is_route` does not match, and the rest of `handle_client` continues. Returns 1 when this function has written the response.

Called from `handle_client` after the session-login poll and before static files. `is_get` is true for GET and HEAD. `is_head` is true only for HEAD, so the stream path can return status JSON.

`OPTIONS` is answered earlier by the generic handler. The body is the bytes after `\r\n\r\n`, or empty when that marker is absent. `read_request` caps the whole HTTP message at 8 MiB. The prompt cap of 256 KiB is applied after JSON parse.

The create child:

```c
pid_t w = fork();
if (w == 0) {
  close(cfd);
  reply = ng_run_execute(agent, id_new);
  free(reply);
  free(id_new);
  _exit(0);
}
```

Fork failure marks the meta `error` / `fork_failed` and sends HTTP 503. The parent sends `202` with `poll`, `events`, and `stream` buffers of 160 bytes. A truncated path is HTTP 500 `oom`.

## Tests

```bash
make test
```

`tests/wire` covers `ng_messages_compact` and `ng_messages_compact_ex`. `tests/filter` links `stream_filter.c` and covers redact, drop, `drop_kind`, and a held prefix. `tests/run` links the legacy library, uses a private workdir, and covers routes, filters, flush of a held prefix, the code-change heuristic, pause, intercept, resume, abort, effort canon, `ng_run_apply`, status `alive`, tail, and finish.

The HTTP path is exercised with curl against `--offline` and an `@!` prompt. That path does not call a cloud model. A prompt that is not `@!` on a local backend still tries the local chat endpoint.
