#ifndef NANOBOT_STREAM_FILTER_H
#define NANOBOT_STREAM_FILTER_H

#include <stddef.h>

/* Streaming output filter. Rules run in order on each chunk.
 * op "redact" replaces match with with (default "***").
 * op "drop" discards the whole pending chunk when match occurs.
 * op "drop_kind" discards every chunk whose kind equals match or kind.
 * A needle split across chunks is held until it completes or the flush.
 * match, kind, and with are at most 128 bytes. At most 16 rules.
 */
typedef struct ng_stream_filter ng_stream_filter;

/* NULL or "{}" or {"rules":[]} is a pass-through filter.
 * A broken object returns NULL. */
ng_stream_filter *ng_stream_filter_parse(const char *json);
void ng_stream_filter_free(ng_stream_filter *f);

/* Longest drop/redact needle, minus one. The caller hold buffer must be
 * at least this many bytes (160 is enough for the 128-byte cap). */
size_t ng_stream_filter_hold(const ng_stream_filter *f);

/* Push one chunk. kind NULL is "token".
 * carry / *carry_n hold an unmatched needle prefix between calls.
 * carry_sz is the byte size of carry. *out is malloc'd when the return is 1
 * (an empty string means the chunk is held, not dropped).
 * Returns 1 pass, 0 dropped, -1 allocation failure.
 */
int ng_stream_filter_push(ng_stream_filter *f, const char *kind, const char *text,
                          char *carry, size_t *carry_n, size_t carry_sz, char **out);

/* Emit a held prefix at the end of a stream. Same return codes as push. */
int ng_stream_filter_flush(ng_stream_filter *f, const char *kind,
                           char *carry, size_t *carry_n, size_t carry_sz, char **out);

#endif
