#ifndef NANOBOT_SCHED_H
#define NANOBOT_SCHED_H
/* LLM request gate: serialize (or limit concurrency) so forked workers,
 * fleet peers, and grokium host do not stampede a local llama server.
 *
 * Shared lock path by default (XDG_RUNTIME_DIR / /tmp) so *all* nanobot
 * processes on the machine coordinate — not per-NANOBOT_HOME.
 * Grok cloud: typically serial OFF. Local: ON.
 */

/* Enable/disable gate (1 = enforce slots). */
void ng_llm_sched_set_enabled(int on);
int ng_llm_sched_enabled(void);

/* Override lock file path (or NANOBOT_LLM_LOCK env). NULL = default shared. */
void ng_llm_sched_set_path(const char *path);
const char *ng_llm_sched_path(void);

/* Concurrent LLM HTTP slots (1 = exclusive). Cap 8. Env NANOBOT_LLM_SLOTS. */
void ng_llm_sched_set_slots(int n);
int ng_llm_sched_slots(void);

/* Blocking acquire/release around a single outbound LLM call (incl. stream). */
void ng_llm_sched_acquire(void);
void ng_llm_sched_release(void);

/* Non-blocking try; returns 1 if acquired, 0 if all slots busy. */
int ng_llm_sched_try_acquire(void);

/* Run fn under the gate. fn returns malloc'd string (caller frees). */
typedef char *(*ng_llm_job_fn)(void *userdata);
char *ng_llm_sched_run(ng_llm_job_fn fn, void *userdata);

/* JSON snapshot for hub status UIs (malloc'd). */
char *ng_llm_sched_status_json(void);

#endif
