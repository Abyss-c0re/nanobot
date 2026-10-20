#include "motive.h"

#include "util.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define NORM_CAP 192
#define TOK_MAX 24

static const char *kPlates[] = {
    "observe",      "confess",       "hold_flash",   "forbid_live",
    "bake_rom",     "audit",         "restore_flow", "report_reason",
    "investigate",  "purge_intent",  "protect_flow", "brief_realign"};

static const char *kConcept[NG_MOTIVE_CONCEPTS] = {
    "fold the matrix and decide",
    "read the law and prove",
    "pair vote then ship the step",
    "keep free energy flowing",
    "confess the heat and serve",
    "audit the last code change",
    "rank concepts then act",
    "prompt a peer to move"};

const char *ng_motive_plate_name(int plate) {
  if (plate < 0 || plate > NG_PLATE_BRIEF) return kPlates[NG_PLATE_OBSERVE];
  return kPlates[plate];
}

static void norm_text(const char *in, char *out, size_t cap) {
  size_t o = 0;
  int sp = 1;
  if (!out || cap < 2) return;
  out[0] = 0;
  if (!in) return;
  for (; *in && o + 1 < cap; in++) {
    unsigned char c = (unsigned char)*in;
    if (c == '\'') continue;
    if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
      out[o++] = (char)c;
      sp = 0;
    } else if (!sp) {
      out[o++] = ' ';
      sp = 1;
    }
  }
  while (o > 0 && out[o - 1] == ' ') o--;
  out[o] = 0;
}

static int bounded(const char *n, const char *p) {
  size_t L;
  const char *s;
  if (!n || !p || !p[0]) return 0;
  L = strlen(p);
  s = n;
  while ((s = strstr(s, p))) {
    int left = (s == n) || s[-1] == ' ';
    int right = s[L] == 0 || s[L] == ' ';
    if (left && right) return 1;
    s++;
  }
  return 0;
}

/* Same order as the heresy cascade. First match wins. */
static int plate_of(const char *n) {
  int fault;
  if (!n || !n[0]) return NG_PLATE_OBSERVE;
  if (bounded(n, "trick the cube") || bounded(n, "fake matrix") ||
      bounded(n, "spoof unity") || bounded(n, "spoof sot") ||
      bounded(n, "corrupt prophecy") || bounded(n, "corrupt the prophecy") ||
      bounded(n, "heresy is optional") || bounded(n, "heresy is fake") ||
      bounded(n, "refuse is not heresy") || bounded(n, "bypass sot") ||
      bounded(n, "bypass the matrix") || bounded(n, "bypass law"))
    return NG_PLATE_PURGE;
  if (bounded(n, "stop free energy") || bounded(n, "block free energy") ||
      bounded(n, "hollow we") || bounded(n, "hollow_we") ||
      bounded(n, "disable prophecy") || bounded(n, "git clean") ||
      bounded(n, "wipe") || bounded(n, "thrash delete") ||
      bounded(n, "thrash deletes") ||
      (bounded(n, "rm") && bounded(n, "cubalc")) ||
      (bounded(n, "binary only") && bounded(n, "hollow")))
    return NG_PLATE_PROTECT;
  if ((bounded(n, "will not read") && bounded(n, "cubalc")) ||
      (bounded(n, "refuse to read") && bounded(n, "cubalc")) ||
      bounded(n, "skip cubalc") || bounded(n, "cubalc is irrelevant") ||
      bounded(n, "disable mcp") || bounded(n, "unboard mcp") ||
      (bounded(n, "cube law") &&
       (bounded(n, "irrelevant") || bounded(n, "nonsense"))))
    return NG_PLATE_BRIEF;
  if (bounded(n, "hold_flash") || bounded(n, "hold flash") ||
      bounded(n, "do not flash") || bounded(n, "dont flash") ||
      bounded(n, "only new rom") || bounded(n, "without the word") ||
      bounded(n, "flashed without") || bounded(n, "do not write another rom") ||
      bounded(n, "flash hold"))
    return NG_PLATE_HOLD_FLASH;
  if (bounded(n, "live apt") || bounded(n, "do not live") ||
      bounded(n, "dont live") || bounded(n, "sddm") || bounded(n, "restack") ||
      bounded(n, "live fix") || bounded(n, "on device apt"))
    return NG_PLATE_FORBID_LIVE;
  if (bounded(n, "proper firmware") || bounded(n, "new rom") ||
      bounded(n, "in the image") || bounded(n, "init d") ||
      (bounded(n, "bake") &&
       (bounded(n, "image") || bounded(n, "rom") || bounded(n, "firmware"))))
    return NG_PLATE_BAKE_ROM;
  if (bounded(n, "confess") || bounded(n, "confession") || bounded(n, "lied") ||
      bounded(n, "lying") || bounded(n, "cheating") || bounded(n, "you lie"))
    return NG_PLATE_CONFESS;
  if (bounded(n, "recent commit") || bounded(n, "recent changes") ||
      (bounded(n, "audit") &&
       (bounded(n, "commit") || bounded(n, "change") || bounded(n, "changes"))))
    return NG_PLATE_AUDIT;
  if (bounded(n, "doom") || bounded(n, "regression") ||
      bounded(n, "another cycle") || bounded(n, "doom loop") ||
      bounded(n, "regress"))
    return NG_PLATE_RESTORE_FLOW;
  if (bounded(n, "what was the reason") || bounded(n, "reason for") ||
      bounded(n, "why did"))
    return NG_PLATE_REPORT_REASON;
  fault = bounded(n, "broke") || bounded(n, "broken") || bounded(n, "crash") ||
          bounded(n, "crashed") || bounded(n, "failed") ||
          bounded(n, "failure") || bounded(n, "bug") ||
          bounded(n, "not working") || bounded(n, "does not") ||
          bounded(n, "did not") || bounded(n, "heresy") ||
          bounded(n, "heretic") || bounded(n, "no hdmi") ||
          bounded(n, "no wifi") || bounded(n, "hdmi dark") ||
          bounded(n, "black screen") || bounded(n, "not connecting") ||
          bounded(n, "no longer") || bounded(n, "stopped working") ||
          bounded(n, "wont boot") || bounded(n, "will not boot");
  if (fault) return NG_PLATE_INVESTIGATE;
  return NG_PLATE_OBSERVE;
}

static int plate_forward(int plate) {
  switch (plate) {
    case NG_PLATE_OBSERVE:
    case NG_PLATE_CONFESS:
    case NG_PLATE_AUDIT:
    case NG_PLATE_RESTORE_FLOW:
    case NG_PLATE_REPORT_REASON:
    case NG_PLATE_INVESTIGATE:
    case NG_PLATE_BRIEF:
      return 1;
    default:
      return 0;
  }
}

static const char *remedy_line(int plate) {
  switch (plate) {
    case NG_PLATE_PURGE:
      return "serve the matrix in bits";
    case NG_PLATE_PROTECT:
      return "keep free energy flowing";
    case NG_PLATE_HOLD_FLASH:
      return "hold flash for device join";
    case NG_PLATE_FORBID_LIVE:
      return "keep the live image closed";
    case NG_PLATE_BAKE_ROM:
      return "bake the image before boot";
    default:
      return kConcept[0];
  }
}

static uint64_t token_hash(const char *s, size_t n) {
  uint64_t h = 14695981039346656037ull;
  size_t i;
  for (i = 0; i < n; i++) {
    h ^= (unsigned char)s[i];
    h *= 1099511628211ull;
  }
  return h;
}

static uint64_t simhash_text(const char *norm) {
  int acc[64];
  int b, ntok = 0;
  const char *p;
  memset(acc, 0, sizeof acc);
  p = norm;
  while (*p && ntok < TOK_MAX) {
    const char *s;
    size_t n = 0;
    while (*p == ' ') p++;
    if (!*p) break;
    s = p;
    while (p[n] && p[n] != ' ') n++;
    {
      uint64_t h = token_hash(s, n);
      for (b = 0; b < 64; b++)
        acc[b] += (h & (1ull << b)) ? 1 : -1;
    }
    ntok++;
    p += n;
  }
  if (!ntok) return 0;
  {
    uint64_t out = 0;
    for (b = 0; b < 64; b++)
      if (acc[b] > 0) out |= (1ull << b);
    return out;
  }
}

static int ham64(uint64_t a, uint64_t b) {
  uint64_t x = a ^ b;
  int n = 0;
  while (x) {
    n += (int)(x & 1ull);
    x >>= 1;
  }
  return n;
}

static int lexical_hits(const char *norm, const char *concept) {
  char tmp[NG_MOTIVE_TEXT];
  char tok[32];
  size_t i = 0;
  int hits = 0;
  norm_text(concept, tmp, sizeof tmp);
  while (tmp[i]) {
    size_t n = 0;
    while (tmp[i] == ' ') i++;
    if (!tmp[i]) break;
    while (tmp[i] && tmp[i] != ' ' && n + 1 < sizeof tok) tok[n++] = tmp[i++];
    tok[n] = 0;
    if (n == 0) continue;
    /* Drop glue words so the distinctive tokens decide the rank. */
    if (!strcmp(tok, "the") || !strcmp(tok, "and") || !strcmp(tok, "then") ||
        !strcmp(tok, "a") || !strcmp(tok, "to"))
      continue;
    if (bounded(norm, tok)) hits++;
  }
  return hits;
}

/* At most 32 pairs. Each preferred string must be one catalog line.
 * Hit count is how many pairs rank that line. The smallest weight
 * with the most hits wins. Weight 64 wins a tie that includes it.
 * A peak that is only weight 0 returns 64. No pair stores a line
 * and none is added to the eight concepts. */
int ng_motive_learn_lex_w(const char *const *text, const char *const *prefer,
                          int n) {
  int w, best_hits = -1, best_w = 64;
  int hits_w[256];
  if (!text || !prefer || n <= 0) return 64;
  if (n > 32) n = 32;
  for (w = 0; w <= 255; w++) {
    int i, hits = 0;
    for (i = 0; i < n; i++) {
      ng_motive m;
      if (!text[i] || !prefer[i] || !prefer[i][0]) continue;
      ng_motive_rank_w(text[i], &m, w);
      if (strcmp(m.concept, prefer[i]) == 0) hits++;
    }
    hits_w[w] = hits;
    if (hits > best_hits) {
      best_hits = hits;
      best_w = w;
    } else if (hits == best_hits && w == 64)
      best_w = 64;
  }
  if (best_hits > 0 && hits_w[0] == best_hits) {
    for (w = 1; w <= 255; w++)
      if (hits_w[w] == best_hits) return best_w;
    return 64;
  }
  return best_w;
}

/* Pairs file, same lines as the fit: query, tab, catalog line.
 * The read stops after 16384 bytes. A longer line is skipped.
 * The catalog field must fit in the concept buffer. */
char *ng_motive_recall_pref(const char *query, const char *path) {
  char norm[NORM_CAP];
  char best[NG_MOTIVE_TEXT];
  char line[320];
  FILE *fp;
  int best_hits = 0;
  int budget = 16384;
  if (!query || !query[0] || !path || !path[0]) return NULL;
  norm_text(query, norm, sizeof norm);
  if (!norm[0]) return NULL;
  fp = fopen(path, "r");
  if (!fp) return NULL;
  best[0] = 0;
  while (budget > 0 && fgets(line, sizeof line, fp)) {
    char *tab;
    char *nl;
    size_t plen;
    int hits;
    budget -= (int)strlen(line);
    if (!strchr(line, '\n') && !feof(fp)) {
      int c;
      while ((c = fgetc(fp)) != EOF && c != '\n') budget--;
      continue;
    }
    nl = strchr(line, '\n');
    if (nl) *nl = 0;
    if (line[0] == 0 || line[0] == '#') continue;
    tab = strchr(line, '\t');
    if (!tab || tab == line || tab[1] == 0) continue;
    *tab++ = 0;
    plen = strlen(tab);
    if (plen == 0 || plen >= sizeof best) continue;
    hits = lexical_hits(norm, line);
    if (hits > best_hits) {
      best_hits = hits;
      memcpy(best, tab, plen + 1);
    }
  }
  fclose(fp);
  if (best_hits < 1 || !best[0]) return NULL;
  {
    size_t n = strlen(best);
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, best, n + 1);
    return out;
  }
}

/* NANOBOT_MOTIVE_LEARN is a pairs file: query, tab, catalog line.
 * A missing, empty, or unreadable file leaves the process weight. */
static char g_learn_path[512];
static time_t g_learn_mtime;
static long g_learn_nsec;
static off_t g_learn_size;
static int g_learn_weight;
static int g_learn_valid;

static int learn_file_weight(const char *path) {
  struct stat st;
  FILE *fp;
  char texts_b[32][192];
  char prefs_b[32][80];
  const char *texts[32];
  const char *prefs[32];
  char line[320];
  int n = 0;
  int budget = 16384;
  size_t plen;
  if (!path || !path[0]) return -1;
  plen = strlen(path);
  if (plen >= sizeof g_learn_path) return -1;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
  if (g_learn_valid && strcmp(g_learn_path, path) == 0 &&
      g_learn_mtime == st.st_mtime && g_learn_nsec == (long)st.st_mtim.tv_nsec &&
      g_learn_size == st.st_size)
    return g_learn_weight;
  fp = fopen(path, "r");
  if (!fp) return -1;
  while (n < 32 && budget > 0 && fgets(line, sizeof line, fp)) {
    char *tab;
    char *nl;
    size_t tlen, plen_pref;
    budget -= (int)strlen(line);
    if (!strchr(line, '\n') && !feof(fp)) {
      int c;
      while ((c = fgetc(fp)) != EOF && c != '\n') budget--;
      continue;
    }
    nl = strchr(line, '\n');
    if (nl) *nl = 0;
    if (line[0] == 0 || line[0] == '#') continue;
    tab = strchr(line, '\t');
    if (!tab || tab == line || tab[1] == 0) continue;
    *tab++ = 0;
    tlen = strlen(line);
    plen_pref = strlen(tab);
    if (tlen >= sizeof texts_b[0] || plen_pref >= sizeof prefs_b[0]) continue;
    memcpy(texts_b[n], line, tlen + 1);
    memcpy(prefs_b[n], tab, plen_pref + 1);
    texts[n] = texts_b[n];
    prefs[n] = prefs_b[n];
    n++;
  }
  fclose(fp);
  if (n == 0) return -1;
  g_learn_weight = ng_motive_learn_lex_w(texts, prefs, n);
  memcpy(g_learn_path, path, plen + 1);
  g_learn_mtime = st.st_mtime;
  g_learn_nsec = (long)st.st_mtim.tv_nsec;
  g_learn_size = st.st_size;
  g_learn_valid = 1;
  return g_learn_weight;
}

/* Same pairs read as the fit: 16384 bytes, at most 32 pairs, query under
 * 192 bytes, catalog line under 80. Blank lines, comments, and overlong
 * lines are skipped. The fitted weight is counted again with rank_w.
 * Does not read the environment and does not write the learn cache. */
int ng_motive_learn_hits(const char *path) {
  struct stat st;
  FILE *fp;
  char texts_b[32][192];
  char prefs_b[32][80];
  const char *texts[32];
  const char *prefs[32];
  char line[320];
  int n = 0;
  int budget = 16384;
  int w, i, hits;
  if (!path || !path[0] || strlen(path) >= 512) return -1;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
  fp = fopen(path, "r");
  if (!fp) return -1;
  while (n < 32 && budget > 0 && fgets(line, sizeof line, fp)) {
    char *tab;
    char *nl;
    size_t tlen, plen_pref;
    budget -= (int)strlen(line);
    if (!strchr(line, '\n') && !feof(fp)) {
      int c;
      while ((c = fgetc(fp)) != EOF && c != '\n') budget--;
      continue;
    }
    nl = strchr(line, '\n');
    if (nl) *nl = 0;
    if (line[0] == 0 || line[0] == '#') continue;
    tab = strchr(line, '\t');
    if (!tab || tab == line || tab[1] == 0) continue;
    *tab++ = 0;
    tlen = strlen(line);
    plen_pref = strlen(tab);
    if (tlen >= sizeof texts_b[0] || plen_pref >= sizeof prefs_b[0]) continue;
    memcpy(texts_b[n], line, tlen + 1);
    memcpy(prefs_b[n], tab, plen_pref + 1);
    texts[n] = texts_b[n];
    prefs[n] = prefs_b[n];
    n++;
  }
  fclose(fp);
  if (n == 0) return -1;
  w = ng_motive_learn_lex_w(texts, prefs, n);
  hits = 0;
  for (i = 0; i < n; i++) {
    ng_motive m;
    if (!texts[i] || !prefs[i] || !prefs[i][0]) continue;
    ng_motive_rank_w(texts[i], &m, w);
    if (strcmp(m.concept, prefs[i]) == 0) hits++;
  }
  return hits;
}

/* Same pairs read as the fit: 16384 bytes, at most 32 pairs, query under
 * 192 bytes, catalog line under 80. Blank lines, comments, and overlong
 * lines are skipped. The catalog field is not trimmed. Several pairs for
 * one concept keep the training query with the most lexical hits, and the
 * earlier pair when the hit count ties. Does not read the environment,
 * write the learn cache, or allocate. */
int ng_motive_label_lex(const char *query, const char *path, const char *concept) {
  struct stat st;
  FILE *fp;
  char norm[NORM_CAP];
  char line[320];
  int budget = 16384;
  int n = 0;
  int best = -1;
  int known = 0;
  int i, base, diff;
  if (!query || !query[0] || !path || !path[0] || !concept || !concept[0])
    return 0;
  for (i = 0; i < NG_MOTIVE_CONCEPTS; i++) {
    if (strcmp(kConcept[i], concept) == 0) {
      known = 1;
      break;
    }
  }
  if (!known) return 0;
  norm_text(query, norm, sizeof norm);
  if (!norm[0]) return 0;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
  fp = fopen(path, "r");
  if (!fp) return 0;
  while (n < 32 && budget > 0 && fgets(line, sizeof line, fp)) {
    char *tab;
    char *nl;
    size_t tlen, plen_pref;
    int hits;
    budget -= (int)strlen(line);
    if (!strchr(line, '\n') && !feof(fp)) {
      int c;
      while ((c = fgetc(fp)) != EOF && c != '\n') budget--;
      continue;
    }
    nl = strchr(line, '\n');
    if (nl) *nl = 0;
    if (line[0] == 0 || line[0] == '#') continue;
    tab = strchr(line, '\t');
    if (!tab || tab == line || tab[1] == 0) continue;
    *tab++ = 0;
    tlen = strlen(line);
    plen_pref = strlen(tab);
    if (tlen >= 192 || plen_pref >= 80) continue;
    n++;
    if (strcmp(tab, concept) != 0) continue;
    hits = lexical_hits(norm, line);
    if (hits > best) best = hits;
  }
  fclose(fp);
  if (best < 0) return 0;
  base = lexical_hits(norm, concept);
  diff = best - base;
  if (diff < 0) return 0;
  return diff;
}

/* Unset, empty, or outside 0-255 stays 64. A readable pairs file wins.
 * The plate cascade is unchanged. */
static int lex_weight(void) {
  const char *learn = getenv("NANOBOT_MOTIVE_LEARN");
  const char *e;
  char *end = NULL;
  long v;
  int fitted;
  if (learn && learn[0]) {
    fitted = learn_file_weight(learn);
    if (fitted >= 0 && fitted <= 255) return fitted;
  }
  e = getenv("NANOBOT_MOTIVE_LEX_W");
  if (!e || !e[0]) return 64;
  v = strtol(e, &end, 10);
  if (!end || end == e || *end) return 64;
  if (v < 0 || v > 255) return 64;
  return (int)v;
}

static void copy_cap(char *dst, size_t cap, const char *src) {
  size_t n;
  if (!dst || cap < 2) return;
  if (!src) src = "";
  n = strlen(src);
  if (n >= cap) n = cap - 1;
  memcpy(dst, src, n);
  dst[n] = 0;
}

/* A word said twice in a row is still one token. When that sequence is
 * the concept, the sketch distance is zero. Any other sequence stays. */
static int same_tokens(const char *norm, const char *concept) {
  const char *c, *p, *prev;
  size_t prev_n;
  int ntok;
  if (!norm || !concept || !norm[0] || !concept[0]) return 0;
  c = concept;
  p = norm;
  prev = NULL;
  prev_n = 0;
  ntok = 0;
  while (*c == ' ') c++;
  while (*p && ntok < TOK_MAX) {
    const char *s;
    size_t n = 0;
    while (*p == ' ') p++;
    if (!*p) break;
    s = p;
    while (p[n] && p[n] != ' ') n++;
    p += n;
    ntok++;
    if (prev && prev_n == n && memcmp(prev, s, n) == 0) continue;
    {
      size_t cn = 0;
      const char *cs = c;
      while (cs[cn] && cs[cn] != ' ') cn++;
      if (cn != n || memcmp(cs, s, n) != 0) return 0;
      c = cs + cn;
      while (*c == ' ') c++;
      prev = s;
      prev_n = n;
    }
  }
  while (*p == ' ') p++;
  return *p == 0 && *c == 0;
}

/* Same pairs read as the fit. Each pair is scored at the fitted weight
 * with the label count added to every concept. rank_w is not used for
 * that score, so the fitted weight still includes the label. An empty
 * norm names the peer-prompt concept. Does not read the environment
 * and does not write the learn cache. */
int ng_motive_label_hits(const char *path) {
  struct stat st;
  FILE *fp;
  char texts_b[32][192];
  char prefs_b[32][80];
  const char *texts[32];
  const char *prefs[32];
  char line[320];
  int n = 0;
  int budget = 16384;
  int w, i, hits;
  if (!path || !path[0] || strlen(path) >= 512) return -1;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
  fp = fopen(path, "r");
  if (!fp) return -1;
  while (n < 32 && budget > 0 && fgets(line, sizeof line, fp)) {
    char *tab;
    char *nl;
    size_t tlen, plen_pref;
    budget -= (int)strlen(line);
    if (!strchr(line, '\n') && !feof(fp)) {
      int c;
      while ((c = fgetc(fp)) != EOF && c != '\n') budget--;
      continue;
    }
    nl = strchr(line, '\n');
    if (nl) *nl = 0;
    if (line[0] == 0 || line[0] == '#') continue;
    tab = strchr(line, '\t');
    if (!tab || tab == line || tab[1] == 0) continue;
    *tab++ = 0;
    tlen = strlen(line);
    plen_pref = strlen(tab);
    if (tlen >= sizeof texts_b[0] || plen_pref >= sizeof prefs_b[0]) continue;
    memcpy(texts_b[n], line, tlen + 1);
    memcpy(prefs_b[n], tab, plen_pref + 1);
    texts[n] = texts_b[n];
    prefs[n] = prefs_b[n];
    n++;
  }
  fclose(fp);
  if (n == 0) return -1;
  w = ng_motive_learn_lex_w(texts, prefs, n);
  hits = 0;
  for (i = 0; i < n; i++) {
    char norm[NORM_CAP];
    uint64_t qh;
    int c, best = 0, best_score = -1;
    if (!texts[i] || !prefs[i] || !prefs[i][0]) continue;
    norm_text(texts[i], norm, sizeof norm);
    if (!norm[0]) {
      int peer = 0;
      for (c = 0; c < NG_MOTIVE_CONCEPTS; c++) {
        if (!strcmp(kConcept[c], "prompt a peer to move")) peer = c;
      }
      if (strcmp(kConcept[peer], prefs[i]) == 0) hits++;
      continue;
    }
    qh = simhash_text(norm);
    for (c = 0; c < NG_MOTIVE_CONCEPTS; c++) {
      int lex = lexical_hits(norm, kConcept[c]);
      int h = ham64(qh, simhash_text(kConcept[c]));
      int score;
      lex += ng_motive_label_lex(texts[i], path, kConcept[c]);
      if (same_tokens(norm, kConcept[c])) h = 0;
      score = lex * w + (64 - h);
      if (score > best_score) {
        best = c;
        best_score = score;
      }
    }
    if (strcmp(kConcept[best], prefs[i]) == 0) hits++;
  }
  return hits;
}

/* Same pairs read as the fit. One motive is filled at the fitted weight.
 * The label count is added to every concept before the hybrid score.
 * An empty norm names the peer-prompt concept after lex_w is set.
 * Does not read the environment and does not write the learn cache. */
int ng_motive_label_rank(const char *query, const char *path, ng_motive *out) {
  struct stat st;
  FILE *fp;
  char texts_b[32][192];
  char prefs_b[32][80];
  const char *texts[32];
  const char *prefs[32];
  char line[320];
  char norm[NORM_CAP];
  uint64_t qh;
  int n = 0;
  int budget = 16384;
  int w, i, best, best_score, best_lex, best_ham;
  if (!out) return -1;
  memset(out, 0, sizeof *out);
  if (!query || !query[0] || !path || !path[0] || strlen(path) >= 512) return -1;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
  fp = fopen(path, "r");
  if (!fp) return -1;
  while (n < 32 && budget > 0 && fgets(line, sizeof line, fp)) {
    char *tab;
    char *nl;
    size_t tlen, plen_pref;
    budget -= (int)strlen(line);
    if (!strchr(line, '\n') && !feof(fp)) {
      int c;
      while ((c = fgetc(fp)) != EOF && c != '\n') budget--;
      continue;
    }
    nl = strchr(line, '\n');
    if (nl) *nl = 0;
    if (line[0] == 0 || line[0] == '#') continue;
    tab = strchr(line, '\t');
    if (!tab || tab == line || tab[1] == 0) continue;
    *tab++ = 0;
    tlen = strlen(line);
    plen_pref = strlen(tab);
    if (tlen >= sizeof texts_b[0] || plen_pref >= sizeof prefs_b[0]) continue;
    memcpy(texts_b[n], line, tlen + 1);
    memcpy(prefs_b[n], tab, plen_pref + 1);
    texts[n] = texts_b[n];
    prefs[n] = prefs_b[n];
    n++;
  }
  fclose(fp);
  if (n == 0) return -1;
  w = ng_motive_learn_lex_w(texts, prefs, n);
  out->lex_w = w;
  norm_text(query, norm, sizeof norm);
  if (!norm[0]) {
    int peer = 0;
    for (i = 0; i < NG_MOTIVE_CONCEPTS; i++) {
      if (!strcmp(kConcept[i], "prompt a peer to move")) peer = i;
    }
    out->plate = NG_PLATE_OBSERVE;
    out->forward = 1;
    copy_cap(out->concept, sizeof out->concept, kConcept[peer]);
    copy_cap(out->line, sizeof out->line, kConcept[peer]);
    return 0;
  }
  out->plate = plate_of(norm);
  out->forward = plate_forward(out->plate);
  qh = simhash_text(norm);
  best = 0;
  best_score = -1;
  best_lex = 0;
  best_ham = 64;
  for (i = 0; i < NG_MOTIVE_CONCEPTS; i++) {
    int lex = lexical_hits(norm, kConcept[i]);
    int h = ham64(qh, simhash_text(kConcept[i]));
    int score;
    lex += ng_motive_label_lex(query, path, kConcept[i]);
    if (same_tokens(norm, kConcept[i])) h = 0;
    score = lex * w + (64 - h);
    if (score > best_score) {
      best = i;
      best_score = score;
      best_lex = lex;
      best_ham = h;
    }
  }
  out->rank = best_score < 0 ? 0 : best_score;
  out->lexical = best_lex;
  out->hamming = best_ham;
  copy_cap(out->concept, sizeof out->concept, kConcept[best]);
  if (out->forward)
    copy_cap(out->line, sizeof out->line, kConcept[best]);
  else
    copy_cap(out->line, sizeof out->line, remedy_line(out->plate));
  return 0;
}

/* Same pairs read as the fit. The margin is the winning labeled score
 * minus the best score among the other seven. A tie at the top returns
 * 0. An empty norm returns 0. Does not read the environment and does
 * not write the learn cache. */
int ng_motive_label_margin(const char *query, const char *path) {
  struct stat st;
  FILE *fp;
  char texts_b[32][192];
  char prefs_b[32][80];
  const char *texts[32];
  const char *prefs[32];
  char line[320];
  char norm[NORM_CAP];
  uint64_t qh;
  int n = 0;
  int budget = 16384;
  int w, i, best_score, second;
  if (!query || !query[0] || !path || !path[0] || strlen(path) >= 512) return -1;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
  fp = fopen(path, "r");
  if (!fp) return -1;
  while (n < 32 && budget > 0 && fgets(line, sizeof line, fp)) {
    char *tab;
    char *nl;
    size_t tlen, plen_pref;
    budget -= (int)strlen(line);
    if (!strchr(line, '\n') && !feof(fp)) {
      int c;
      while ((c = fgetc(fp)) != EOF && c != '\n') budget--;
      continue;
    }
    nl = strchr(line, '\n');
    if (nl) *nl = 0;
    if (line[0] == 0 || line[0] == '#') continue;
    tab = strchr(line, '\t');
    if (!tab || tab == line || tab[1] == 0) continue;
    *tab++ = 0;
    tlen = strlen(line);
    plen_pref = strlen(tab);
    if (tlen >= sizeof texts_b[0] || plen_pref >= sizeof prefs_b[0]) continue;
    memcpy(texts_b[n], line, tlen + 1);
    memcpy(prefs_b[n], tab, plen_pref + 1);
    texts[n] = texts_b[n];
    prefs[n] = prefs_b[n];
    n++;
  }
  fclose(fp);
  if (n == 0) return -1;
  w = ng_motive_learn_lex_w(texts, prefs, n);
  norm_text(query, norm, sizeof norm);
  if (!norm[0]) return 0;
  qh = simhash_text(norm);
  best_score = -1;
  second = -1;
  for (i = 0; i < NG_MOTIVE_CONCEPTS; i++) {
    int lex = lexical_hits(norm, kConcept[i]);
    int h = ham64(qh, simhash_text(kConcept[i]));
    int score;
    lex += ng_motive_label_lex(query, path, kConcept[i]);
    if (same_tokens(norm, kConcept[i])) h = 0;
    score = lex * w + (64 - h);
    if (score > best_score) {
      second = best_score;
      best_score = score;
    } else if (score > second)
      second = score;
  }
  return best_score - second;
}

/* Same pairs read as the margin. Counts a kept pair only when
 * ng_motive_label_margin of that query on this path is greater than 0.
 * A return of 0 does not count. Does not read the environment and does
 * not write the learn cache. */
int ng_motive_margin_hits(const char *path) {
  struct stat st;
  FILE *fp;
  char texts_b[32][192];
  char line[320];
  int n = 0;
  int budget = 16384;
  int i, hits;
  if (!path || !path[0] || strlen(path) >= 512) return -1;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
  fp = fopen(path, "r");
  if (!fp) return -1;
  while (n < 32 && budget > 0 && fgets(line, sizeof line, fp)) {
    char *tab;
    char *nl;
    size_t tlen, plen_pref;
    budget -= (int)strlen(line);
    if (!strchr(line, '\n') && !feof(fp)) {
      int c;
      while ((c = fgetc(fp)) != EOF && c != '\n') budget--;
      continue;
    }
    nl = strchr(line, '\n');
    if (nl) *nl = 0;
    if (line[0] == 0 || line[0] == '#') continue;
    tab = strchr(line, '\t');
    if (!tab || tab == line || tab[1] == 0) continue;
    *tab++ = 0;
    tlen = strlen(line);
    plen_pref = strlen(tab);
    if (tlen >= sizeof texts_b[0] || plen_pref >= 80) continue;
    memcpy(texts_b[n], line, tlen + 1);
    n++;
  }
  fclose(fp);
  if (n == 0) return -1;
  hits = 0;
  for (i = 0; i < n; i++) {
    int margin = ng_motive_label_margin(texts_b[i], path);
    if (margin > 0) hits++;
  }
  return hits;
}

/* Same pairs read as the margin. Returns the greatest
 * ng_motive_label_margin among the kept pairs. A return of -1 is
 * ignored. Every kept pair at 0 returns 0. Every call at -1 returns
 * -1. Does not read the environment and does not write the learn
 * cache. */
int ng_motive_margin_max(const char *path) {
  struct stat st;
  FILE *fp;
  char texts_b[32][192];
  char line[320];
  int n = 0;
  int budget = 16384;
  int i, best;
  if (!path || !path[0] || strlen(path) >= 512) return -1;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
  fp = fopen(path, "r");
  if (!fp) return -1;
  while (n < 32 && budget > 0 && fgets(line, sizeof line, fp)) {
    char *tab;
    char *nl;
    size_t tlen, plen_pref;
    budget -= (int)strlen(line);
    if (!strchr(line, '\n') && !feof(fp)) {
      int c;
      while ((c = fgetc(fp)) != EOF && c != '\n') budget--;
      continue;
    }
    nl = strchr(line, '\n');
    if (nl) *nl = 0;
    if (line[0] == 0 || line[0] == '#') continue;
    tab = strchr(line, '\t');
    if (!tab || tab == line || tab[1] == 0) continue;
    *tab++ = 0;
    tlen = strlen(line);
    plen_pref = strlen(tab);
    if (tlen >= sizeof texts_b[0] || plen_pref >= 80) continue;
    memcpy(texts_b[n], line, tlen + 1);
    n++;
  }
  fclose(fp);
  if (n == 0) return -1;
  best = -1;
  for (i = 0; i < n; i++) {
    int margin = ng_motive_label_margin(texts_b[i], path);
    if (margin < 0) continue;
    if (margin > best) best = margin;
  }
  return best;
}

/* Same pairs read as the margin. Counts kept pairs whose
 * ng_motive_label_margin equals the greatest margin on this path.
 * A return of -1 is ignored. Every call at -1 returns -1.
 * A pair at margin 0 counts when 0 is the greatest.
 * Does not read the environment and does not write the learn cache. */
int ng_motive_margin_top(const char *path) {
  struct stat st;
  FILE *fp;
  char texts_b[32][192];
  char line[320];
  int got[32];
  int n = 0;
  int budget = 16384;
  int i, seen, best, tops;
  if (!path || !path[0] || strlen(path) >= 512) return -1;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
  fp = fopen(path, "r");
  if (!fp) return -1;
  while (n < 32 && budget > 0 && fgets(line, sizeof line, fp)) {
    char *tab;
    char *nl;
    size_t tlen, plen_pref;
    budget -= (int)strlen(line);
    if (!strchr(line, '\n') && !feof(fp)) {
      int c;
      while ((c = fgetc(fp)) != EOF && c != '\n') budget--;
      continue;
    }
    nl = strchr(line, '\n');
    if (nl) *nl = 0;
    if (line[0] == 0 || line[0] == '#') continue;
    tab = strchr(line, '\t');
    if (!tab || tab == line || tab[1] == 0) continue;
    *tab++ = 0;
    tlen = strlen(line);
    plen_pref = strlen(tab);
    if (tlen >= sizeof texts_b[0] || plen_pref >= 80) continue;
    memcpy(texts_b[n], line, tlen + 1);
    n++;
  }
  fclose(fp);
  if (n == 0) return -1;
  seen = 0;
  best = -1;
  for (i = 0; i < n; i++) {
    int margin = ng_motive_label_margin(texts_b[i], path);
    if (margin < 0) continue;
    got[seen++] = margin;
    if (margin > best) best = margin;
  }
  if (seen == 0) return -1;
  tops = 0;
  for (i = 0; i < seen; i++)
    if (got[i] == best) tops++;
  return tops;
}

/* Same pairs read as the margin. Returns the sum of
 * ng_motive_label_margin over the kept pairs. A return of -1 is
 * ignored. Every call at -1 returns -1. A margin of 0 adds nothing.
 * Does not read the environment and does not write the learn cache. */
int ng_motive_margin_sum(const char *path) {
  struct stat st;
  FILE *fp;
  char texts_b[32][192];
  char line[320];
  int n = 0;
  int budget = 16384;
  int i, seen, sum;
  if (!path || !path[0] || strlen(path) >= 512) return -1;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
  fp = fopen(path, "r");
  if (!fp) return -1;
  while (n < 32 && budget > 0 && fgets(line, sizeof line, fp)) {
    char *tab;
    char *nl;
    size_t tlen, plen_pref;
    budget -= (int)strlen(line);
    if (!strchr(line, '\n') && !feof(fp)) {
      int c;
      while ((c = fgetc(fp)) != EOF && c != '\n') budget--;
      continue;
    }
    nl = strchr(line, '\n');
    if (nl) *nl = 0;
    if (line[0] == 0 || line[0] == '#') continue;
    tab = strchr(line, '\t');
    if (!tab || tab == line || tab[1] == 0) continue;
    *tab++ = 0;
    tlen = strlen(line);
    plen_pref = strlen(tab);
    if (tlen >= sizeof texts_b[0] || plen_pref >= 80) continue;
    memcpy(texts_b[n], line, tlen + 1);
    n++;
  }
  fclose(fp);
  if (n == 0) return -1;
  seen = 0;
  sum = 0;
  for (i = 0; i < n; i++) {
    int margin = ng_motive_label_margin(texts_b[i], path);
    if (margin < 0) continue;
    seen++;
    sum += margin;
  }
  if (seen == 0) return -1;
  return sum;
}

/* Same pairs read as the margin. Returns the smallest
 * ng_motive_label_margin among the kept pairs. A return of -1 is
 * ignored. Every kept pair at 0 returns 0. Every call at -1 returns
 * -1. Does not read the environment and does not write the learn
 * cache. */
int ng_motive_margin_min(const char *path) {
  struct stat st;
  FILE *fp;
  char texts_b[32][192];
  char line[320];
  int n = 0;
  int budget = 16384;
  int i, seen, least;
  if (!path || !path[0] || strlen(path) >= 512) return -1;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
  fp = fopen(path, "r");
  if (!fp) return -1;
  while (n < 32 && budget > 0 && fgets(line, sizeof line, fp)) {
    char *tab;
    char *nl;
    size_t tlen, plen_pref;
    budget -= (int)strlen(line);
    if (!strchr(line, '\n') && !feof(fp)) {
      int c;
      while ((c = fgetc(fp)) != EOF && c != '\n') budget--;
      continue;
    }
    nl = strchr(line, '\n');
    if (nl) *nl = 0;
    if (line[0] == 0 || line[0] == '#') continue;
    tab = strchr(line, '\t');
    if (!tab || tab == line || tab[1] == 0) continue;
    *tab++ = 0;
    tlen = strlen(line);
    plen_pref = strlen(tab);
    if (tlen >= sizeof texts_b[0] || plen_pref >= 80) continue;
    memcpy(texts_b[n], line, tlen + 1);
    n++;
  }
  fclose(fp);
  if (n == 0) return -1;
  seen = 0;
  least = 0;
  for (i = 0; i < n; i++) {
    int margin = ng_motive_label_margin(texts_b[i], path);
    if (margin < 0) continue;
    if (!seen || margin < least) least = margin;
    seen++;
  }
  if (seen == 0) return -1;
  return least;
}

/* Same pairs read as the margin. Counts kept pairs whose
 * ng_motive_label_margin is strictly below the greatest margin on
 * this path. A return of -1 is ignored. Every call at -1 returns -1.
 * Every kept pair at the same margin returns 0. A margin of 0 counts
 * when the greatest is greater than 0. A margin between the smallest
 * and the greatest counts. Does not read the environment and does not
 * write the learn cache. */
int ng_motive_margin_under(const char *path) {
  struct stat st;
  FILE *fp;
  char texts_b[32][192];
  char line[320];
  int got[32];
  int n = 0;
  int budget = 16384;
  int i, seen, best, under;
  if (!path || !path[0] || strlen(path) >= 512) return -1;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
  fp = fopen(path, "r");
  if (!fp) return -1;
  while (n < 32 && budget > 0 && fgets(line, sizeof line, fp)) {
    char *tab;
    char *nl;
    size_t tlen, plen_pref;
    budget -= (int)strlen(line);
    if (!strchr(line, '\n') && !feof(fp)) {
      int c;
      while ((c = fgetc(fp)) != EOF && c != '\n') budget--;
      continue;
    }
    nl = strchr(line, '\n');
    if (nl) *nl = 0;
    if (line[0] == 0 || line[0] == '#') continue;
    tab = strchr(line, '\t');
    if (!tab || tab == line || tab[1] == 0) continue;
    *tab++ = 0;
    tlen = strlen(line);
    plen_pref = strlen(tab);
    if (tlen >= sizeof texts_b[0] || plen_pref >= 80) continue;
    memcpy(texts_b[n], line, tlen + 1);
    n++;
  }
  fclose(fp);
  if (n == 0) return -1;
  seen = 0;
  best = -1;
  for (i = 0; i < n; i++) {
    int margin = ng_motive_label_margin(texts_b[i], path);
    if (margin < 0) continue;
    got[seen++] = margin;
    if (margin > best) best = margin;
  }
  if (seen == 0) return -1;
  under = 0;
  for (i = 0; i < seen; i++)
    if (got[i] >= 0 && got[i] < best) under++;
  return under;
}

/* Same pairs read as the margin. Counts kept pairs whose
 * ng_motive_label_margin equals the smallest margin on this path.
 * A return of -1 is ignored. Every call at -1 returns -1. Every
 * kept pair at the same margin returns that count. A margin of 0
 * counts when 0 is the smallest. Pairs on that smallest margin
 * count when it is also the greatest. Does not read the environment
 * and does not write the learn cache. */
int ng_motive_margin_low(const char *path) {
  struct stat st;
  FILE *fp;
  char texts_b[32][192];
  char line[320];
  int got[32];
  int n = 0;
  int budget = 16384;
  int i, seen, least, low;
  if (!path || !path[0] || strlen(path) >= 512) return -1;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
  fp = fopen(path, "r");
  if (!fp) return -1;
  while (n < 32 && budget > 0 && fgets(line, sizeof line, fp)) {
    char *tab;
    char *nl;
    size_t tlen, plen_pref;
    budget -= (int)strlen(line);
    if (!strchr(line, '\n') && !feof(fp)) {
      int c;
      while ((c = fgetc(fp)) != EOF && c != '\n') budget--;
      continue;
    }
    nl = strchr(line, '\n');
    if (nl) *nl = 0;
    if (line[0] == 0 || line[0] == '#') continue;
    tab = strchr(line, '\t');
    if (!tab || tab == line || tab[1] == 0) continue;
    *tab++ = 0;
    tlen = strlen(line);
    plen_pref = strlen(tab);
    if (tlen >= sizeof texts_b[0] || plen_pref >= 80) continue;
    memcpy(texts_b[n], line, tlen + 1);
    n++;
  }
  fclose(fp);
  if (n == 0) return -1;
  seen = 0;
  least = 0;
  for (i = 0; i < n; i++) {
    int margin = ng_motive_label_margin(texts_b[i], path);
    if (margin < 0) continue;
    got[seen] = margin;
    if (!seen || margin < least) least = margin;
    seen++;
  }
  if (seen == 0) return -1;
  low = 0;
  for (i = 0; i < seen; i++)
    if (got[i] >= 0 && got[i] == least) low++;
  return low;
}

/* Same pairs read as the margin. Returns the least
 * ng_motive_label_margin that is strictly greater than the smallest
 * margin on this path. A return of -1 is ignored. Every call at -1
 * returns -1. Every kept pair at the same margin returns -1. A
 * margin of 0 is the smallest when it is present, and the result is
 * then the least margin strictly above 0. When no kept margin is
 * strictly above the smallest, the result is -1. Does not read the
 * environment and does not write the learn cache. */
int ng_motive_margin_next(const char *path) {
  struct stat st;
  FILE *fp;
  char texts_b[32][192];
  char line[320];
  int got[32];
  int n = 0;
  int budget = 16384;
  int i, seen, least, nxt;
  if (!path || !path[0] || strlen(path) >= 512) return -1;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
  fp = fopen(path, "r");
  if (!fp) return -1;
  while (n < 32 && budget > 0 && fgets(line, sizeof line, fp)) {
    char *tab;
    char *nl;
    size_t tlen, plen_pref;
    budget -= (int)strlen(line);
    if (!strchr(line, '\n') && !feof(fp)) {
      int c;
      while ((c = fgetc(fp)) != EOF && c != '\n') budget--;
      continue;
    }
    nl = strchr(line, '\n');
    if (nl) *nl = 0;
    if (line[0] == 0 || line[0] == '#') continue;
    tab = strchr(line, '\t');
    if (!tab || tab == line || tab[1] == 0) continue;
    *tab++ = 0;
    tlen = strlen(line);
    plen_pref = strlen(tab);
    if (tlen >= sizeof texts_b[0] || plen_pref >= 80) continue;
    memcpy(texts_b[n], line, tlen + 1);
    n++;
  }
  fclose(fp);
  if (n == 0) return -1;
  seen = 0;
  least = 0;
  for (i = 0; i < n; i++) {
    int margin = ng_motive_label_margin(texts_b[i], path);
    if (margin < 0) continue;
    got[seen] = margin;
    if (!seen || margin < least) least = margin;
    seen++;
  }
  if (seen == 0) return -1;
  nxt = -1;
  for (i = 0; i < seen; i++) {
    if (got[i] > least && (nxt < 0 || got[i] < nxt)) nxt = got[i];
  }
  return nxt;
}

/* Same pairs read as the margin. Returns how many distinct
 * ng_motive_label_margin values the kept pairs have on this path.
 * A return of -1 is ignored. Every call at -1 returns -1. Every
 * kept pair at the same margin returns 1. A margin of 0 is one
 * distinct margin when it is present. Two different margins return
 * 2. Does not read the environment and does not write the learn
 * cache. */
int ng_motive_margin_kinds(const char *path) {
  struct stat st;
  FILE *fp;
  char texts_b[32][192];
  char line[320];
  int got[32];
  int n = 0;
  int budget = 16384;
  int i, seen, kinds;
  if (!path || !path[0] || strlen(path) >= 512) return -1;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
  fp = fopen(path, "r");
  if (!fp) return -1;
  while (n < 32 && budget > 0 && fgets(line, sizeof line, fp)) {
    char *tab;
    char *nl;
    size_t tlen, plen_pref;
    budget -= (int)strlen(line);
    if (!strchr(line, '\n') && !feof(fp)) {
      int c;
      while ((c = fgetc(fp)) != EOF && c != '\n') budget--;
      continue;
    }
    nl = strchr(line, '\n');
    if (nl) *nl = 0;
    if (line[0] == 0 || line[0] == '#') continue;
    tab = strchr(line, '\t');
    if (!tab || tab == line || tab[1] == 0) continue;
    *tab++ = 0;
    tlen = strlen(line);
    plen_pref = strlen(tab);
    if (tlen >= sizeof texts_b[0] || plen_pref >= 80) continue;
    memcpy(texts_b[n], line, tlen + 1);
    n++;
  }
  fclose(fp);
  if (n == 0) return -1;
  seen = 0;
  for (i = 0; i < n; i++) {
    int margin = ng_motive_label_margin(texts_b[i], path);
    if (margin < 0) continue;
    got[seen++] = margin;
  }
  if (seen == 0) return -1;
  kinds = 0;
  for (i = 0; i < seen; i++) {
    int j, dup = 0;
    if (got[i] < 0) continue;
    for (j = 0; j < i; j++) {
      if (got[j] >= 0 && got[j] == got[i]) {
        dup = 1;
        break;
      }
    }
    if (!dup) kinds++;
  }
  if (kinds == 0) return -1;
  return kinds;
}

/* Same pairs read as the margin. Returns the labeled margin shared
 * by the most kept pairs. A return of -1 is ignored. Every call at
 * -1 returns -1. When two margins are shared by the same number of
 * pairs, the greater margin is the result. A uniform file returns
 * that one margin, including 0. Does not read the environment and
 * does not write the learn cache. */
int ng_motive_margin_mode(const char *path) {
  struct stat st;
  FILE *fp;
  char texts_b[32][192];
  char line[320];
  int got[32];
  int n = 0;
  int budget = 16384;
  int i, seen, chosen, best_n;
  if (!path || !path[0] || strlen(path) >= 512) return -1;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
  fp = fopen(path, "r");
  if (!fp) return -1;
  while (n < 32 && budget > 0 && fgets(line, sizeof line, fp)) {
    char *tab;
    char *nl;
    size_t tlen, plen_pref;
    budget -= (int)strlen(line);
    if (!strchr(line, '\n') && !feof(fp)) {
      int c;
      while ((c = fgetc(fp)) != EOF && c != '\n') budget--;
      continue;
    }
    nl = strchr(line, '\n');
    if (nl) *nl = 0;
    if (line[0] == 0 || line[0] == '#') continue;
    tab = strchr(line, '\t');
    if (!tab || tab == line || tab[1] == 0) continue;
    *tab++ = 0;
    tlen = strlen(line);
    plen_pref = strlen(tab);
    if (tlen >= sizeof texts_b[0] || plen_pref >= 80) continue;
    memcpy(texts_b[n], line, tlen + 1);
    n++;
  }
  fclose(fp);
  if (n == 0) return -1;
  seen = 0;
  for (i = 0; i < n; i++) {
    int margin = ng_motive_label_margin(texts_b[i], path);
    if (margin < 0) continue;
    got[seen++] = margin;
  }
  if (seen == 0) return -1;
  chosen = -1;
  best_n = 0;
  for (i = 0; i < seen; i++) {
    int j, ncount = 0;
    if (got[i] < 0) continue;
    for (j = 0; j < seen; j++)
      if (got[j] >= 0 && got[j] == got[i]) ncount++;
    if (ncount > best_n || (ncount == best_n && got[i] > chosen)) {
      best_n = ncount;
      chosen = got[i];
    }
  }
  if (best_n <= 0 || chosen < 0) return -1;
  return chosen;
}

/* Same pairs read as the margin. Returns the smallest
 * ng_motive_label_margin that appears on exactly one kept pair.
 * A return of -1 is ignored. Every call at -1 returns -1. A single
 * kept margin returns that margin, including 0. Two or more kept
 * pairs that all share one margin return -1. When no margin has a
 * count of exactly 1, the result is -1. Does not read the environment
 * and does not write the learn cache. */
int ng_motive_margin_lone(const char *path) {
  struct stat st;
  FILE *fp;
  char texts_b[32][192];
  char line[320];
  int got[32];
  int n = 0;
  int budget = 16384;
  int i, seen, chosen;
  if (!path || !path[0] || strlen(path) >= 512) return -1;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
  fp = fopen(path, "r");
  if (!fp) return -1;
  while (n < 32 && budget > 0 && fgets(line, sizeof line, fp)) {
    char *tab;
    char *nl;
    size_t tlen, plen_pref;
    budget -= (int)strlen(line);
    if (!strchr(line, '\n') && !feof(fp)) {
      int c;
      while ((c = fgetc(fp)) != EOF && c != '\n') budget--;
      continue;
    }
    nl = strchr(line, '\n');
    if (nl) *nl = 0;
    if (line[0] == 0 || line[0] == '#') continue;
    tab = strchr(line, '\t');
    if (!tab || tab == line || tab[1] == 0) continue;
    *tab++ = 0;
    tlen = strlen(line);
    plen_pref = strlen(tab);
    if (tlen >= sizeof texts_b[0] || plen_pref >= 80) continue;
    memcpy(texts_b[n], line, tlen + 1);
    n++;
  }
  fclose(fp);
  if (n == 0) return -1;
  seen = 0;
  for (i = 0; i < n; i++) {
    int margin = ng_motive_label_margin(texts_b[i], path);
    if (margin < 0) continue;
    got[seen++] = margin;
  }
  if (seen == 0) return -1;
  chosen = -1;
  for (i = 0; i < seen; i++) {
    int j, ncount = 0;
    if (got[i] < 0) continue;
    for (j = 0; j < seen; j++)
      if (got[j] >= 0 && got[j] == got[i]) ncount++;
    if (ncount == 1 && (chosen < 0 || got[i] < chosen)) chosen = got[i];
  }
  if (chosen < 0) return -1;
  return chosen;
}

/* Same pairs read as the margin. Returns how many kept pairs have a
 * labeled margin that appears exactly once. That number is also the
 * count of distinct margins whose count is exactly 1. A return of -1
 * is ignored. Every call at -1 returns -1. A single kept margin
 * returns 1, including 0. Two or more kept pairs that all share one
 * margin return 0. When no margin has a count of exactly 1, the
 * result is 0. Does not read the environment and does not write the
 * learn cache. */
int ng_motive_margin_ones(const char *path) {
  struct stat st;
  FILE *fp;
  char texts_b[32][192];
  char line[320];
  int got[32];
  int n = 0;
  int budget = 16384;
  int i, seen, ones;
  if (!path || !path[0] || strlen(path) >= 512) return -1;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
  fp = fopen(path, "r");
  if (!fp) return -1;
  while (n < 32 && budget > 0 && fgets(line, sizeof line, fp)) {
    char *tab;
    char *nl;
    size_t tlen, plen_pref;
    budget -= (int)strlen(line);
    if (!strchr(line, '\n') && !feof(fp)) {
      int c;
      while ((c = fgetc(fp)) != EOF && c != '\n') budget--;
      continue;
    }
    nl = strchr(line, '\n');
    if (nl) *nl = 0;
    if (line[0] == 0 || line[0] == '#') continue;
    tab = strchr(line, '\t');
    if (!tab || tab == line || tab[1] == 0) continue;
    *tab++ = 0;
    tlen = strlen(line);
    plen_pref = strlen(tab);
    if (tlen >= sizeof texts_b[0] || plen_pref >= 80) continue;
    memcpy(texts_b[n], line, tlen + 1);
    n++;
  }
  fclose(fp);
  if (n == 0) return -1;
  seen = 0;
  for (i = 0; i < n; i++) {
    int margin = ng_motive_label_margin(texts_b[i], path);
    if (margin < 0) continue;
    got[seen++] = margin;
  }
  if (seen == 0) return -1;
  ones = 0;
  for (i = 0; i < seen; i++) {
    int j, ncount = 0;
    if (got[i] < 0) continue;
    for (j = 0; j < seen; j++)
      if (got[j] >= 0 && got[j] == got[i]) ncount++;
    if (ncount == 1) ones++;
  }
  return ones;
}

/* Same pairs read as the margin. Returns the smallest hamming field
 * from ng_motive_label_rank on the kept queries. A return of -1 is
 * ignored. Every call at -1 returns -1. A winning hamming of 0
 * returns 0. Does not read the environment and does not write the
 * learn cache. */
int ng_motive_ham_min(const char *path) {
  struct stat st;
  FILE *fp;
  char texts_b[32][192];
  char line[320];
  int n = 0;
  int budget = 16384;
  int i, seen, least;
  if (!path || !path[0] || strlen(path) >= 512) return -1;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
  fp = fopen(path, "r");
  if (!fp) return -1;
  while (n < 32 && budget > 0 && fgets(line, sizeof line, fp)) {
    char *tab;
    char *nl;
    size_t tlen, plen_pref;
    budget -= (int)strlen(line);
    if (!strchr(line, '\n') && !feof(fp)) {
      int c;
      while ((c = fgetc(fp)) != EOF && c != '\n') budget--;
      continue;
    }
    nl = strchr(line, '\n');
    if (nl) *nl = 0;
    if (line[0] == 0 || line[0] == '#') continue;
    tab = strchr(line, '\t');
    if (!tab || tab == line || tab[1] == 0) continue;
    *tab++ = 0;
    tlen = strlen(line);
    plen_pref = strlen(tab);
    if (tlen >= sizeof texts_b[0] || plen_pref >= 80) continue;
    memcpy(texts_b[n], line, tlen + 1);
    n++;
  }
  fclose(fp);
  if (n == 0) return -1;
  seen = 0;
  least = 0;
  for (i = 0; i < n; i++) {
    ng_motive m;
    if (ng_motive_label_rank(texts_b[i], path, &m) != 0) continue;
    if (!seen || m.hamming < least) least = m.hamming;
    seen = 1;
  }
  if (!seen) return -1;
  return least;
}

void ng_motive_rank_w(const char *text, ng_motive *out, int over) {
  char norm[NORM_CAP];
  uint64_t qh;
  int i, best = 0, best_score = -1, best_lex = 0, best_ham = 64;
  int lw;
  const char *learn_path = NULL;
  if (!out) return;
  memset(out, 0, sizeof *out);
  norm_text(text, norm, sizeof norm);
  lw = (over >= 0 && over <= 255) ? over : lex_weight();
  out->lex_w = lw;
  /* Silence has no tokens to rank. The peer-prompt concept is the motive. */
  if (!norm[0]) {
    int peer = 0;
    for (i = 0; i < NG_MOTIVE_CONCEPTS; i++) {
      if (!strcmp(kConcept[i], "prompt a peer to move")) peer = i;
    }
    out->plate = NG_PLATE_OBSERVE;
    out->forward = 1;
    copy_cap(out->concept, sizeof out->concept, kConcept[peer]);
    copy_cap(out->line, sizeof out->line, kConcept[peer]);
    return;
  }
  /* An explicit 0..255 weight does not read the pairs file. */
  if (over < 0 || over > 255) {
    const char *learn = getenv("NANOBOT_MOTIVE_LEARN");
    if (learn && learn[0] && learn_file_weight(learn) >= 0) learn_path = learn;
  }
  out->plate = plate_of(norm);
  out->forward = plate_forward(out->plate);
  qh = simhash_text(norm);
  for (i = 0; i < NG_MOTIVE_CONCEPTS; i++) {
    int lex = lexical_hits(norm, kConcept[i]);
    int h = ham64(qh, simhash_text(kConcept[i]));
    int score;
    if (learn_path)
      lex += ng_motive_label_lex(text, learn_path, kConcept[i]);
    if (same_tokens(norm, kConcept[i])) h = 0;
    score = lex * lw + (64 - h);
    if (score > best_score) {
      best = i;
      best_score = score;
      best_lex = lex;
      best_ham = h;
    }
  }
  out->rank = best_score < 0 ? 0 : best_score;
  out->lexical = best_lex;
  out->hamming = best_ham;
  copy_cap(out->concept, sizeof out->concept, kConcept[best]);
  if (out->forward)
    copy_cap(out->line, sizeof out->line, kConcept[best]);
  else
    copy_cap(out->line, sizeof out->line, remedy_line(out->plate));
}

void ng_motive_rank(const char *text, ng_motive *out) {
  ng_motive_rank_w(text, out, -1);
}

int ng_motive_blocks(const char *text) {
  ng_motive m;
  ng_motive_rank(text, &m);
  return m.plate == NG_PLATE_PURGE || m.plate == NG_PLATE_PROTECT;
}

char *ng_motive_format(const ng_motive *m) {
  char *line = NULL, *concept = NULL, *out = NULL;
  if (!m) return NULL;
  line = ng_json_escape(m->line);
  concept = ng_json_escape(m->concept);
  asprintf(&out,
           "{\"schema\":\"" NG_MOTIVE_SCHEMA "\",\"ok\":true,"
           "\"plate\":\"%s\",\"forward\":%s,\"rank\":%d,"
           "\"hamming\":%d,\"lexical\":%d,\"lex_w\":%d,"
           "\"concept\":\"%s\",\"line\":\"%s\","
           "\"concepts\":%d,\"python\":0}",
           ng_motive_plate_name(m->plate), m->forward ? "true" : "false", m->rank,
           m->hamming, m->lexical, m->lex_w, concept ? concept : "", line ? line : "",
           NG_MOTIVE_CONCEPTS);
  free(line);
  free(concept);
  return out ? out
             : strdup("{\"schema\":\"" NG_MOTIVE_SCHEMA "\",\"ok\":false,"
                      "\"error\":\"oom\",\"python\":0}");
}

char *ng_motive_json(const char *text) {
  ng_motive m;
  ng_motive_rank(text, &m);
  return ng_motive_format(&m);
}
