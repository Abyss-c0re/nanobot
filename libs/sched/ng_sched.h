#ifndef NANOBOT_SCHED_H
#define NANOBOT_SCHED_H
/* Light LLM request gate: optional serialize (file lock) so forked workers
 * and concurrent peer handlers do not stampede a local llama server.
 * Grok: typically off. Local: typically on. */

/* Enable/disable serialization (1 = one LLM HTTP at a time process-wide+fork). */
void ng_llm_sched_set_enabled(int on);
int ng_llm_sched_enabled(void);

/* Blocking acquire/release around a single outbound LLM call. No-op if disabled. */
void ng_llm_sched_acquire(void);
void ng_llm_sched_release(void);

/* Run fn under the gate. fn returns malloc'd string (caller frees). */
typedef char *(*ng_llm_job_fn)(void *userdata);
char *ng_llm_sched_run(ng_llm_job_fn fn, void *userdata);

#endif
