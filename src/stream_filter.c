#include "stream_filter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NG_FILTER_RULES 16
#define NG_FILTER_BYTE 128

typedef struct {
  int op; /* 1 drop, 2 redact, 3 drop_kind */
  char match[NG_FILTER_BYTE + 1];
  char repl[NG_FILTER_BYTE + 1];
  char kind[32];
} ng_filter_rule;

struct ng_stream_filter {
  ng_filter_rule rules[NG_FILTER_RULES];
  int n;
  size_t hold;
};

static const char *skip_ws(const char *p) {
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  return p;
}

static int copy_cap(char *dst, size_t dstsz, const char *src) {
  size_t n;
  if (!dst || dstsz == 0) return -1;
  if (!src) src = "";
  n = strlen(src);
  if (n >= dstsz) return -1;
  memcpy(dst, src, n + 1);
  return 0;
}

/* First JSON string for key inside one object. Caller frees. */
static char *obj_string(const char *obj, const char *key) {
  char pat[80];
  const char *p;
  size_t n = 0;
  char *out;
  if (!obj || !key) return NULL;
  snprintf(pat, sizeof pat, "\"%s\"", key);
  p = strstr(obj, pat);
  if (!p) return NULL;
  p += strlen(pat);
  p = strchr(p, ':');
  if (!p) return NULL;
  p = skip_ws(p + 1);
  if (*p != '"') return NULL;
  p++;
  while (p[n] && p[n] != '"') {
    if (p[n] == '\\' && p[n + 1]) n += 2;
    else n++;
  }
  out = malloc(n + 1);
  if (!out) return NULL;
  {
    size_t i = 0, j = 0;
    while (i < n) {
      if (p[i] == '\\' && i + 1 < n) {
        char c = p[++i];
        if (c == 'n') out[j++] = '\n';
        else if (c == 't') out[j++] = '\t';
        else out[j++] = c;
        i++;
      } else {
        out[j++] = p[i++];
      }
    }
    out[j] = 0;
  }
  return out;
}

static char *take_object(const char **pp) {
  const char *p = skip_ws(*pp);
  const char *s;
  int depth = 0;
  size_t n;
  char *obj;
  if (*p != '{') return NULL;
  s = p;
  for (; *p; p++) {
    if (*p == '"') {
      p++;
      while (*p && *p != '"') {
        if (*p == '\\' && p[1]) p++;
        p++;
      }
      if (!*p) return NULL;
      continue;
    }
    if (*p == '{') depth++;
    else if (*p == '}') {
      depth--;
      if (depth == 0) {
        n = (size_t)(p - s) + 1;
        obj = malloc(n + 1);
        if (!obj) return NULL;
        memcpy(obj, s, n);
        obj[n] = 0;
        *pp = p + 1;
        return obj;
      }
    }
  }
  return NULL;
}

ng_stream_filter *ng_stream_filter_parse(const char *json) {
  ng_stream_filter *f;
  const char *p;
  f = calloc(1, sizeof *f);
  if (!f) return NULL;
  if (!json || !json[0] || strcmp(json, "{}") == 0) return f;
  p = strstr(json, "\"rules\"");
  if (!p) {
    if (json[0] == '{') return f;
    free(f);
    return NULL;
  }
  p = strchr(p, '[');
  if (!p) {
    free(f);
    return NULL;
  }
  p++;
  while (*p && f->n < NG_FILTER_RULES) {
    char *obj;
    char *op, *match, *with, *kind;
    ng_filter_rule *r;
    p = skip_ws(p);
    if (*p == ',') { p++; continue; }
    if (*p == ']' || !*p) break;
    obj = take_object(&p);
    if (!obj) break;
    op = obj_string(obj, "op");
    match = obj_string(obj, "match");
    with = obj_string(obj, "with");
    kind = obj_string(obj, "kind");
    r = &f->rules[f->n];
    memset(r, 0, sizeof *r);
    if (op && strcmp(op, "drop") == 0) r->op = 1;
    else if (op && strcmp(op, "redact") == 0) r->op = 2;
    else if (op && strcmp(op, "drop_kind") == 0) r->op = 3;
    if (r->op == 3) {
      const char *k = (kind && kind[0]) ? kind : match;
      if (!k || copy_cap(r->kind, sizeof r->kind, k) != 0 || !r->kind[0]) r->op = 0;
    } else if (r->op) {
      if (!match || copy_cap(r->match, sizeof r->match, match) != 0 || !r->match[0])
        r->op = 0;
      else if (r->op == 2) {
        if (!with || !with[0]) copy_cap(r->repl, sizeof r->repl, "***");
        else if (copy_cap(r->repl, sizeof r->repl, with) != 0) r->op = 0;
      }
      if (r->op && strlen(r->match) > f->hold + 1) f->hold = strlen(r->match) - 1;
    }
    if (r->op) f->n++;
    free(op); free(match); free(with); free(kind);
    free(obj);
  }
  return f;
}

void ng_stream_filter_free(ng_stream_filter *f) { free(f); }

size_t ng_stream_filter_hold(const ng_stream_filter *f) {
  return f ? f->hold : 0;
}

static int is_prefix(const ng_filter_rule *r, const char *s, size_t n) {
  size_t ml;
  if (!r || (r->op != 1 && r->op != 2) || !r->match[0]) return 0;
  ml = strlen(r->match);
  if (n == 0 || n >= ml) return 0;
  return memcmp(s, r->match, n) == 0;
}

static int kind_denied(const ng_stream_filter *f, const char *kind) {
  int i;
  if (!f || !kind) return 0;
  for (i = 0; i < f->n; i++) {
    if (f->rules[i].op == 3 && strcmp(f->rules[i].kind, kind) == 0) return 1;
  }
  return 0;
}

/* Scan work. *consumed is the raw prefix that is safe to emit.
 * *dropped is 1 when a drop needle completed. *out is the passed text.
 */
static int scan_work(const ng_stream_filter *f, const char *work, size_t n,
                     int flush, size_t *consumed, int *dropped, char **out) {
  size_t i = 0, o = 0, cap;
  char *buf;
  *consumed = 0;
  *dropped = 0;
  *out = NULL;
  cap = n + 1;
  if (f) {
    int k;
    for (k = 0; k < f->n; k++) {
      if (f->rules[k].op == 2 && f->rules[k].repl[0])
        cap += n * (strlen(f->rules[k].repl) + 1);
    }
  }
  if (cap > 1024 * 1024) cap = 1024 * 1024;
  buf = malloc(cap + 1);
  if (!buf) return -1;
  while (i < n) {
    int found = 0;
    int k;
    if (f) {
      for (k = 0; k < f->n; k++) {
        const ng_filter_rule *r = &f->rules[k];
        size_t ml;
        if (r->op != 1 && r->op != 2) continue;
        ml = strlen(r->match);
        if (ml == 0 || i + ml > n) continue;
        if (memcmp(work + i, r->match, ml) != 0) continue;
        if (r->op == 1) {
          free(buf);
          *dropped = 1;
          *consumed = n;
          return 0;
        }
        if (o + strlen(r->repl) > cap) {
          free(buf);
          return -1;
        }
        memcpy(buf + o, r->repl, strlen(r->repl));
        o += strlen(r->repl);
        i += ml;
        found = 1;
        break;
      }
    }
    if (found) continue;
    if (!flush && f) {
      size_t left = n - i;
      for (k = 0; k < f->n; k++) {
        if (is_prefix(&f->rules[k], work + i, left)) {
          buf[o] = 0;
          *out = buf;
          *consumed = i;
          return 1;
        }
      }
    }
    if (o + 1 > cap) {
      free(buf);
      return -1;
    }
    buf[o++] = work[i++];
  }
  buf[o] = 0;
  *out = buf;
  *consumed = i;
  return 1;
}

static int push_common(ng_stream_filter *f, const char *kind, const char *text,
                       char *carry, size_t *carry_n, size_t carry_sz, char **out,
                       int flush) {
  size_t cn, tl, n, consumed = 0;
  int dropped = 0;
  char *work;
  char *passed = NULL;
  int rc;
  if (!out) return -1;
  *out = NULL;
  if (!text) text = "";
  if (kind_denied(f, kind && kind[0] ? kind : "token")) return 0;
  cn = (carry && carry_n) ? *carry_n : 0;
  if (!carry || !carry_n || carry_sz < 2) cn = 0;
  if (cn > carry_sz) cn = carry_sz;
  tl = strlen(text);
  work = malloc(cn + tl + 1);
  if (!work) return -1;
  if (cn) memcpy(work, carry, cn);
  memcpy(work + cn, text, tl + 1);
  n = cn + tl;
  rc = scan_work(f, work, n, flush, &consumed, &dropped, &passed);
  if (rc < 0) {
    free(work);
    return -1;
  }
  if (dropped) {
    free(work);
    free(passed);
    if (carry_n) *carry_n = 0;
    if (carry && carry_sz) carry[0] = 0;
    return 0;
  }
  if (consumed < n) {
    size_t left = n - consumed;
    if (!carry || left >= carry_sz) {
      /* Cannot hold the prefix. Emit it as literal text. */
      char *both;
      size_t pl = passed ? strlen(passed) : 0;
      both = malloc(pl + left + 1);
      if (!both) {
        free(work);
        free(passed);
        return -1;
      }
      if (pl) memcpy(both, passed, pl);
      memcpy(both + pl, work + consumed, left);
      both[pl + left] = 0;
      free(passed);
      passed = both;
      consumed = n;
    } else {
      memcpy(carry, work + consumed, left);
      carry[left] = 0;
      *carry_n = left;
    }
  } else if (carry_n) {
    *carry_n = 0;
    if (carry && carry_sz) carry[0] = 0;
  }
  free(work);
  *out = passed ? passed : strdup("");
  if (!*out) return -1;
  return 1;
}

int ng_stream_filter_push(ng_stream_filter *f, const char *kind, const char *text,
                          char *carry, size_t *carry_n, size_t carry_sz, char **out) {
  return push_common(f, kind, text, carry, carry_n, carry_sz, out, 0);
}

int ng_stream_filter_flush(ng_stream_filter *f, const char *kind,
                           char *carry, size_t *carry_n, size_t carry_sz, char **out) {
  return push_common(f, kind, "", carry, carry_n, carry_sz, out, 1);
}
