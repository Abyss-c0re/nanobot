#ifndef NANOBOT_BRAINCELL_H
#define NANOBOT_BRAINCELL_H
/*
 * Braincells — specialized cells under BrainCube's internal mini-hive.
 *
 * BrainCube is a mini-hive (route/fuse lattice), not a full fleet:
 *   prompt → core.decide → SOLO | HIVE (explore+plan+implement local cells)
 *   local cells: $NANOBOT_HOME/braincells/*.json
 *   external nanobots: optional NANOBOT_PEER_URL peer bus (signal / remote capacity)
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
