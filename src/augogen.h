#ifndef NANOBOT_AUGOGEN_H
#define NANOBOT_AUGOGEN_H
#include "agent.h"

/* Augogen — Grok Build x.ai/suggestPrompt compatibility.
 * Full auto (default): generate → braincube pair-confirm → apply → enqueue run.
 * Mesh vote 0 still vetoes. HOLD_FLASH=1. Forbidden: flash/wipe/cycle-stop.
 * NANOBOT_AUGOGEN_AUTO=0 / settings AUGOGEN_AUTO=0 restores vote-only.
 * Schema: nanobot.augogen.v1
 */

#define NG_AUGOGEN_SCHEMA "nanobot.augogen.v1"

/* Handle POST JSON (action=generate|pending|vote|reject|apply|reset).
 * agent may be NULL for pending/vote/reject/reset (no LLM).
 * Returns malloc'd dual-wire plate. */
char *ng_augogen_handle(ng_agent_cfg *agent, const char *json_body);

/* GET pending plate (malloc'd). */
char *ng_augogen_pending_json(void);

#endif
