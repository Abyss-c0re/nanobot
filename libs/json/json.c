#include <nanobot/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* UTF-8 sequence length for lead byte, or 0 if invalid lead. */
static int utf8_seq_len(unsigned char c) {
  if (c < 0x80) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 0; /* continuation or illegal lead */
}

/* Four hex digits, or 0. */
static int json_hex4(const char *s, unsigned *out) {
  unsigned v = 0;
  int i;
  if (!s || !out) return 0;
  for (i = 0; i < 4; i++) {
    unsigned char c = (unsigned char)s[i];
    unsigned d;
    if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
    else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
    else return 0;
    v = (v << 4) | d;
  }
  *out = v;
  return 1;
}

/* Write one scalar. The caller skips 0 and lone surrogates. */
static int json_utf8_put(char *d, unsigned cp) {
  if (cp < 0x80) {
    d[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    d[0] = (char)(0xC0 | (cp >> 6));
    d[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    d[0] = (char)(0xE0 | (cp >> 12));
    d[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    d[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  d[0] = (char)(0xF0 | (cp >> 18));
  d[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  d[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  d[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

char *nb_json_escape(const char *s) {
  if (!s) s = "";
  /* Worst case: every byte becomes \\uXXXX (6). */
  size_t slen = strlen(s);
  char *o = malloc(slen * 6 + 1);
  if (!o) return NULL;
  char *d = o;
  const unsigned char *p = (const unsigned char *)s;
  while (*p) {
    unsigned char c = *p;
    if (c == '"') {
      *d++ = '\\'; *d++ = '"'; p++;
    } else if (c == '\\') {
      *d++ = '\\'; *d++ = '\\'; p++;
    } else if (c == '\n') {
      *d++ = '\\'; *d++ = 'n'; p++;
    } else if (c == '\r') {
      *d++ = '\\'; *d++ = 'r'; p++;
    } else if (c == '\t') {
      *d++ = '\\'; *d++ = 't'; p++;
    } else if (c < 0x20) {
      d += sprintf(d, "\\u%04x", (unsigned)c);
      p++;
    } else if (c < 0x80) {
      *d++ = (char)c;
      p++;
    } else {
      /* Multi-byte UTF-8: only emit complete valid sequences.
       * Truncated summary caps used to cut mid-em-dash (e2 80 94 → lone e2)
       * which Grok rejects as "invalid unicode code point". */
      int need = utf8_seq_len(c);
      int ok = need > 1;
      for (int i = 1; ok && i < need; i++) {
        if ((p[i] & 0xC0) != 0x80) ok = 0;
      }
      if (ok) {
        for (int i = 0; i < need; i++) *d++ = (char)p[i];
        p += need;
      } else {
        /* skip one bad byte (do not emit invalid UTF-8 into JSON) */
        p++;
      }
    }
  }
  *d = 0;
  return o;
}

static const char *find_key(const char *json, const char *key) {
  char pat[128];
  snprintf(pat, sizeof pat, "\"%s\"", key);
  const char *p = json;
  while ((p = strstr(p, pat)) != NULL) {
    if (p > json && p[-1] == '\\') {
      p++;
      continue;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p == ':') return p + 1;
    p++;
  }
  return NULL;
}

char *nb_json_get_string(const char *json, const char *key) {
  const char *p = find_key(json, key);
  if (!p) return NULL;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  if (*p != '"') return NULL;
  p++;
  char *out = malloc(strlen(p) + 1);
  if (!out) return NULL;
  char *d = out;
  while (*p && *p != '"') {
    if (*p == '\\' && p[1]) {
      p++;
      if (*p == 'n') {
        *d++ = '\n';
        p++;
      } else if (*p == 'r') {
        *d++ = '\r';
        p++;
      } else if (*p == 't') {
        *d++ = '\t';
        p++;
      } else if (*p == 'b') {
        *d++ = '\b';
        p++;
      } else if (*p == 'f') {
        *d++ = '\f';
        p++;
      } else if (*p == 'u') {
        unsigned cp = 0;
        if (!json_hex4(p + 1, &cp)) {
          *d++ = 'u';
          p++;
        } else {
          p += 5;
          if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
            unsigned lo = 0;
            if (json_hex4(p + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
              cp = 0x10000u + (((cp - 0xD800u) << 10) | (lo - 0xDC00u));
              p += 6;
            } else {
              cp = 0;
            }
          } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0;
          }
          if (cp != 0) d += json_utf8_put(d, cp);
        }
      } else {
        *d++ = *p;
        p++;
      }
    } else {
      *d++ = *p++;
    }
  }
  *d = 0;
  return out;
}

char *nb_json_message_content(const char *json) {
  const char *msg = strstr(json, "\"message\"");
  if (msg) {
    char *c = nb_json_get_string(msg, "content");
    if (c && c[0]) return c;
    free(c);
  }
  return nb_json_get_string(json, "content");
}

int nb_json_first_tool_call(const char *json, char **name, char **args, char **id) {
  *name = *args = *id = NULL;
  const char *tc = strstr(json, "\"tool_calls\"");
  if (!tc) return 0;
  const char *fn = strstr(tc, "\"function\"");
  if (!fn) return 0;
  *name = nb_json_get_string(fn, "name");
  *args = nb_json_get_string(fn, "arguments");
  *id = nb_json_get_string(tc, "id");
  if (!*name) return 0;
  if (!*args) *args = strdup("{}");
  if (!*id) *id = strdup("call_0");
  return 1;
}
