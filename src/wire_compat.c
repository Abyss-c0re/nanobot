#include "wire_compat.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SUG_BUDGET 24000u
#define SUG_MSG_CAP 1500u
#define SUG_MARK "\n…"

static int ci_eq(const char *a, const char *b) {
  if (!a || !b) return 0;
  while (*a && *b) {
    unsigned char ca = (unsigned char)*a++;
    unsigned char cb = (unsigned char)*b++;
    if (tolower(ca) != tolower(cb)) return 0;
  }
  return *a == 0 && *b == 0;
}

static int ci_has(const char *hay, const char *needle) {
  size_t n;
  if (!hay || !needle || !needle[0]) return 0;
  n = strlen(needle);
  for (; *hay; hay++) {
    size_t i = 0;
    while (i < n && hay[i] &&
           tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i]))
      i++;
    if (i == n) return 1;
  }
  return 0;
}

static char *dup_n(const char *s, size_t n) {
  char *o = malloc(n + 1);
  if (!o) return NULL;
  memcpy(o, s, n);
  o[n] = 0;
  return o;
}

static const char *skip_ws(const char *p) {
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  return p;
}

int ng_effort_canon(const char *in, char *out, size_t outsz) {
  char tmp[40];
  size_t n = 0;
  const char *p;
  if (!out || outsz < 2) return -1;
  out[0] = 0;
  if (!in || !in[0] || ci_eq(in, "default") || ci_eq(in, "off") || ci_eq(in, "none"))
    return 0;
  for (p = in; *p; p++) {
    unsigned char c = (unsigned char)*p;
    if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
      return -1;
    if (n + 1 >= sizeof tmp) return -1;
    tmp[n++] = (char)c;
  }
  tmp[n] = 0;
  if (n == 0 || n > 32) return -1;
  if (tmp[0] < 'a' || tmp[0] > 'z') return -1;
  if (!strcmp(tmp, "x-high") || !strcmp(tmp, "x_high")) {
    memcpy(tmp, "xhigh", 6);
    n = 5;
  }
  if (n + 1 > outsz) return -1;
  memcpy(out, tmp, n + 1);
  return 0;
}

static int ident_ok(const char *e) {
  size_t n = 0;
  if (!e || !e[0]) return 0;
  for (; e[n]; n++) {
    unsigned char c = (unsigned char)e[n];
    if (n >= 40) return 0;
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
      return 0;
  }
  return 1;
}

const char *ng_client_identifier(void) {
  const char *e = getenv("NANOBOT_CLIENT_NAME");
  if (!ident_ok(e)) e = getenv("GROK_CLIENT_NAME");
  if (!ident_ok(e)) return "nanobot";
  return e;
}

char *ng_suggest_truncate(const char *text, size_t max_len) {
  size_t n, keep, floor, cut, mlen;
  const char *mark = SUG_MARK;
  char *out;
  if (!text) return NULL;
  while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r') text++;
  n = strlen(text);
  while (n && (text[n - 1] == ' ' || text[n - 1] == '\t' ||
               text[n - 1] == '\n' || text[n - 1] == '\r'))
    n--;
  if (n <= max_len) return dup_n(text, n);
  mlen = strlen(mark);
  if (max_len <= mlen) return dup_n(text, max_len ? max_len : 0);
  keep = max_len - mlen;
  while (keep > 0 && (unsigned char)text[keep] >= 0x80 &&
         (unsigned char)text[keep] < 0xC0)
    keep--;
  floor = keep / 2;
  cut = keep;
  if (keep > floor) {
    size_t i;
    for (i = keep; i > floor; i--) {
      char c = text[i - 1];
      if (c == '\n' || c == ' ' || c == '\t') {
        cut = i - 1;
        break;
      }
    }
  }
  while (cut > 0 && (text[cut - 1] == ' ' || text[cut - 1] == '\t' ||
                     text[cut - 1] == '\n' || text[cut - 1] == '\r'))
    cut--;
  out = malloc(cut + mlen + 1);
  if (!out) return NULL;
  memcpy(out, text, cut);
  memcpy(out + cut, mark, mlen + 1);
  return out;
}

static int role_is_agent(const char *role) {
  return role && (ci_eq(role, "assistant") || ci_eq(role, "agent"));
}

char *ng_suggest_pack_turns(const char *const *roles, const char *const *texts, int n) {
  char *lines[128];
  int ln = 0;
  size_t used = 0;
  int saw = 0;
  int i;
  char *out;
  size_t o = 0;
  if (!roles || !texts || n <= 0) return NULL;
  if (n > 128) {
    roles += n - 128;
    texts += n - 128;
    n = 128;
  }
  for (i = n - 1; i >= 0 && ln < 128; i--) {
    int agent = role_is_agent(roles[i]);
    size_t cap, prefix;
    char *body, *line;
    const char *lab;
    if (!texts[i] || !texts[i][0]) continue;
    prefix = agent ? 7u : 6u;
    if (agent && !saw)
      cap = (SUG_BUDGET > used + prefix) ? (SUG_BUDGET - used - prefix) : 0;
    else
      cap = SUG_MSG_CAP;
    if (cap < 8 && ln > 0) break;
    body = ng_suggest_truncate(texts[i], cap);
    if (!body || !body[0]) {
      free(body);
      continue;
    }
    lab = agent ? "Agent" : "User";
    if (asprintf(&line, "%s: %s", lab, body) < 0) line = NULL;
    free(body);
    if (!line) continue;
    if (used + strlen(line) > SUG_BUDGET && ln > 0) {
      free(line);
      break;
    }
    used += strlen(line) + 2;
    if (agent) saw = 1;
    lines[ln++] = line;
  }
  if (!saw) {
    for (i = 0; i < ln; i++) free(lines[i]);
    return NULL;
  }
  o = 1;
  for (i = 0; i < ln; i++) o += strlen(lines[i]) + 2;
  out = malloc(o + 1);
  if (!out) {
    for (i = 0; i < ln; i++) free(lines[i]);
    return NULL;
  }
  out[0] = 0;
  for (i = ln - 1; i >= 0; i--) {
    if (out[0]) strcat(out, "\n\n");
    strcat(out, lines[i]);
    free(lines[i]);
  }
  return out;
}

char *ng_suggest_pack_text(const char *transcript) {
  size_t n, drop;
  const char *start, *nl;
  if (!transcript || !transcript[0]) return NULL;
  n = strlen(transcript);
  if (n <= SUG_BUDGET) return dup_n(transcript, n);
  drop = n - SUG_BUDGET;
  start = transcript + drop;
  while (*start && (unsigned char)*start >= 0x80 && (unsigned char)*start < 0xC0)
    start++;
  nl = strchr(start, '\n');
  if (nl && (size_t)((transcript + n) - (nl + 1)) >= 64)
    start = nl + 1;
  else {
    const char *sp = start;
    size_t k = 0;
    while (sp[k] && sp[k] != ' ' && sp[k] != '\n' && k < 240) k++;
    if (sp[k]) start = sp + k + 1;
  }
  return dup_n(start, strlen(start));
}

int ng_resp_is_context_overflow(const char *resp) {
  static const char *marks[] = {
    "context_length_exceeded",
    "context_length",
    "maximum context length",
    "max context length",
    "context window",
    "prompt is too long",
    "too many tokens",
    "context_overflow",
    "max_context_length",
    NULL
  };
  int i;
  if (!resp || !resp[0]) return 0;
  if (!strstr(resp, "\"error\"") && !strstr(resp, "\"code\"") &&
      !strstr(resp, "context_length_exceeded"))
    return 0;
  for (i = 0; marks[i]; i++) {
    if (ci_has(resp, marks[i])) return 1;
  }
  return 0;
}

static char *dup_json_value(const char *p) {
  const char *s;
  int depth = 0;
  char open, close;
  if (!p || (*p != '{' && *p != '[')) return NULL;
  open = *p;
  close = (open == '{') ? '}' : ']';
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
    if (*p == open) depth++;
    else if (*p == close) {
      depth--;
      if (depth == 0) return dup_n(s, (size_t)(p - s) + 1);
    }
  }
  return NULL;
}

char *ng_mcp_extract_tool_result(const char *resp) {
  const char *sc, *p;
  if (!resp) return NULL;
  sc = strstr(resp, "\"structuredContent\"");
  if (!sc) return NULL;
  p = strchr(sc, ':');
  if (!p) return NULL;
  p = skip_ws(p + 1);
  if (*p != '{' && *p != '[') return NULL;
  return dup_json_value(p);
}

char *ng_mcp_read_bearer_file(const char *path) {
  FILE *f;
  char buf[2048];
  size_t n, i, L;
  char *s;
  if (!path || !path[0]) return NULL;
  f = fopen(path, "r");
  if (!f) return NULL;
  n = fread(buf, 1, sizeof buf, f);
  fclose(f);
  if (n == 0 || n >= sizeof buf) return NULL;
  buf[n] = 0;
  s = buf;
  while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
  L = strlen(s);
  while (L && (s[L - 1] == ' ' || s[L - 1] == '\t' ||
               s[L - 1] == '\n' || s[L - 1] == '\r')) {
    s[--L] = 0;
  }
  if (!L) return NULL;
  for (i = 0; i < L; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x20 || c == 0x7f) return NULL;
  }
  return dup_n(s, L);
}

static char *take_object(const char **pp) {
  const char *p = skip_ws(*pp);
  const char *s;
  int depth = 0;
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
        char *o = dup_n(s, (size_t)(p - s) + 1);
        *pp = p + 1;
        return o;
      }
    }
  }
  return NULL;
}

static char *join_msgs(char **parts, int n) {
  size_t total = 2;
  size_t o = 1;
  int i;
  char *out;
  for (i = 0; i < n; i++) total += strlen(parts[i]) + 1;
  out = malloc(total + 1);
  if (!out) return NULL;
  out[0] = '[';
  out[1] = 0;
  for (i = 0; i < n; i++) {
    if (i) {
      out[o++] = ',';
      out[o] = 0;
    }
    memcpy(out + o, parts[i], strlen(parts[i]));
    o += strlen(parts[i]);
    out[o] = 0;
  }
  out[o++] = ']';
  out[o] = 0;
  return out;
}

static char *compact_plate(int dropped, int truncated) {
  char *s = NULL;
  if (truncated)
    asprintf(&s,
             "{\"role\":\"user\",\"content\":\"{\\\"schema\\\":\\\"nanobot.compact.v1\\\","
             "\\\"ok\\\":true,\\\"truncated\\\":1}\"}");
  else
    asprintf(&s,
             "{\"role\":\"user\",\"content\":\"{\\\"schema\\\":\\\"nanobot.compact.v1\\\","
             "\\\"ok\\\":true,\\\"dropped\\\":%d}\"}",
             dropped);
  return s;
}

char *ng_messages_compact(const char *messages, int refill) {
  const char *p;
  char *head = NULL;
  char *last[2] = {NULL, NULL};
  int nlast = 0;
  int total = 0;
  int keep_tail;
  int keep_head;
  int kept;
  char *parts[6];
  int pn = 0;
  int dropped;
  char *plate = NULL;
  char *out;
  int i;
  if (!messages || refill < 1) return NULL;
  p = skip_ws(messages);
  if (*p != '[') return NULL;
  p++;
  for (;;) {
    char *obj;
    p = skip_ws(p);
    if (*p == ',' ) { p++; continue; }
    if (*p == ']' || !*p) break;
    obj = take_object(&p);
    if (!obj) break;
    total++;
    if (!head) {
      head = obj;
      continue;
    }
    if (nlast < 2) {
      last[nlast++] = obj;
      continue;
    }
    free(last[0]);
    last[0] = last[1];
    last[1] = obj;
  }
  if (total <= 0 || !head) {
    free(head);
    free(last[0]);
    free(last[1]);
    return NULL;
  }
  keep_tail = (refill <= 1) ? 2 : 1;
  keep_head = (refill >= 3) ? 0 : 1;
  if (keep_tail > nlast) keep_tail = nlast;
  kept = (keep_head ? 1 : 0) + keep_tail;
  /* A lone prompt is the last object. Dropping it would leave only a plate. */
  if (kept <= 0) {
    free(head);
    free(last[0]);
    free(last[1]);
    return NULL;
  }
  if (kept >= total) {
    int shrunk = 0;
    char *all[3];
    int an = 0;
    all[an++] = head;
    if (nlast > 0) all[an++] = last[0];
    if (nlast > 1) all[an++] = last[1];
    for (i = 0; i < an - 1; i++) {
      if (strlen(all[i]) > 1800) {
        char *rep = compact_plate(0, 1);
        if (rep) {
          free(all[i]);
          all[i] = rep;
          shrunk = 1;
        }
      }
    }
    if (!shrunk || an <= 0) {
      for (i = 0; i < an; i++) free(all[i]);
      return NULL;
    }
    head = all[0];
    if (an > 1) last[0] = all[1];
    if (an > 2) last[1] = all[2];
    out = join_msgs(all, an);
    for (i = 0; i < an; i++) free(all[i]);
    if (out && strlen(out) >= strlen(messages)) {
      free(out);
      return NULL;
    }
    return out;
  }
  dropped = total - kept;
  if (keep_head) parts[pn++] = head;
  else free(head);
  plate = compact_plate(dropped, 0);
  if (!plate) {
    free(last[0]);
    free(last[1]);
    if (keep_head) free(parts[0]);
    return NULL;
  }
  parts[pn++] = plate;
  if (keep_tail == 1) {
    if (nlast == 2) free(last[0]);
    parts[pn++] = nlast == 2 ? last[1] : last[0];
  } else {
    if (nlast > 0) parts[pn++] = last[0];
    if (nlast > 1) parts[pn++] = last[1];
  }
  out = join_msgs(parts, pn);
  for (i = 0; i < pn; i++) free(parts[i]);
  if (out && strlen(out) >= strlen(messages)) {
    free(out);
    return NULL;
  }
  return out;
}
