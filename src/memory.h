#ifndef NANOBOT_MEMORY_H
#define NANOBOT_MEMORY_H
#include <stddef.h>

/* Bare-minimum durable context for nanobot (user folder + compacting).
 *
 * Layout under NANOBOT_HOME/memory/:
 *   core.txt     — always-on operator facts (manual or learned, hard-capped)
 *   profile.txt  — short adaptive user prefs (capped)
 *   summary.txt  — compacted older turns (capped)
 *   recent.jsonl — last N user/assistant pairs (pruned)
 */

#define NG_MEM_MAX_RECENT_TURNS 4   /* pairs kept verbatim */
#define NG_MEM_MAX_MSG_CHARS    360 /* per message when storing */
#define NG_MEM_MAX_CORE         700
#define NG_MEM_MAX_PROFILE      900
#define NG_MEM_MAX_SUMMARY     1600

void ng_memory_init(void); /* mkdir memory/ */

/* Build system content: vacuum identity + core + profile + summary. malloc'd */
char *ng_memory_system_prompt(void);

/* Load recent history as JSON array fragment of messages (no outer []).
 * Empty string if none. malloc'd. */
char *ng_memory_recent_json_fragment(void);

/* After a completed exchange: store user+assistant, prune, compact old. */
void ng_memory_record_exchange(const char *user, const char *assistant);

/* Optional: append a short learned profile note (deduped, capped). */
void ng_memory_note_profile(const char *line);

#endif
