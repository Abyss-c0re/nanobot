#include "memory.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

static void mem_path(char *out, size_t n, const char *name) {
  snprintf(out, n, "%s/memory/%s", ng_workdir(), name);
}

void ng_memory_init(void) {
  char dir[640];
  snprintf(dir, sizeof dir, "%s/memory", ng_workdir());
  mkdir(ng_workdir(), 0755);
  mkdir(dir, 0755);
  /* seed core if missing */
  char core[640];
  mem_path(core, sizeof core, "core.txt");
  if (access(core, R_OK) != 0) {
    const char *seed =
      "nanobot is a small standalone agent host (UI, shell, memory, MCP).\n"
      "Prefer short answers. Do not destroy the host. Write under NANOBOT_HOME when needed.\n"
      "User may run offline shell with @! <command>.\n";
    ng_write_file(core, seed, strlen(seed));
  }
}

static char *read_capped(const char *path, size_t max_bytes) {
  size_t len = 0;
  char *b = ng_read_file(path, &len);
  if (!b) return NULL;
  if (len > max_bytes) {
    /* keep tail (most recent notes) */
    size_t start = len - max_bytes;
    while (start < len && b[start] != '\n') start++;
    if (start < len) start++;
    char *t = malloc(max_bytes + 1);
    if (!t) { free(b); return NULL; }
    size_t n = len - start;
    if (n > max_bytes) n = max_bytes;
    memcpy(t, b + start, n);
    t[n] = 0;
    free(b);
    return t;
  }
  return b;
}

static void truncate_store(char *s, size_t max_chars) {
  if (!s) return;
  size_t n = strlen(s);
  if (n <= max_chars) return;
  s[max_chars] = 0;
  /* avoid cutting mid-utf8 mid-word messily */
  if (max_chars > 3) {
    s[max_chars - 1] = '.';
    s[max_chars - 2] = '.';
    s[max_chars - 3] = '.';
  }
}

char *ng_memory_system_prompt(void) {
  ng_memory_init();
  char corep[640], profp[640], sump[640];
  mem_path(corep, sizeof corep, "core.txt");
  mem_path(profp, sizeof profp, "profile.txt");
  mem_path(sump, sizeof sump, "summary.txt");

  char *core = read_capped(corep, NG_MEM_MAX_CORE);
  char *prof = read_capped(profp, NG_MEM_MAX_PROFILE);
  char *sum = read_capped(sump, NG_MEM_MAX_SUMMARY);

  char *out = NULL;
  asprintf(&out,
    "You are nanobot, a tiny standalone agent host on this machine.\n"
    "Prefer short answers. Use run_terminal_command for shell when needed. "
    "User may run offline shell with @! <command>.\n"
    "Do not destroy the host system. Write under the nanobot home directory when needed.\n"
    "Adapt via memory/ files without bloating context; keep facts small.\n"
    "\n## Always true (core)\n%s\n"
    "%s%s"
    "%s%s",
    core && core[0] ? core : "(none)",
    prof && prof[0] ? "\n## User profile (adaptive, compact)\n" : "",
    prof && prof[0] ? prof : "",
    sum && sum[0] ? "\n## Compacted earlier context\n" : "",
    sum && sum[0] ? sum : "");

  free(core); free(prof); free(sum);
  return out ? out : strdup("You are nanobot, a tiny standalone agent. Keep answers short.");
}

/* One recent line: {"role":"...","content":"..."} */
typedef struct {
  char role[16];
  char *content;
} mem_msg;

static void free_msgs(mem_msg *m, int n) {
  for (int i = 0; i < n; i++) free(m[i].content);
}

static int load_recent(mem_msg **out_msgs, int *out_n) {
  *out_msgs = NULL; *out_n = 0;
  char path[640];
  mem_path(path, sizeof path, "recent.jsonl");
  size_t len = 0;
  char *raw = ng_read_file(path, &len);
  if (!raw || !len) { free(raw); return 0; }

  mem_msg *arr = calloc(64, sizeof(mem_msg));
  int n = 0, cap = 64;
  char *p = raw;
  while (*p && n < 60) {
    char *nl = strchr(p, '\n');
    if (nl) *nl = 0;
    if (p[0] == '{') {
      char *role = ng_json_get_string(p, "role");
      char *content = ng_json_get_string(p, "content");
      if (role && content) {
        if (n >= cap) {
          cap *= 2;
          mem_msg *na = realloc(arr, (size_t)cap * sizeof(mem_msg));
          if (!na) { free(role); free(content); break; }
          arr = na;
        }
        snprintf(arr[n].role, sizeof arr[n].role, "%s", role);
        arr[n].content = content;
        content = NULL;
        n++;
      }
      free(role); free(content);
    }
    if (!nl) break;
    p = nl + 1;
  }
  free(raw);
  *out_msgs = arr;
  *out_n = n;
  return 0;
}

static int save_recent(mem_msg *msgs, int n) {
  char path[640], tmp[660];
  mem_path(path, sizeof path, "recent.jsonl");
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  FILE *f = fopen(tmp, "w");
  if (!f) return -1;
  for (int i = 0; i < n; i++) {
    char *esc = ng_json_escape(msgs[i].content ? msgs[i].content : "");
    fprintf(f, "{\"role\":\"%s\",\"content\":\"%s\"}\n",
            msgs[i].role[0] ? msgs[i].role : "user",
            esc ? esc : "");
    free(esc);
  }
  fclose(f);
  if (rename(tmp, path) != 0) {
    unlink(tmp);
    return -1;
  }
  return 0;
}

char *ng_memory_recent_json_fragment(void) {
  mem_msg *msgs = NULL;
  int n = 0;
  load_recent(&msgs, &n);
  if (n <= 0) {
    free(msgs);
    return strdup("");
  }
  char *out = strdup("");
  for (int i = 0; i < n; i++) {
    char *esc = ng_json_escape(msgs[i].content ? msgs[i].content : "");
    char *piece = NULL;
    asprintf(&piece, "%s{\"role\":\"%s\",\"content\":\"%s\"}",
             out[0] ? "," : "",
             msgs[i].role,
             esc ? esc : "");
    free(esc);
    free(out);
    out = piece ? piece : strdup("");
  }
  free_msgs(msgs, n);
  free(msgs);
  return out;
}

static void append_summary_line(const char *line) {
  char path[640];
  mem_path(path, sizeof path, "summary.txt");
  size_t len = 0;
  char *cur = ng_read_file(path, &len);
  size_t ll = strlen(line);
  size_t need = (cur ? len : 0) + ll + 2;
  char *nbuf = malloc(need + 1);
  if (!nbuf) { free(cur); return; }
  nbuf[0] = 0;
  if (cur && cur[0]) {
    memcpy(nbuf, cur, len);
    nbuf[len] = 0;
    if (len && nbuf[len - 1] != '\n') strcat(nbuf, "\n");
  }
  strcat(nbuf, line);
  if (nbuf[strlen(nbuf) - 1] != '\n') strcat(nbuf, "\n");
  free(cur);

  /* cap: keep tail */
  size_t total = strlen(nbuf);
  const char *write_from = nbuf;
  if (total > NG_MEM_MAX_SUMMARY) {
    write_from = nbuf + (total - NG_MEM_MAX_SUMMARY);
    while (*write_from && *write_from != '\n') write_from++;
    if (*write_from == '\n') write_from++;
  }
  ng_write_file(path, write_from, strlen(write_from));
  free(nbuf);
}

static void compact_drop(mem_msg *msgs, int from, int to) {
  /* Fold dropped turns into one short summary line */
  char buf[512];
  size_t o = 0;
  o += (size_t)snprintf(buf + o, sizeof buf - o, "- older:");
  for (int i = from; i < to && o + 40 < sizeof buf; i++) {
    const char *c = msgs[i].content ? msgs[i].content : "";
    char snippet[48];
    snprintf(snippet, sizeof snippet, "%.40s", c);
    /* strip newlines */
    for (char *p = snippet; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
    o += (size_t)snprintf(buf + o, sizeof buf - o, " [%s] %s",
                          msgs[i].role, snippet);
  }
  append_summary_line(buf);
  ng_log("memory: compacted %d messages into summary", to - from);
}

void ng_memory_record_exchange(const char *user, const char *assistant) {
  if (!user || !user[0]) return;
  ng_memory_init();

  char *u = strdup(user);
  char *a = strdup(assistant ? assistant : "");
  if (!u || !a) { free(u); free(a); return; }
  truncate_store(u, NG_MEM_MAX_MSG_CHARS);
  truncate_store(a, NG_MEM_MAX_MSG_CHARS);

  mem_msg *msgs = NULL;
  int n = 0;
  load_recent(&msgs, &n);
  if (!msgs) {
    msgs = calloc(16, sizeof(mem_msg));
    n = 0;
  }

  /* append pair */
  mem_msg *na = realloc(msgs, (size_t)(n + 2) * sizeof(mem_msg));
  if (!na) { free(u); free(a); free_msgs(msgs, n); free(msgs); return; }
  msgs = na;
  snprintf(msgs[n].role, sizeof msgs[n].role, "user");
  msgs[n].content = u;
  n++;
  snprintf(msgs[n].role, sizeof msgs[n].role, "assistant");
  msgs[n].content = a;
  n++;

  int max_msgs = NG_MEM_MAX_RECENT_TURNS * 2;
  if (n > max_msgs) {
    int drop = n - max_msgs;
    /* drop complete pairs from start */
    if (drop % 2) drop++;
    if (drop > n) drop = n;
    compact_drop(msgs, 0, drop);
    for (int i = 0; i < drop; i++) free(msgs[i].content);
    memmove(msgs, msgs + drop, (size_t)(n - drop) * sizeof(mem_msg));
    n -= drop;
  }

  save_recent(msgs, n);
  free_msgs(msgs, n);
  free(msgs);
}

void ng_memory_note_profile(const char *line) {
  if (!line || !line[0]) return;
  ng_memory_init();
  char path[640];
  mem_path(path, sizeof path, "profile.txt");
  size_t len = 0;
  char *cur = ng_read_file(path, &len);
  if (cur && strstr(cur, line)) { free(cur); return; } /* dedupe exact */
  char *nl = NULL;
  asprintf(&nl, "%s%s%s\n", cur ? cur : "", (cur && cur[0] && cur[len-1] != '\n') ? "\n" : "", line);
  free(cur);
  if (!nl) return;
  size_t total = strlen(nl);
  const char *w = nl;
  if (total > NG_MEM_MAX_PROFILE) {
    w = nl + (total - NG_MEM_MAX_PROFILE);
    while (*w && *w != '\n') w++;
    if (*w == '\n') w++;
  }
  ng_write_file(path, w, strlen(w));
  free(nl);
}
