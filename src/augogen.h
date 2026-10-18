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
 * A concept line with one word said twice in a row keeps sketch distance zero.
 * A failed fork marks that auto job error and names it. It does not stay queued.
 * The plate stays unexecuted. A queued job left for a later listen stays queued.
 * A motive text decodes a JSON unicode escape before the rank.
 * A JSON backspace or form feed in that text is a separator before the rank.
 * A motive request may pass a quoted lex_w for that call only.
 * Generate keeps the process weight. The plate file omits lex_w.
 * When that motive request quoted lex_w, the one peer POST quotes it too.
 * A missing or bad value omits lex_w on the hop.
 * That POST also quotes the ranked plate name, lexical, hamming, and concept.
 * A quoted plate, lexical, hamming, or concept that disagrees with the peer
 * rank is not stored. A missing value keeps the store.
 * A quote in the plate status, plate name, or cli version stays escaped.
 * A symlink at the plate path is not read. The write replaces the link
 * and leaves the target.
 * A symlink named augogen is not the plate directory. The read ignores
 * it. The write replaces that link with a directory and leaves the target.
 * A regular file named augogen is not the plate directory. The read
 * ignores it. The write replaces that file with a directory only when
 * it holds no bytes. A file that still holds bytes stays, and the write
 * does not replace it.
 * A fifo named augogen is not the plate directory. The read ignores
 * it and does not open it. The write replaces that fifo with a
 * directory and does not open it. A waiting writer is not paired.
 * A directory at the plate path is not the plate. The read ignores it.
 * The write removes that directory only when it is empty. A directory
 * that still holds a name stays, and the write does not enter it.
 * A symlink named guide.bin or oversee.bin is not the cube. The read
 * ignores it. The write replaces that link and leaves the target.
 * A directory named guide.bin or oversee.bin is not the cube. The read
 * ignores it. The write removes that directory only when it is empty.
 * A directory that still holds a name stays, and the write does not
 * enter it.
 * Generate does not POST. Without enqueue the plate stays empty.
 * agent may be NULL for pending/vote/reject/reset (no LLM).
 * Returns malloc'd dual-wire plate. */
char *ng_augogen_handle(ng_agent_cfg *agent, const char *json_body);

/* GET pending plate (malloc'd). */
char *ng_augogen_pending_json(void);

#endif
