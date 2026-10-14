#ifndef NANOBOT_WIRE_COMPAT_H
#define NANOBOT_WIRE_COMPAT_H

#include <stddef.h>

/* Canonical reasoning-effort token. 0 = ok (empty out means omit). -1 = reject. */
int ng_effort_canon(const char *in, char *out, size_t outsz);

/* Proxy attribution id. Default "nanobot". NANOBOT_CLIENT_NAME, else GROK_CLIENT_NAME. */
const char *ng_client_identifier(void);

/* Cut text to max_len on a newline or whitespace, then "\n…" when shortened. */
char *ng_suggest_truncate(const char *text, size_t max_len);

/* Keep the newest tail of a preformatted transcript inside the 24000-char budget. */
char *ng_suggest_pack_text(const char *transcript);

/* Memory order (oldest first). Newest agent reply is not mid-cut. NULL if no agent line. */
char *ng_suggest_pack_turns(const char *const *roles, const char *const *texts, int n);

/* 1 when a provider body is a context-length error. */
int ng_resp_is_context_overflow(const char *resp);

/* Shrink a chat messages JSON array. NULL when the array cannot get smaller.
 * refill is 1..3. Caller stops after three failed or exhausted refills. */
char *ng_messages_compact(const char *messages, int refill);

/* structuredContent object/array when present, else NULL (caller keeps text). */
char *ng_mcp_extract_tool_result(const char *resp);

/* Bearer token file, re-read by the caller. NULL if missing, empty, or unsafe. */
char *ng_mcp_read_bearer_file(const char *path);

#endif
