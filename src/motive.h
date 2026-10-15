#ifndef NANOBOT_MOTIVE_H
#define NANOBOT_MOTIVE_H

/* Hive motive. Hybrid rank of eight concepts plus a priority cascade.
 * Rank uses the caller stack: 192-byte norm, every token in that norm, no heap.
 * NANOBOT_MOTIVE_LEX_W is the lexical weight, 0-255. Unset stays 64.
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
/* over < 0 reads NANOBOT_MOTIVE_LEX_W. 0-255 applies to this call only. */
void ng_motive_rank_w(const char *text, ng_motive *out, int over);
char *ng_motive_format(const ng_motive *m);
/* 1 when the act is a destructive plate (purge or protect). */
int ng_motive_blocks(const char *text);
char *ng_motive_json(const char *text);

#endif
