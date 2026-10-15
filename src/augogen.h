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

/* Handle POST JSON (action=generate|pending|vote|reject|apply|reset|motive).
 * motive ranks eight concepts and a heresy cascade. enqueue=1 stores a
 * forward line. Auto runs it only when an agent is present.
 * One optional IPv4 peer receives that stored line once. hop=1 does not
 * forward again. An empty NANOBOT_MOTIVE_PEER means no POST.
 * The peer token may come from an owner-only file. A group or other bit
 * skips that file.
 * A generate with no model line reports a forward concept and its plate.
 * enqueue=1 stores that concept line and keeps the plate name,
 * rank, lexical, and hamming.
 * The motive JSON and the generate report name lex_w.
 * A motive text decodes a JSON unicode escape before the rank.
 * A JSON backspace or form feed in that text is a separator before the rank.
 * A motive request may pass a quoted lex_w for that call only.
 * Generate keeps the process weight. The plate file omits lex_w.
 * When that motive request quoted lex_w, the one peer POST quotes it too.
 * A missing or bad value omits lex_w on the hop.
 * That POST also quotes the ranked plate name, lexical, hamming, and concept.
 * A quoted plate, lexical, hamming, or concept that disagrees with the peer
 * rank is not stored. A missing value keeps the store.
 * Generate does not POST. Without enqueue the plate stays empty.
 * agent may be NULL for pending/vote/reject/reset (no LLM).
 * Returns malloc'd dual-wire plate. */
char *ng_augogen_handle(ng_agent_cfg *agent, const char *json_body);

/* GET pending plate (malloc'd). */
char *ng_augogen_pending_json(void);

#endif
