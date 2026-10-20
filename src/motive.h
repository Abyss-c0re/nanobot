#ifndef NANOBOT_MOTIVE_H
#define NANOBOT_MOTIVE_H

/* Hive motive. Hybrid rank of eight concepts plus a priority cascade.
 * Rank uses the caller stack: 192-byte norm, 24 tokens, no heap.
 * A concept line with one word said twice in a row keeps sketch distance zero.
 * NANOBOT_MOTIVE_LEX_W is the lexical weight, 0-255. Unset stays 64.
 * NANOBOT_MOTIVE_LEARN may name a pairs file that fits that weight.
 * The fit does not store a line and does not add a concept.
 * A forward plate may become a 2–12 word self-prompt. Destructive plates
 * stay on the wire as a remedy line and are not applied.
 * Schema: nanobot.motive.v1
 */

#define NG_MOTIVE_SCHEMA "nanobot.motive.v1"
#define NG_MOTIVE_CONCEPTS 8
#define NG_MOTIVE_TEXT 64

enum {
  NG_PLATE_OBSERVE = 0,
  NG_PLATE_CONFESS,
  NG_PLATE_HOLD_FLASH,
  NG_PLATE_FORBID_LIVE,
  NG_PLATE_BAKE_ROM,
  NG_PLATE_AUDIT,
  NG_PLATE_RESTORE_FLOW,
  NG_PLATE_REPORT_REASON,
  NG_PLATE_INVESTIGATE,
  NG_PLATE_PURGE,
  NG_PLATE_PROTECT,
  NG_PLATE_BRIEF
};

typedef struct {
  int plate;
  int forward; /* only a forward plate may be stored as a self-prompt */
  int rank;
  int hamming;
  int lexical;
  int lex_w; /* NANOBOT_MOTIVE_LEX_W used for this rank. Unset is 64. */
  char concept[NG_MOTIVE_TEXT];
  char line[NG_MOTIVE_TEXT];
} ng_motive;

const char *ng_motive_plate_name(int plate);
void ng_motive_rank(const char *text, ng_motive *out);
/* over < 0 reads NANOBOT_MOTIVE_LEX_W. 0-255 applies to this call only.
 * Outside that range, a pairs file that supplies the weight also adds
 * ng_motive_label_lex to that concept's lexical count. */
void ng_motive_rank_w(const char *text, ng_motive *out, int over);
/* Grid-search lex_w on up to 32 (text, catalog line) pairs.
 * No pairs, a tie that includes 64, or a peak only at weight 0
 * returns 64. Does not store. */
int ng_motive_learn_lex_w(const char *const *text, const char *const *prefer,
                          int n);
/* Malloc'd catalog line of the pairs-file query with the greatest lexical
 * overlap. Ties keep the earlier pair. NULL when the file is missing, the
 * query is empty, or the overlap is below 1. The caller frees the line. */
char *ng_motive_recall_pref(const char *query, const char *path);
/* How many pairs rank their preferred catalog line at the fitted weight.
 * Null, empty, a path of 512 bytes or more, a missing file, a non-regular
 * file, or no valid pair returns -1. Zero is a real count. Does not read
 * the environment or write the learn cache. */
int ng_motive_learn_hits(const char *path);
/* Hits of query against the one training query labeled with this catalog
 * concept, minus the hits already on that concept line. Negative is 0.
 * Null, empty, a missing or non-regular file, an unknown concept, or no
 * exact catalog pair returns 0. Same pairs read as the fit. Does not read
 * the environment, write the learn cache, or allocate. */
int ng_motive_label_lex(const char *query, const char *path, const char *concept);
/* How many pairs the labeled hybrid score names as their preferred
 * catalog line at the fitted weight. The label count is added on this
 * call. Null, empty, a path of 512 bytes or more, a missing file, a
 * non-regular file, or no valid pair returns -1. Zero is a real count.
 * Does not read the environment or write the learn cache. */
int ng_motive_label_hits(const char *path);
/* One motive at the fitted weight. The label count is added on this
 * call. Null out returns -1. Null, empty, a path of 512 bytes or more,
 * a missing file, a non-regular file, or no valid pair returns -1 and
 * zeroes out. Zero is not a hit count. Does not install
 * NANOBOT_MOTIVE_LEARN or write the learn cache. */
int ng_motive_label_rank(const char *query, const char *path, ng_motive *out);
/* Winning labeled score minus the best score among the other seven.
 * A tie at the top returns 0. An empty norm returns 0. Null, empty,
 * a path of 512 bytes or more, a missing file, a non-regular file, or
 * no valid pair returns -1. Does not read the environment or write
 * the learn cache. */
int ng_motive_label_margin(const char *query, const char *path);
/* How many kept pairs have a positive labeled margin on this path.
 * A return of 0 does not count. Null, empty, a path of 512 bytes or
 * more, a missing file, a non-regular file, or no valid pair returns
 * -1. Zero is a real count. Does not read the environment or write
 * the learn cache. */
int ng_motive_margin_hits(const char *path);
/* Greatest labeled margin among the kept pairs on this path.
 * A return of -1 is ignored. Every kept pair at 0 returns 0.
 * Every call at -1 returns -1. Null, empty, a path of 512 bytes
 * or more, a missing file, a non-regular file, or no valid pair
 * returns -1. Does not read the environment or write the learn
 * cache. */
int ng_motive_margin_max(const char *path);
/* How many kept pairs share the greatest labeled margin on this path.
 * A return of -1 is ignored. Every call at -1 returns -1.
 * A pair at margin 0 counts when 0 is the greatest. Null, empty,
 * a path of 512 bytes or more, a missing file, a non-regular file,
 * or no valid pair returns -1. Does not read the environment or
 * write the learn cache. */
int ng_motive_margin_top(const char *path);
/* Sum of the labeled margins on the kept pairs of this path.
 * A return of -1 is ignored. Every call at -1 returns -1.
 * A margin of 0 adds nothing. Null, empty, a path of 512 bytes
 * or more, a missing file, a non-regular file, or no valid pair
 * returns -1. Does not read the environment or write the learn
 * cache. */
int ng_motive_margin_sum(const char *path);
/* Smallest labeled margin among the kept pairs on this path.
 * A return of -1 is ignored. Every kept pair at 0 returns 0.
 * Every call at -1 returns -1. Null, empty, a path of 512 bytes
 * or more, a missing file, a non-regular file, or no valid pair
 * returns -1. Does not read the environment or write the learn
 * cache. */
int ng_motive_margin_min(const char *path);
/* How many kept pairs have a labeled margin strictly below the
 * greatest margin on this path. A return of -1 is ignored. Every
 * call at -1 returns -1. Every kept pair at the same margin returns
 * 0. A margin of 0 counts when the greatest is greater than 0.
 * Null, empty, a path of 512 bytes or more, a missing file, a
 * non-regular file, or no valid pair returns -1. Does not read the
 * environment or write the learn cache. */
int ng_motive_margin_under(const char *path);
/* How many kept pairs have a labeled margin equal to the smallest
 * margin on this path. A return of -1 is ignored. Every call at -1
 * returns -1. Every kept pair at the same margin returns that count.
 * A margin of 0 counts when 0 is the smallest. Null, empty, a path
 * of 512 bytes or more, a missing file, a non-regular file, or no
 * valid pair returns -1. Does not read the environment or write the
 * learn cache. */
int ng_motive_margin_low(const char *path);
/* Least labeled margin strictly greater than the smallest margin
 * on this path. A return of -1 is ignored. Every call at -1
 * returns -1. Every kept pair at the same margin returns -1.
 * A margin of 0 is the smallest when it is present, and the result
 * is then the least margin strictly above 0. When no kept margin
 * is strictly above the smallest, the result is -1. Null, empty,
 * a path of 512 bytes or more, a missing file, a non-regular file,
 * or no valid pair returns -1. Does not read the environment or
 * write the learn cache. */
int ng_motive_margin_next(const char *path);
/* How many distinct labeled margins the kept pairs have on this path.
 * A return of -1 is ignored. Every call at -1 returns -1. Every kept
 * pair at the same margin returns 1. A margin of 0 is one distinct
 * margin when it is present. Two different margins return 2. Null,
 * empty, a path of 512 bytes or more, a missing file, a non-regular
 * file, or no valid pair returns -1. Does not read the environment
 * or write the learn cache. */
int ng_motive_margin_kinds(const char *path);
/* The labeled margin shared by the most kept pairs on this path.
 * A return of -1 is ignored. Every call at -1 returns -1. When two
 * margins are shared by the same number of pairs, the greater margin
 * is the result. A uniform file returns that one margin, including 0.
 * Null, empty, a path of 512 bytes or more, a missing file, a
 * non-regular file, or no valid pair returns -1. Does not read the
 * environment or write the learn cache. */
int ng_motive_margin_mode(const char *path);
/* Smallest labeled margin that appears on exactly one kept pair on
 * this path. A return of -1 is ignored. Every call at -1 returns -1.
 * A single kept margin returns that margin, including 0. Two or more
 * kept pairs that all share one margin return -1. When no margin has
 * a count of exactly 1, the result is -1. Null, empty, a path of 512
 * bytes or more, a missing file, a non-regular file, or no valid pair
 * returns -1. Does not read the environment or write the learn cache. */
int ng_motive_margin_lone(const char *path);
/* How many kept pairs have a labeled margin that appears exactly once
 * on this path. That number is also the count of distinct margins
 * whose count is exactly 1. A return of -1 is ignored. Every call at
 * -1 returns -1. A single kept margin returns 1, including 0. Two or
 * more kept pairs that all share one margin return 0. When no margin
 * has a count of exactly 1, the result is 0. Null, empty, a path of
 * 512 bytes or more, a missing file, a non-regular file, or no valid
 * pair returns -1. Does not read the environment or write the learn
 * cache. */
int ng_motive_margin_ones(const char *path);
/* Smallest winning labeled hamming among the kept pairs on this path.
 * A return of -1 from the labeled rank is ignored. Every call at -1
 * returns -1. A winning hamming of 0 returns 0. Null, empty, a path
 * of 512 bytes or more, a missing file, a non-regular file, or no
 * valid pair returns -1. Does not read the environment or write the
 * learn cache. */
int ng_motive_ham_min(const char *path);
/* Greatest winning labeled hamming among the kept pairs on this path.
 * A return of -1 from the labeled rank is ignored. Every call at -1
 * returns -1. A winning hamming of 0 returns 0. Null, empty, a path
 * of 512 bytes or more, a missing file, a non-regular file, or no
 * valid pair returns -1. Does not read the environment or write the
 * learn cache. */
int ng_motive_ham_max(const char *path);
/* Sum of the winning labeled hammings among the kept pairs on this
 * path. A return of -1 from the labeled rank is ignored. Every call
 * at -1 returns -1. A winning hamming of 0 adds nothing. A file whose
 * winning hammings are all 0 returns 0. Null, empty, a path of 512
 * bytes or more, a missing file, a non-regular file, or no valid pair
 * returns -1. Does not read the environment or write the learn cache. */
int ng_motive_ham_sum(const char *path);
char *ng_motive_format(const ng_motive *m);
/* 1 when the act is a destructive plate (purge or protect). */
int ng_motive_blocks(const char *text);
char *ng_motive_json(const char *text);

#endif
