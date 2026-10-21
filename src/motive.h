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
/* Least winning labeled hamming strictly greater than the smallest
 * winning labeled hamming among the kept pairs on this path. A return
 * of -1 from the labeled rank is ignored. Every call at -1 returns -1.
 * Every kept pair at the same hamming returns -1, including 0. Zero is
 * a real hamming. When no kept hamming is strictly above the smallest,
 * the result is -1. Null, empty, a path of 512 bytes or more, a missing
 * file, a non-regular file, or no valid pair returns -1. Does not read
 * the environment or write the learn cache. */
int ng_motive_ham_next(const char *path);
/* Winning labeled hamming shared by the most kept pairs on this path.
 * A return of -1 from the labeled rank is ignored. Every call at -1
 * returns -1. When two hammings are shared by the same number of pairs,
 * the greater hamming is the result. A uniform file returns that
 * hamming, including 0. Zero is a real hamming. Null, empty, a path of
 * 512 bytes or more, a missing file, a non-regular file, or no valid
 * pair returns -1. Does not read the environment or write the learn
 * cache. */
int ng_motive_ham_mode(const char *path);
/* Smallest winning labeled hamming that appears on exactly one kept
 * pair on this path. A return of -1 from the labeled rank is ignored.
 * Every call at -1 returns -1. A single kept pair returns that
 * hamming, including 0. Two or more kept pairs that all share one
 * hamming return -1. When no hamming has a count of exactly 1, the
 * result is -1. When two or more hammings each appear once, the
 * smaller hamming is the result. Zero is a real hamming. Null, empty,
 * a path of 512 bytes or more, a missing file, a non-regular file, or
 * no valid pair returns -1. Does not read the environment or write
 * the learn cache. */
int ng_motive_ham_lone(const char *path);
/* Greatest winning labeled hamming that appears on exactly one kept
 * pair on this path. A return of -1 from the labeled rank is ignored.
 * Every call at -1 returns -1. A single kept pair returns that
 * hamming, including 0. Two or more kept pairs that all share one
 * hamming return -1. When no hamming has a count of exactly 1, the
 * result is -1. When two or more hammings each appear once, the
 * greater hamming is the result. Zero is a real hamming. Null, empty,
 * a path of 512 bytes or more, a missing file, a non-regular file, or
 * no valid pair returns -1. Does not read the environment or write
 * the learn cache. */
int ng_motive_ham_upper(const char *path);
/* Greatest labeled margin that appears on exactly one kept pair on
 * this path. A return of -1 is ignored. Every call at -1 returns -1.
 * A single kept margin returns that margin, including 0. Two or more
 * kept pairs that all share one margin return -1. When no margin has
 * a count of exactly 1, the result is -1. When two or more margins
 * each appear once, the greater margin is the result. Zero is a real
 * margin. Null, empty, a path of 512 bytes or more, a missing file,
 * a non-regular file, or no valid pair returns -1. Does not read the
 * environment or write the learn cache. */
int ng_motive_margin_upper(const char *path);
/* How many kept pairs have a labeled margin strictly greater than the
 * smallest labeled margin and strictly less than the greatest labeled
 * margin on this path. A return of -1 is ignored. Every call at -1
 * returns -1. When every kept margin is the same, including 0, the
 * result is 0. When the kept margins have exactly two distinct values,
 * the result is 0. A single kept pair returns 0. A margin equal to the
 * smallest or the greatest is not in the middle. Zero is a real margin.
 * Null, empty, a path of 512 bytes or more, a missing file, a
 * non-regular file, or no valid pair returns -1. Does not read the
 * environment or write the learn cache. */
int ng_motive_margin_mid(const char *path);
/* Sum of the labeled margins that appear on exactly one kept pair on
 * this path. A return of -1 is ignored. Every call at -1 returns -1.
 * A single kept pair returns that margin, including 0. A repeated
 * margin adds nothing, including a repeated 0. When kept margins exist
 * and none appears once, the result is 0. Zero is a real sum. Null,
 * empty, a path of 512 bytes or more, a missing file, a non-regular
 * file, or no valid pair returns -1. Does not read the environment
 * or write the learn cache. */
int ng_motive_margin_osum(const char *path);
/* Sum of the winning labeled hammings that appear on exactly one
 * kept pair on this path. A return of -1 from the labeled rank is
 * ignored. Every call at -1 returns -1. A single kept pair returns
 * that hamming, including 0. A repeated hamming adds nothing,
 * including a repeated 0. When kept hammings exist and none appears
 * once, the result is 0. Zero is a real sum. Null, empty, a path of
 * 512 bytes or more, a missing file, a non-regular file, or no valid
 * pair returns -1. Does not read the environment or write the learn
 * cache. */
int ng_motive_ham_osum(const char *path);
/* Sum of the distinct labeled margins on the kept pairs of this path.
 * A return of -1 is ignored. Every call at -1 returns -1. Each margin
 * value is added once, whether that value appears on one kept pair or
 * on many. A repeated value is added once, not once per pair. A
 * repeated 0 is added once and adds nothing. A single kept pair
 * returns that margin, including 0. When the only distinct margin is
 * 0, the result is 0. Zero is a real sum. Null, empty, a path of 512
 * bytes or more, a missing file, a non-regular file, or no valid pair
 * returns -1. Does not read the environment or write the learn cache. */
int ng_motive_margin_ksum(const char *path);
/* Sum of the distinct winning labeled hammings on the kept pairs of
 * this path. A return of -1 from the labeled rank is ignored. Every
 * call at -1 returns -1. Each hamming value is added once, whether
 * that value appears on one kept pair or on many. A repeated value is
 * added once, not once per pair. A repeated 0 is added once and adds
 * nothing. A single kept pair returns that hamming, including 0.
 * When the only distinct hamming is 0, the result is 0. Zero is a
 * real sum. Null, empty, a path of 512 bytes or more, a missing file,
 * a non-regular file, or no valid pair returns -1. Does not read the
 * environment or write the learn cache. */
int ng_motive_ham_ksum(const char *path);
/* Greatest winning labeled hamming strictly less than the greatest
 * winning labeled hamming among the kept pairs on this path. A return
 * of -1 from the labeled rank is ignored. Every call at -1 returns -1.
 * Every kept pair at the same hamming returns -1, including 0. Zero is
 * a real hamming. When that greatest-below value is 0, the result is
 * 0. When no kept hamming is strictly below the greatest, the result
 * is -1. Null, empty, a path of 512 bytes or more, a missing file, a
 * non-regular file, or no valid pair returns -1. Does not read the
 * environment or write the learn cache. */
int ng_motive_ham_prev(const char *path);
/* Greatest labeled margin strictly less than the greatest labeled
 * margin among the kept pairs on this path. A return of -1 is ignored.
 * Every call at -1 returns -1. Every kept pair at the same margin
 * returns -1, including 0. Zero is a real margin. When that
 * greatest-below value is 0, the result is 0. When no kept margin is
 * strictly below the greatest, the result is -1. Null, empty, a path
 * of 512 bytes or more, a missing file, a non-regular file, or no
 * valid pair returns -1. Does not read the environment or write the
 * learn cache. */
int ng_motive_margin_prev(const char *path);
/* Greatest labeled margin strictly greater than the smallest labeled
 * margin among the kept pairs on this path. A return of -1 is ignored.
 * Every call at -1 returns -1. Every kept pair at the same margin
 * returns -1, including 0. Zero is a real margin. When that
 * greatest-above value is 0, the result is 0. When no kept margin is
 * strictly above the smallest, the result is -1. Null, empty, a path
 * of 512 bytes or more, a missing file, a non-regular file, or no
 * valid pair returns -1. Does not read the environment or write the
 * learn cache. */
int ng_motive_margin_high(const char *path);
/* Greatest winning labeled hamming strictly greater than the smallest
 * winning labeled hamming among the kept pairs on this path. A return
 * of -1 from the labeled rank is ignored. Every call at -1 returns -1.
 * Every kept pair at the same hamming returns -1, including 0. Zero is
 * a real hamming. When that greatest-above value is 0, the result is
 * 0. When no kept hamming is strictly above the smallest, the result
 * is -1. Null, empty, a path of 512 bytes or more, a missing file, a
 * non-regular file, or no valid pair returns -1. Does not read the
 * environment or write the learn cache. */
int ng_motive_ham_high(const char *path);
/* Sum of the winning labeled hammings strictly greater than the
 * smallest winning labeled hamming among the kept pairs on this path.
 * A return of -1 from the labeled rank is ignored. Every call at -1
 * returns -1. Every kept pair at the same hamming returns -1,
 * including 0. A repeated hamming above the smallest is added again.
 * Zero is a real hamming. When that sum is 0, the result is 0. When
 * no kept hamming is strictly above the smallest, the result is -1.
 * Null, empty, a path of 512 bytes or more, a missing file, a
 * non-regular file, or no valid pair returns -1. Does not read the
 * environment or write the learn cache. */
int ng_motive_ham_asum(const char *path);
/* Sum of the labeled margins strictly greater than the smallest
 * labeled margin among the kept pairs on this path. A return of -1
 * is ignored. Every call at -1 returns -1. Every kept pair at the
 * same margin returns -1, including 0. A repeated margin above the
 * smallest is added again. Zero is a real margin. When that sum is
 * 0, the result is 0. When no kept margin is strictly above the
 * smallest, the result is -1. Null, empty, a path of 512 bytes or
 * more, a missing file, a non-regular file, or no valid pair returns
 * -1. Does not read the environment or write the learn cache. */
int ng_motive_margin_asum(const char *path);
/* Sum of the labeled margins that appear on at least two kept pairs
 * of this path. A return of -1 is ignored. Every call at -1 returns
 * -1. A margin that appears once adds nothing. Each kept pair whose
 * margin value appears at least twice adds that margin. A repeated
 * margin is added again, including a repeated 0. When no margin value
 * appears at least twice, the result is -1. Zero is a real margin.
 * When that sum is 0, the result is 0. Null, empty, a path of 512
 * bytes or more, a missing file, a non-regular file, or no valid pair
 * returns -1. Does not read the environment or write the learn cache. */
int ng_motive_margin_rsum(const char *path);
/* Sum of the winning labeled hammings that appear on at least two
 * kept pairs of this path. A return of -1 from the labeled rank is
 * ignored. Every call at -1 returns -1. A hamming that appears once
 * adds nothing. Each kept pair whose hamming value appears at least
 * twice adds that hamming. A repeated hamming is added again,
 * including a repeated 0. When no hamming value appears at least
 * twice, the result is -1. Zero is a real hamming. When that sum is
 * 0, the result is 0. Null, empty, a path of 512 bytes or more, a
 * missing file, a non-regular file, or no valid pair returns -1.
 * Does not read the environment or write the learn cache. */
int ng_motive_ham_rsum(const char *path);
/* Sum of the least and the greatest winning labeled hammings on the
 * kept pairs of this path. A return of -1 from the labeled rank is
 * ignored. Every call at -1 returns -1. When the least and the
 * greatest are the same value, that value is added twice, including
 * a repeated 0. A single kept pair returns that hamming added twice.
 * Zero is a real hamming. When that sum is 0, the result is 0. Null,
 * empty, a path of 512 bytes or more, a missing file, a non-regular
 * file, or no valid pair returns -1. Does not read the environment
 * or write the learn cache. */
int ng_motive_ham_esum(const char *path);
/* Sum of the least and the greatest labeled margins on the kept
 * pairs of this path. A return of -1 is ignored. Every call at -1
 * returns -1. When the least and the greatest are the same value,
 * that value is added twice, including a repeated 0. A single kept
 * pair returns that margin added twice. Zero is a real margin. When
 * that sum is 0, the result is 0. Null, empty, a path of 512 bytes
 * or more, a missing file, a non-regular file, or no valid pair
 * returns -1. Does not read the environment or write the learn cache. */
int ng_motive_margin_esum(const char *path);
/* Sum of the labeled margins strictly greater than the least kept
 * margin and strictly less than the greatest kept margin on this
 * path. A return of -1 is ignored. Every call at -1 returns -1. A
 * margin equal to the least adds nothing. A margin equal to the
 * greatest adds nothing. A repeated interior margin is added again.
 * When no kept margin is strictly inside the extremes, the result
 * is -1. A uniform file returns -1, including when every labeled
 * margin is 0. A file with exactly two distinct margins returns -1.
 * A single kept pair returns -1. Zero is a real margin. Zero is
 * never strictly greater than the least nonnegative margin. When
 * interior margins exist and their sum is 0, the result is 0. Null,
 * empty, a path of 512 bytes or more, a missing file, a non-regular
 * file, or no valid pair returns -1. Does not read the environment
 * or write the learn cache. */
int ng_motive_margin_xsum(const char *path);
/* Sum of the winning labeled hammings strictly greater than the least
 * kept hamming and strictly less than the greatest kept hamming on
 * this path. A return of -1 from the labeled rank is ignored. Every
 * call at -1 returns -1. A hamming equal to the least adds nothing.
 * A hamming equal to the greatest adds nothing. A repeated interior
 * hamming is added again. When no kept hamming is strictly inside
 * the extremes, the result is -1. A uniform file returns -1, including
 * when every winning hamming is 0. A file with exactly two distinct
 * hammings returns -1. A single kept pair returns -1. Zero is a real
 * hamming. Zero is never strictly greater than the least nonnegative
 * hamming. When interior hammings exist and their sum is 0, the
 * result is 0. Null, empty, a path of 512 bytes or more, a missing
 * file, a non-regular file, or no valid pair returns -1. Does not
 * read the environment or write the learn cache. */
int ng_motive_ham_xsum(const char *path);
/* Count of the winning labeled hammings strictly greater than the
 * least kept hamming and strictly less than the greatest kept
 * hamming on this path. A return of -1 from the labeled rank is
 * ignored. Every call at -1 returns -1. A hamming equal to the least
 * adds nothing. A hamming equal to the greatest adds nothing. A
 * repeated interior hamming is counted again. When no kept hamming
 * is strictly inside the extremes, the result is -1. A uniform file
 * returns -1, including when every winning hamming is 0. A file with
 * exactly two distinct hammings returns -1. A single kept pair
 * returns -1. Zero is a real hamming. Zero is never strictly greater
 * than the least nonnegative hamming. An empty interior is -1, not
 * 0. Null, empty, a path of 512 bytes or more, a missing file, a
 * non-regular file, or no valid pair returns -1. Does not read the
 * environment or write the learn cache. */
int ng_motive_ham_mid(const char *path);
/* Count of the winning labeled hammings strictly less than the
 * greatest kept hamming on this path. A return of -1 from the labeled
 * rank is ignored. Every call at -1 returns -1. A hamming equal to
 * the greatest adds nothing. A hamming equal to the least counts,
 * including 0, when that hamming is strictly less than the greatest.
 * A repeated hamming below the greatest is counted again. When none
 * are strictly below the greatest, the result is -1. A uniform file
 * returns -1, including when every winning hamming is 0. A single
 * kept pair returns -1. Zero is a real hamming. When the greatest
 * kept hamming is 0, nothing is strictly below it, so the result is
 * -1. An empty below-count is -1, not 0. Null, empty, a path of 512
 * bytes or more, a missing file, a non-regular file, or no valid pair
 * returns -1. Does not read the environment or write the learn cache. */
int ng_motive_ham_under(const char *path);
/* Count of the winning labeled hammings strictly greater than the
 * least kept hamming on this path. A return of -1 from the labeled
 * rank is ignored. Every call at -1 returns -1. A hamming equal to
 * the least adds nothing, including 0. A hamming equal to the greatest
 * counts when it is strictly greater than the least. A repeated
 * hamming above the least is counted again. When none are strictly
 * greater than the least, the result is -1. A uniform file returns -1,
 * including when every winning hamming is 0. A single kept pair
 * returns -1. Zero is a real hamming. Zero is never strictly greater
 * than the least nonnegative hamming, so a 0 never adds to this count.
 * An empty above-count is -1, not 0. Null, empty, a path of 512 bytes
 * or more, a missing file, a non-regular file, or no valid pair
 * returns -1. Does not read the environment or write the learn cache. */
int ng_motive_ham_over(const char *path);
/* Count of kept pairs whose winning labeled hamming appears on exactly
 * one kept pair of this path. That count is also how many distinct
 * hammings have a count of exactly 1. A return of -1 from the labeled
 * rank is ignored. Every call at -1 returns -1. A hamming that appears
 * twice or more adds nothing, including a repeated 0. A hamming that
 * appears once counts, including 0. When no hamming appears once, the
 * result is -1, not 0. A single kept pair returns 1, including when
 * that hamming is 0. Two or more kept pairs that all share one hamming
 * return -1. A uniform file returns -1, including when every winning
 * hamming is 0. Zero is a real hamming. An empty once-count is -1,
 * not 0. Null, empty, a path of 512 bytes or more, a missing file, a
 * non-regular file, or no valid pair returns -1. Does not read the
 * environment or write the learn cache. */
int ng_motive_ham_ones(const char *path);
/* Count of kept pairs whose winning labeled hamming appears on at
 * least two kept pairs of this path. A return of -1 from the labeled
 * rank is ignored. Every call at -1 returns -1. A hamming that appears
 * once adds nothing, including a unique 0. A hamming that appears
 * twice or more counts once for every kept pair that has it, including
 * a repeated 0. When no kept pair has a hamming that appears at least
 * twice, the result is -1, not 0. A single kept pair returns -1,
 * including when that hamming is 0. Two or more kept pairs that all
 * share one hamming return the number of those pairs. A uniform file
 * of two or more pairs returns that pair count, including when every
 * winning hamming is 0. A file where every winning hamming appears
 * once returns -1. Zero is a real hamming. An empty shared count is
 * -1, not 0. Null, empty, a path of 512 bytes or more, a missing file,
 * a non-regular file, or no valid pair returns -1. Does not read the
 * environment or write the learn cache. */
int ng_motive_ham_many(const char *path);
/* Count of distinct winning labeled hammings that appear on at least
 * two kept pairs of this path. A return of -1 from the labeled rank is
 * ignored. Every call at -1 returns -1. A hamming that appears once
 * adds nothing, including a unique 0. A hamming that appears twice or
 * more counts as one distinct value, including a repeated 0. When no
 * hamming appears at least twice, the result is -1, not 0. A single
 * kept pair returns -1, including when that hamming is 0. Two or more
 * kept pairs that all share one hamming return 1. A uniform file of
 * two or more pairs returns 1, including when every winning hamming is
 * 0. A file where every winning hamming appears once returns -1. Zero
 * is a real hamming. An empty shared-kind count is -1, not 0. Null,
 * empty, a path of 512 bytes or more, a missing file, a non-regular
 * file, or no valid pair returns -1. Does not read the environment or
 * write the learn cache. */
int ng_motive_ham_kmany(const char *path);
/* Lower median of the winning labeled hammings on the kept pairs of
 * this path. A return of -1 from the labeled rank is ignored. Every
 * call at -1 returns -1. A negative hamming is ignored. When no
 * nonnegative hamming is kept, the result is -1. Kept hammings,
 * including duplicates and 0, are sorted ascending. The result is the
 * value at index (count - 1) / 2. When the count is even, that index
 * is the smaller of the two central values. When the count is odd,
 * that index is the single central value. A single kept pair returns
 * that hamming, including 0. A uniform file returns that hamming,
 * including 0. Zero is a real hamming. The result is one kept hamming,
 * not a pair count and not a sum. Null, empty, a path of 512 bytes or
 * more, a missing file, a non-regular file, or no valid pair returns
 * -1. Does not read the environment or write the learn cache. */
int ng_motive_ham_med(const char *path);
/* Upper median of the winning labeled hammings on the kept pairs of
 * this path. A return of -1 from the labeled rank is ignored. Every
 * call at -1 returns -1. A negative hamming is ignored. When no
 * nonnegative hamming is kept, the result is -1. Kept hammings,
 * including duplicates and 0, are sorted ascending. The result is the
 * value at index count / 2. When the count is even, that index is the
 * larger of the two central values. When the count is odd, that index
 * is the single central value. A single kept pair returns that
 * hamming, including 0. A uniform file returns that hamming, including
 * 0. Zero is a real hamming. The result is one kept hamming, not a
 * pair count and not a sum. Null, empty, a path of 512 bytes or more,
 * a missing file, a non-regular file, or no valid pair returns -1.
 * Does not read the environment or write the learn cache. */
int ng_motive_ham_mup(const char *path);
/* Sum of the lower and upper central winning labeled hammings on the
 * kept pairs of this path. A return of -1 from the labeled rank is
 * ignored. Every call at -1 returns -1. A negative hamming is ignored.
 * When no nonnegative hamming is kept, the result is -1. Kept hammings,
 * including duplicates and 0, are sorted ascending. The lower index is
 * (count - 1) / 2. The upper index is count / 2. The result is the sum
 * of those two values. When the count is odd, both indexes are the same
 * and that central value is added twice, including a repeated 0. When
 * the count is even, both central values are added, including 0. A
 * single kept pair returns that hamming added twice, including 0. A
 * uniform file returns that hamming added twice, including 0. Zero is a
 * real hamming. The result is a sum of kept hammings, not a pair count.
 * Null, empty, a path of 512 bytes or more, a missing file, a
 * non-regular file, or no valid pair returns -1. Does not read the
 * environment or write the learn cache. */
int ng_motive_ham_csum(const char *path);
char *ng_motive_format(const ng_motive *m);
/* 1 when the act is a destructive plate (purge or protect). */
int ng_motive_blocks(const char *text);
char *ng_motive_json(const char *text);

#endif
