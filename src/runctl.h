#ifndef NANOBOT_RUNCTL_H
#define NANOBOT_RUNCTL_H

#include <stddef.h>

/* File-backed async run. Each HTTP worker is a separate process, so abort,
 * intercept, filters, and limit changes are files under $NANOBOT_HOME/runs/<id>/.
 * The worker re-reads them between turns and between stream chunks.
 */

typedef struct ng_run ng_run;

/* Limits copied onto the agent at the start of each turn.
 * compact_keep_head < 0 uses the historical schedule.
 * compact_explicit 0 leaves compaction at the historical 3-refill default.
 * max_tokens 0 lets the agent pick a completion cap from the prompt length.
 * context_tokens 0 disables preemptive compaction.
 */
typedef struct ng_run_limits {
  char model[96];
  char effort[40];
  int has_effort;
  int max_tokens;
  int context_tokens;
  int compact_max_refills;
  int compact_keep_tail;
  int compact_keep_head;
  int compact_shrink_at;
  int compact_preemptive;
  int compact_explicit;
} ng_run_limits;

void ng_run_limits_clear(ng_run_limits *lim);

/* Create runs/<id> and return a malloc'd id. NULL on a bad prompt or I/O error.
 * prompt is required. limits and filter_json may be NULL.
 */
char *ng_run_create(const char *prompt, const ng_run_limits *limits,
                    const char *filter_json);

ng_run *ng_run_open(const char *id);
void ng_run_close(ng_run *run);
const char *ng_run_id(const ng_run *run);

/* status=running and the worker pid. */
int ng_run_mark_running(ng_run *run);

/* Append one event. Text of token, thinking, tool_result, and code_change
 * passes through the run filter first. Returns 1 written, 0 dropped by a
 * filter, -1 on error. A held needle prefix returns 1 and writes nothing yet.
 */
int ng_run_emit(ng_run *run, const char *kind, const char *name, const char *text);

/* Write a held token or thinking prefix as its own event. */
int ng_run_flush(ng_run *run);

/* 0 continue, 1 abort, 2 still paused, 3 intercept text is waiting.
 * wait_ms 0 checks once. wait_ms < 0 waits until the run is not paused.
 * Abort wins over intercept. Intercept wins over pause.
 */
int ng_run_poll(ng_run *run, int wait_ms);

/* Read and delete intercept.txt. NULL when empty. */
char *ng_run_take_intercept(ng_run *run);

/* Read prompt.txt. */
char *ng_run_prompt(const ng_run *run);

/* Reload config.json into lim. Returns 0, or -1 if the run is missing. */
int ng_run_load_limits(const ng_run *run, ng_run_limits *lim);

/* status is done, aborted, or error. error may be NULL. */
int ng_run_finish(ng_run *run, const char *status, const char *error);

/* Emit code_change when the tool args or output look like a file edit.
 * Returns 1 when an event was written, 0 when this tool is not an edit.
 */
int ng_run_note_code(ng_run *run, const char *tool, const char *args,
                     const char *output);

int ng_run_request_abort(const char *id);
int ng_run_request_pause(const char *id);
int ng_run_request_resume(const char *id);
int ng_run_request_intercept(const char *id, const char *text);
int ng_run_set_filter(const char *id, const char *filter_json);
int ng_run_set_limits(const char *id, const ng_run_limits *limits);

/* Plates. since < 0 means the whole log. limit <= 0 means 200. */
char *ng_run_events_json(const char *id, long since, int limit);
char *ng_run_status_json(const char *id);
char *ng_run_list_json(void);

/* 1 when path is /api/runs, /api/v1/runs, or /peer/v1/runs, plus an optional
 * id, action, and numeric cursor (/events/12, /stream/0).
 * id and action may be empty. *since is -1 when the path has no cursor.
 */
int ng_run_is_route(const char *path, char *id, size_t idsz, char *action,
                    size_t asz, long *since);

typedef void (*ng_run_line_fn)(void *ud, const char *line);

/* Deliver events with seq > since. timeout_ms 0 reads once.
 * Stops early when the run status is done, aborted, or error.
 * Returns the number of lines delivered.
 */
int ng_run_tail(const char *id, long since, int timeout_ms, ng_run_line_fn fn,
                void *ud);

#endif
