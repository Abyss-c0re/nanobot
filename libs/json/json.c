#include <nanobot/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *nb_json_escape(const char *s) {
  if (!s) s = "";
  size_t n = 0;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    if (*p == '"' || *p == '\\' || *p == '\n' || *p == '\r' || *p == '\t') n += 2;
    else if (*p < 0x20) n += 6;
    else n++;
  }
  char *o = malloc(n + 1);
  if (!o) return NULL;
  char *d = o;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    if (*p == '"') {
      *d++ = '\\';
      *d++ = '"';
    } else if (*p == '\\') {
      *d++ = '\\';
      *d++ = '\\';
    } else if (*p == '\n') {
      *d++ = '\\';
      *d++ = 'n';
    } else if (*p == '\r') {
      *d++ = '\\';
      *d++ = 'r';
    } else if (*p == '\t') {
      *d++ = '\\';
      *d++ = 't';
    } else if (*p < 0x20) {
      d += sprintf(d, "\\u%04x", (unsigned)*p);
    } else {
      *d++ = (char)*p;
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
