#ifndef NANOBOT_BRAINCELL_H
#define NANOBOT_BRAINCELL_H
/*
 * Braincells — nanobots as specialized cells under a BrainCube decision core.
 *
 * Coding hive:
 *   prompt → core.decide → SOLO (one agent) | HIVE (explore+plan+implement cells)
 *   cells communicate via $NANOBOT_HOME/braincells JSON files (+ optional peer bus)
 *   core fuses reports and chooses the next step
 *
 * Enabled when NANOBOT_BRAINCELLS=1 (Grokium sets this for desktop coding).
 */

#include "agent.h"

/* 1 if braincells/hive enabled for this process. */
int ng_braincell_enabled(void);

/* Heuristic: prompt looks like a coding/implementation task. */
int ng_braincell_is_coding_prompt(const char *prompt);

/**
 * Optional coding hive entry. Returns malloc'd final answer if hive handled
 * the prompt; NULL to fall through to normal agent_run.
 * Uses subagents as cells + BrainCube for route/fuse decisions.
 */
char *ng_braincell_try_coding(ng_agent_cfg *c, const char *prompt,
                              int stream_final, ng_stream_fn on_delta, void *ud);

/* OpenAI tools fragment (leading comma) or empty strdup. */
char *ng_braincell_openai_tools_fragment(void);

/* Tool dispatch; malloc result or NULL if not ours. */
char *ng_braincell_try_tool(ng_agent_cfg *c, const char *name, const char *args_json);

/* Status for /resources or hub (malloc JSON). */
char *ng_braincell_status_json(void);

#endif
