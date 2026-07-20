#ifndef NANOBOT_IMPROVE_H
#define NANOBOT_IMPROVE_H
#include "agent.h"

/* Self-improvement mode: seed goals, run agent cycles, append improve log. */

void ng_improve_seed(void); /* ensure memory/self_improve.txt + goals */

/* Status JSON (malloc'd): cycles logged, last line, paths */
char *ng_improve_status_json(void);

/* One cycle: build prompt, run agent, append log. Returns reply (malloc'd). */
char *ng_improve_run_cycle(ng_agent_cfg *agent, const char *focus);

/* N cycles; returns concatenated report (malloc'd). */
char *ng_improve_run_n(ng_agent_cfg *agent, int n, const char *focus);

/* Append free-form note to improve_log.jsonl */
void ng_improve_log_line(const char *kind, const char *text);

#endif
