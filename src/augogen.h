#ifndef NANOBOT_AUGOGEN_H
#define NANOBOT_AUGOGEN_H
#include "agent.h"

/* Augogen — Grok Build x.ai/suggestPrompt compatibility.
 * Generates a logical next user line. NEVER auto-executes.
 * Confirm only after pair vote (guide + oversee) as one braincube.
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
