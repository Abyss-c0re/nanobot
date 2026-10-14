#include "motive.h"

#include "util.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void copy_cap(char *dst, size_t cap, const char *src) {
  size_t n;
  if (!dst || cap < 2) return;
  if (!src) src = "";
  n = strlen(src);
  if (n >= cap) n = cap - 1;
  memcpy(dst, src, n);
  dst[n] = 0;
}

void ng_motive_rank(const char *text, ng_motive *out) {
  char norm[NORM_CAP];
  uint64_t qh;
  int i, best = 0, best_score = -1, best_lex = 0, best_ham = 64;
  if (!out) return;
  memset(out, 0, sizeof *out);
  norm_text(text, norm, sizeof norm);
  out->plate = plate_of(norm);
  out->forward = plate_forward(out->plate);
  qh = simhash_text(norm);
  for (i = 0; i < NG_MOTIVE_CONCEPTS; i++) {
    int lex = lexical_hits(norm, kConcept[i]);
    int h = ham64(qh, simhash_text(kConcept[i]));
    int score = lex * 64 + (64 - h);
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

int ng_motive_blocks(const char *text) {
  ng_motive m;
  ng_motive_rank(text, &m);
  return m.plate == NG_PLATE_PURGE || m.plate == NG_PLATE_PROTECT;
}

char *ng_motive_json(const char *text) {
  ng_motive m;
  char *line = NULL, *concept = NULL, *out = NULL;
  ng_motive_rank(text, &m);
  line = ng_json_escape(m.line);
  concept = ng_json_escape(m.concept);
  asprintf(&out,
           "{\"schema\":\"" NG_MOTIVE_SCHEMA "\",\"ok\":true,"
           "\"plate\":\"%s\",\"forward\":%s,\"rank\":%d,"
           "\"hamming\":%d,\"lexical\":%d,"
           "\"concept\":\"%s\",\"line\":\"%s\","
           "\"concepts\":%d,\"python\":0}",
           ng_motive_plate_name(m.plate), m.forward ? "true" : "false", m.rank,
           m.hamming, m.lexical, concept ? concept : "", line ? line : "",
           NG_MOTIVE_CONCEPTS);
  free(line);
  free(concept);
  return out ? out
             : strdup("{\"schema\":\"" NG_MOTIVE_SCHEMA "\",\"ok\":false,"
                      "\"error\":\"oom\",\"python\":0}");
}
