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
      if (*p == 'n') *d++ = '\n';
      else if (*p == 'r') *d++ = '\r';
      else if (*p == 't') *d++ = '\t';
      else *d++ = *p;
      p++;
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
