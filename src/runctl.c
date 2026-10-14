#include "runctl.h"

#include "stream_filter.h"
#include "util.h"
#include "wire_compat.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define RUN_TEXT_CAP 4000
#define RUN_CARRY 160

struct ng_run {
  char id[48];
  char dir[700];
  long seq;
  ng_stream_filter *filter;
  char filter_raw[8192];
  char token_carry[RUN_CARRY];
  size_t token_n;
  char think_carry[RUN_CARRY];
  size_t think_n;
};

void ng_run_limits_clear(ng_run_limits *lim) {
  if (!lim) return;
  memset(lim, 0, sizeof *lim);
  lim->compact_keep_head = -1;
  lim->compact_shrink_at = 1800;
  lim->compact_max_refills = 3;
  lim->compact_keep_tail = 2;
}

static int id_ok(const char *id) {
  size_t n = 0;
  if (!id || !id[0]) return 0;
  for (; id[n]; n++) {
    unsigned char c = (unsigned char)id[n];
    if (n >= 40) return 0;
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-'))
      return 0;
  }
  return 1;
}

static int kind_ok(const char *kind) {
  size_t n = 0;
  if (!kind || !kind[0]) return 0;
  for (; kind[n]; n++) {
    char c = kind[n];
    if (n >= 24) return 0;
    if (!((c >= 'a' && c <= 'z') || c == '_')) return 0;
  }
  return 1;
}

static int run_path(const char *id, const char *leaf, char *out, size_t outsz) {
  int n;
  if (!id_ok(id)) return -1;
  n = snprintf(out, outsz, "%s/runs/%s%s%s", ng_workdir(), id,
               leaf && leaf[0] ? "/" : "", leaf ? leaf : "");
  if (n < 0 || (size_t)n >= outsz) return -1;
  return 0;
}

static int json_int(const char *json, const char *key, int def) {
  char pat[64];
  const char *p;
  if (!json || !key) return def;
  snprintf(pat, sizeof pat, "\"%s\"", key);
  p = strstr(json, pat);
  if (!p) return def;
  p = strchr(p, ':');
  if (!p) return def;
  return atoi(p + 1);
}

static int write_text(const char *path, const char *text) {
  if (!text) text = "";
  return ng_write_file(path, text, strlen(text));
}

/* leaf includes its separator. Rejects a join that would not fit. */
static int path_join(char *out, size_t outsz, const char *dir, const char *leaf) {
  size_t nd, nl;
  if (!out || outsz == 0 || !dir || !leaf) return -1;
  nd = strlen(dir);
  nl = strlen(leaf);
  if (nd + nl + 1 > outsz) return -1;
  memcpy(out, dir, nd);
  memcpy(out + nd, leaf, nl + 1);
  return 0;
}

static char *limits_json(const ng_run_limits *lim) {
  ng_run_limits tmp;
  char *model = NULL;
  char *effort = NULL;
  char *out = NULL;
  char canon[40];
  if (!lim) {
    ng_run_limits_clear(&tmp);
    lim = &tmp;
  }
  canon[0] = 0;
  if (lim->has_effort && lim->effort[0]) {
    if (ng_effort_canon(lim->effort, canon, sizeof canon) != 0) return NULL;
  }
  model = ng_json_escape(lim->model);
  effort = ng_json_escape(lim->has_effort ? canon : "");
  if (asprintf(&out,
               "{\"model\":\"%s\",\"effort\":\"%s\",\"has_effort\":%d,"
               "\"max_tokens\":%d,\"context_tokens\":%d,"
               "\"compact_explicit\":%d,\"compact_max_refills\":%d,"
               "\"compact_keep_tail\":%d,\"compact_keep_head\":%d,"
               "\"compact_shrink_at\":%d,\"compact_preemptive\":%d}",
               model ? model : "", effort ? effort : "", lim->has_effort ? 1 : 0,
               lim->max_tokens, lim->context_tokens, lim->compact_explicit ? 1 : 0,
               lim->compact_max_refills, lim->compact_keep_tail, lim->compact_keep_head,
               lim->compact_shrink_at, lim->compact_preemptive ? 1 : 0) < 0)
    out = NULL;
  free(model);
  free(effort);
  return out;
}

static int write_meta(const char *id, const char *status, int pid, const char *error) {
  char path[700];
  char *esc;
  char *body = NULL;
  int rc;
  if (run_path(id, "meta.json", path, sizeof path) != 0) return -1;
  esc = ng_json_escape(error ? error : "");
  if (asprintf(&body,
               "{\"schema\":\"nanobot.run.v1\",\"ok\":true,\"action\":\"run\","
               "\"id\":\"%s\",\"status\":\"%s\",\"pid\":%d,\"error\":\"%s\","
               "\"python\":0}\n",
               id, status, pid, esc ? esc : "") < 0)
    body = NULL;
  free(esc);
  if (!body) return -1;
  rc = write_text(path, body);
  free(body);
  return rc;
}

static void load_filter(ng_run *run) {
  char path[700];
  char *raw;
  size_t n = 0;
  if (!run) return;
  if (run_path(run->id, "filter.json", path, sizeof path) != 0) return;
  raw = ng_read_file(path, &n);
  if (!raw) raw = strdup("{\"rules\":[]}");
  if (!raw) return;
  if (n >= sizeof run->filter_raw) n = sizeof run->filter_raw - 1;
  if (memcmp(run->filter_raw, raw, n) == 0 && run->filter_raw[n] == 0 && run->filter) {
    free(raw);
    return;
  }
  memcpy(run->filter_raw, raw, n);
  run->filter_raw[n] = 0;
  ng_stream_filter_free(run->filter);
  run->filter = ng_stream_filter_parse(run->filter_raw);
  if (!run->filter) run->filter = ng_stream_filter_parse(NULL);
  free(raw);
}

char *ng_run_create(const char *prompt, const ng_run_limits *limits,
                    const char *filter_json) {
  static unsigned seq;
  char id[48];
  char root[700], dir[700], path[700];
  char *cfg;
  const char *p;
  ng_run_limits tmp;
  if (!prompt) return NULL;
  if (filter_json && filter_json[0]) {
    ng_stream_filter *check = ng_stream_filter_parse(filter_json);
    if (!check || strlen(filter_json) > 8000) {
      ng_stream_filter_free(check);
      return NULL;
    }
    ng_stream_filter_free(check);
  }
  p = prompt;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  if (!*p) return NULL;
  if (strlen(prompt) > 256 * 1024) return NULL;
  if (snprintf(root, sizeof root, "%s/runs", ng_workdir()) >= (int)sizeof root)
    return NULL;
  mkdir(root, 0700);
  /* Each HTTP request is its own process, so a static counter alone repeats.
   * Pid separates workers. A reused pid in the same second retries. */
  {
    int made = 0;
    unsigned n = (unsigned)getpid();
    int attempt;
    for (attempt = 0; attempt < 16; attempt++) {
      char leaf[56];
      if (snprintf(id, sizeof id, "r%ld-%u-%u", (long)time(NULL), n,
                   (seq + (unsigned)attempt) % 1000) >= (int)sizeof id)
        return NULL;
      if (!id_ok(id)) return NULL;
      snprintf(leaf, sizeof leaf, "/%s", id);
      if (path_join(dir, sizeof dir, root, leaf) != 0) return NULL;
      if (mkdir(dir, 0700) == 0) {
        made = 1;
        seq += (unsigned)attempt + 1;
        break;
      }
      if (errno != EEXIST) return NULL;
    }
    if (!made) return NULL;
  }
  if (path_join(path, sizeof path, dir, "/prompt.txt") != 0) return NULL;
  if (write_text(path, prompt) != 0) return NULL;
  if (path_join(path, sizeof path, dir, "/control") != 0) return NULL;
  if (write_text(path, "run\n") != 0) return NULL;
  if (path_join(path, sizeof path, dir, "/events.jsonl") != 0) return NULL;
  if (write_text(path, "") != 0) return NULL;
  if (!limits) {
    ng_run_limits_clear(&tmp);
    limits = &tmp;
  } else {
    tmp = *limits;
    if (tmp.compact_shrink_at == 0) tmp.compact_shrink_at = 1800;
    if (tmp.compact_max_refills == 0 && !tmp.compact_explicit) tmp.compact_max_refills = 3;
    if (tmp.compact_keep_tail == 0) tmp.compact_keep_tail = 2;
    limits = &tmp;
  }
  cfg = limits_json(limits);
  if (!cfg) return NULL;
  if (path_join(path, sizeof path, dir, "/config.json") != 0) {
    free(cfg);
    return NULL;
  }
  if (write_text(path, cfg) != 0) {
    free(cfg);
    return NULL;
  }
  free(cfg);
  if (path_join(path, sizeof path, dir, "/filter.json") != 0) return NULL;
  if (write_text(path, filter_json && filter_json[0] ? filter_json : "{\"rules\":[]}") != 0)
    return NULL;
  if (write_meta(id, "queued", 0, "") != 0) return NULL;
  return strdup(id);
}

ng_run *ng_run_open(const char *id) {
  ng_run *run;
  char dir[700];
  struct stat st;
  FILE *fp;
  char line[1024];
  if (run_path(id, "", dir, sizeof dir) != 0) return NULL;
  if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) return NULL;
  run = calloc(1, sizeof *run);
  if (!run) return NULL;
  snprintf(run->id, sizeof run->id, "%s", id);
  snprintf(run->dir, sizeof run->dir, "%s", dir);
  load_filter(run);
  if (path_join(dir, sizeof dir, run->dir, "/events.jsonl") != 0) {
    ng_run_close(run);
    return NULL;
  }
  fp = fopen(dir, "r");
  if (fp) {
    while (fgets(line, sizeof line, fp)) {
      if (line[0] == '{') run->seq++;
    }
    fclose(fp);
  }
  return run;
}

void ng_run_close(ng_run *run) {
  if (!run) return;
  ng_stream_filter_free(run->filter);
  free(run);
}

const char *ng_run_id(const ng_run *run) { return run ? run->id : ""; }

int ng_run_mark_running(ng_run *run) {
  if (!run) return -1;
  return write_meta(run->id, "running", (int)getpid(), "");
}

static int append_event(ng_run *run, const char *kind, const char *name, const char *text) {
  char path[700];
  char *ne = NULL;
  char *te = NULL;
  char *line = NULL;
  int fd, rc = -1;
  char capped[RUN_TEXT_CAP + 1];
  size_t n;
  if (!run || !kind_ok(kind)) return -1;
  if (!text) text = "";
  n = strlen(text);
  if (n > RUN_TEXT_CAP) {
    memcpy(capped, text, RUN_TEXT_CAP);
    capped[RUN_TEXT_CAP] = 0;
    text = capped;
  }
  ne = ng_json_escape(name ? name : "");
  te = ng_json_escape(text);
  run->seq++;
  if (asprintf(&line,
               "{\"schema\":\"nanobot.run.v1\",\"seq\":%ld,\"kind\":\"%s\","
               "\"name\":\"%s\",\"text\":\"%s\",\"ok\":true,\"python\":0}\n",
               run->seq, kind, ne ? ne : "", te ? te : "") < 0)
    line = NULL;
  free(ne);
  free(te);
  if (!line) {
    run->seq--;
    return -1;
  }
  if (path_join(path, sizeof path, run->dir, "/events.jsonl") != 0) {
    run->seq--;
    free(line);
    return -1;
  }
  fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
  if (fd >= 0) {
    if (flock(fd, LOCK_EX) == 0) {
      size_t len = strlen(line);
      ssize_t w = write(fd, line, len);
      flock(fd, LOCK_UN);
      if (w == (ssize_t)len) rc = 1;
    }
    close(fd);
  }
  if (rc != 1) run->seq--;
  free(line);
  return rc;
}

static int filter_text(ng_run *run, const char *kind, const char *text, char **shown) {
  char *carry;
  size_t *cn;
  char local[RUN_CARRY];
  size_t local_n = 0;
  char *part = NULL;
  char *tail = NULL;
  int streaming;
  int rc;
  *shown = NULL;
  if (!kind) return -1;
  streaming = (strcmp(kind, "token") == 0 || strcmp(kind, "thinking") == 0);
  if (strcmp(kind, "token") == 0) {
    carry = run->token_carry;
    cn = &run->token_n;
  } else if (strcmp(kind, "thinking") == 0) {
    carry = run->think_carry;
    cn = &run->think_n;
  } else {
    local[0] = 0;
    carry = local;
    cn = &local_n;
    streaming = 0;
  }
  load_filter(run);
  rc = ng_stream_filter_push(run->filter, kind, text ? text : "", carry, cn,
                            RUN_CARRY, &part);
  if (rc <= 0) {
    free(part);
    return rc;
  }
  if (!streaming) {
    int fr = ng_stream_filter_flush(run->filter, kind, carry, cn, RUN_CARRY, &tail);
    if (fr == 1 && tail && tail[0]) {
      char *both = NULL;
      if (asprintf(&both, "%s%s", part ? part : "", tail) < 0) both = NULL;
      free(part);
      part = both;
    }
    free(tail);
    if (!part) return -1;
  }
  *shown = part ? part : strdup("");
  return *shown ? 1 : -1;
}

int ng_run_emit(ng_run *run, const char *kind, const char *name, const char *text) {
  int filtered = 0;
  char *shown = NULL;
  int rc;
  if (!run || !kind_ok(kind)) return -1;
  filtered = (strcmp(kind, "token") == 0 || strcmp(kind, "thinking") == 0 ||
              strcmp(kind, "tool_result") == 0 || strcmp(kind, "code_change") == 0);
  if (filtered) {
    rc = filter_text(run, kind, text, &shown);
    if (rc <= 0) return rc;
    if (!shown || !shown[0]) {
      free(shown);
      return 1;
    }
    text = shown;
  }
  rc = append_event(run, kind, name, text);
  free(shown);
  return rc;
}

int ng_run_flush(ng_run *run) {
  char *tail = NULL;
  int rc;
  if (!run) return -1;
  load_filter(run);
  rc = ng_stream_filter_flush(run->filter, "token", run->token_carry, &run->token_n,
                             RUN_CARRY, &tail);
  if (rc == 1 && tail && tail[0]) append_event(run, "token", "", tail);
  free(tail);
  tail = NULL;
  rc = ng_stream_filter_flush(run->filter, "thinking", run->think_carry,
                             &run->think_n, RUN_CARRY, &tail);
  if (rc == 1 && tail && tail[0]) append_event(run, "thinking", "", tail);
  free(tail);
  return 0;
}

static int read_control(const ng_run *run, char *buf, size_t n) {
  char path[700];
  FILE *fp;
  if (!run || !buf || n < 8) return -1;
  if (path_join(path, sizeof path, run->dir, "/control") != 0) return -1;
  fp = fopen(path, "r");
  if (!fp) {
    snprintf(buf, n, "run");
    return 0;
  }
  if (!fgets(buf, (int)n, fp)) buf[0] = 0;
  fclose(fp);
  return 0;
}

static int intercept_pending(const ng_run *run) {
  char path[700];
  struct stat st;
  if (path_join(path, sizeof path, run->dir, "/intercept.txt") != 0) return 0;
  if (stat(path, &st) != 0) return 0;
  return st.st_size > 0;
}

int ng_run_poll(ng_run *run, int wait_ms) {
  char buf[32];
  int waited = 0;
  if (!run) return 1;
  for (;;) {
    read_control(run, buf, sizeof buf);
    if (!strncmp(buf, "abort", 5)) return 1;
    if (intercept_pending(run)) return 3;
    if (strncmp(buf, "pause", 5) != 0) return 0;
    if (wait_ms == 0) return 2;
    if (wait_ms > 0 && waited >= wait_ms) return 2;
    {
      struct timespec ts;
      ts.tv_sec = 0;
      ts.tv_nsec = 200 * 1000 * 1000;
      nanosleep(&ts, NULL);
      waited += 200;
    }
  }
}

char *ng_run_take_intercept(ng_run *run) {
  char path[700];
  char *text;
  size_t n = 0;
  if (!run) return NULL;
  if (path_join(path, sizeof path, run->dir, "/intercept.txt") != 0) return NULL;
  text = ng_read_file(path, &n);
  unlink(path);
  if (!text || !text[0]) {
    free(text);
    return NULL;
  }
  if (n > 8000) text[8000] = 0;
  return text;
}

char *ng_run_prompt(const ng_run *run) {
  char path[700];
  if (!run) return NULL;
  if (path_join(path, sizeof path, run->dir, "/prompt.txt") != 0) return NULL;
  return ng_read_file(path, NULL);
}

int ng_run_load_limits(const ng_run *run, ng_run_limits *lim) {
  char path[700];
  char *raw;
  char *model;
  char *effort;
  if (!run || !lim) return -1;
  ng_run_limits_clear(lim);
  if (path_join(path, sizeof path, run->dir, "/config.json") != 0) return -1;
  raw = ng_read_file(path, NULL);
  if (!raw) return -1;
  model = ng_json_get_string(raw, "model");
  effort = ng_json_get_string(raw, "effort");
  if (model) snprintf(lim->model, sizeof lim->model, "%s", model);
  if (effort) snprintf(lim->effort, sizeof lim->effort, "%s", effort);
  lim->has_effort = json_int(raw, "has_effort", 0);
  lim->max_tokens = json_int(raw, "max_tokens", 0);
  lim->context_tokens = json_int(raw, "context_tokens", 0);
  lim->compact_explicit = json_int(raw, "compact_explicit", 0);
  lim->compact_max_refills = json_int(raw, "compact_max_refills", 3);
  lim->compact_keep_tail = json_int(raw, "compact_keep_tail", 2);
  lim->compact_keep_head = json_int(raw, "compact_keep_head", -1);
  lim->compact_shrink_at = json_int(raw, "compact_shrink_at", 1800);
  lim->compact_preemptive = json_int(raw, "compact_preemptive", 0);
  free(model);
  free(effort);
  free(raw);
  return 0;
}

int ng_run_finish(ng_run *run, const char *status, const char *error) {
  const char *st = "error";
  if (!run) return -1;
  if (status && (!strcmp(status, "done") || !strcmp(status, "aborted") ||
                 !strcmp(status, "error")))
    st = status;
  return write_meta(run->id, st, (int)getpid(), error);
}

static int looks_like_edit(const char *tool, const char *args, const char *output) {
  if (output && (strncmp(output, "diff ", 5) == 0 || strstr(output, "\n@@") ||
                 strstr(output, "\n+++ ") || strstr(output, "\n--- ")))
    return 1;
  if (args && (strstr(args, "\"path\"") || strstr(args, "\"file\"") ||
               strstr(args, "\"diff\"") || strstr(args, " > ") ||
               strstr(args, "sed -i") || strstr(args, "tee ") || strstr(args, "patch ")))
    return 1;
  if (tool && (strstr(tool, "edit") || strstr(tool, "write") || strstr(tool, "patch") ||
               strstr(tool, "apply")))
    return 1;
  return 0;
}

int ng_run_note_code(ng_run *run, const char *tool, const char *args, const char *output) {
  char buf[1200];
  if (!run || !looks_like_edit(tool, args, output)) return 0;
  snprintf(buf, sizeof buf, "tool=%s args=%.240s output=%.700s",
           tool ? tool : "", args ? args : "", output ? output : "");
  return ng_run_emit(run, "code_change", tool, buf) == 1;
}

static int write_control(const char *id, const char *word) {
  char path[700];
  char line[32];
  if (run_path(id, "control", path, sizeof path) != 0) return -1;
  snprintf(line, sizeof line, "%s\n", word);
  return write_text(path, line);
}

int ng_run_request_abort(const char *id) { return write_control(id, "abort"); }
int ng_run_request_pause(const char *id) { return write_control(id, "pause"); }
int ng_run_request_resume(const char *id) { return write_control(id, "run"); }

int ng_run_request_intercept(const char *id, const char *text) {
  char path[700], tmp[700];
  const char *p;
  if (!text) return -1;
  p = text;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  if (!*p) return -1;
  if (strlen(text) > 8000) return -1;
  if (run_path(id, "intercept.txt", path, sizeof path) != 0) return -1;
  if (path_join(tmp, sizeof tmp, path, ".new") != 0) return -1;
  if (write_text(tmp, text) != 0) return -1;
  if (rename(tmp, path) != 0) {
    unlink(tmp);
    return -1;
  }
  return 0;
}

int ng_run_set_filter(const char *id, const char *filter_json) {
  char path[700];
  ng_stream_filter *f;
  if (!filter_json || !filter_json[0]) return -1;
  if (strlen(filter_json) > 8000) return -1;
  f = ng_stream_filter_parse(filter_json);
  if (!f) return -1;
  ng_stream_filter_free(f);
  if (run_path(id, "filter.json", path, sizeof path) != 0) return -1;
  return write_text(path, filter_json);
}

int ng_run_set_limits(const char *id, const ng_run_limits *limits) {
  char path[700];
  char *cfg;
  int rc;
  if (!limits) return -1;
  if (limits->model[0]) {
    const char *m;
    for (m = limits->model; *m; m++) {
      if (*m == '"' || *m == '\n' || *m == '\r' || *m == ' ') return -1;
    }
  }
  cfg = limits_json(limits);
  if (!cfg) return -1;
  if (run_path(id, "config.json", path, sizeof path) != 0) {
    free(cfg);
    return -1;
  }
  rc = write_text(path, cfg);
  free(cfg);
  return rc;
}

static long line_seq(const char *line) {
  const char *p = strstr(line, "\"seq\":");
  if (!p) return -1;
  return atol(p + 6);
}

char *ng_run_events_json(const char *id, long since, int limit) {
  char path[700];
  FILE *fp;
  char *body;
  size_t cap = 4096, used = 0;
  int n = 0;
  char line[8192];
  if (!id_ok(id)) {
    return strdup("{\"schema\":\"nanobot.run.v1\",\"ok\":false,\"action\":\"events\","
                  "\"error\":\"bad_id\",\"python\":0}");
  }
  if (limit <= 0) limit = 200;
  if (limit > 500) limit = 500;
  if (run_path(id, "events.jsonl", path, sizeof path) != 0) return NULL;
  body = malloc(cap);
  if (!body) return NULL;
  used = (size_t)snprintf(body, cap,
                          "{\"schema\":\"nanobot.run.v1\",\"ok\":true,\"action\":\"events\","
                          "\"id\":\"%s\",\"since\":%ld,\"events\":[",
                          id, since);
  fp = fopen(path, "r");
  if (fp) {
    while (fgets(line, sizeof line, fp) && n < limit) {
      long seq;
      size_t L;
      char *nl;
      if (line[0] != '{') continue;
      seq = line_seq(line);
      if (since >= 0 && seq <= since) continue;
      nl = strchr(line, '\n');
      if (nl) *nl = 0;
      L = strlen(line);
      if (used + L + 8 >= cap) {
        char *nb;
        cap = cap * 2 + L;
        nb = realloc(body, cap);
        if (!nb) break;
        body = nb;
      }
      if (n) body[used++] = ',';
      memcpy(body + used, line, L);
      used += L;
      body[used] = 0;
      n++;
    }
    fclose(fp);
  }
  if (used + 48 >= cap) {
    char *nb = realloc(body, used + 48);
    if (nb) body = nb;
  }
  snprintf(body + used, 48, "],\"python\":0}");
  return body;
}

char *ng_run_status_json(const char *id) {
  char path[700];
  char *meta;
  char *status;
  char *err;
  ng_run *run;
  ng_run_limits lim;
  char *out = NULL;
  int pid = 0;
  int alive = 0;
  long seq = 0;
  if (!id_ok(id)) {
    return strdup("{\"schema\":\"nanobot.run.v1\",\"ok\":false,\"action\":\"status\","
                  "\"error\":\"bad_id\",\"python\":0}");
  }
  if (run_path(id, "meta.json", path, sizeof path) != 0) return NULL;
  meta = ng_read_file(path, NULL);
  if (!meta) {
    return strdup("{\"schema\":\"nanobot.run.v1\",\"ok\":false,\"action\":\"status\","
                  "\"error\":\"run_not_found\",\"python\":0}");
  }
  status = ng_json_get_string(meta, "status");
  err = ng_json_get_string(meta, "error");
  pid = json_int(meta, "pid", 0);
  if (pid > 0 && kill((pid_t)pid, 0) == 0) alive = 1;
  run = ng_run_open(id);
  if (run) {
    seq = run->seq;
    ng_run_load_limits(run, &lim);
    ng_run_close(run);
  } else {
    ng_run_limits_clear(&lim);
  }
  {
    char *me = ng_json_escape(lim.model);
    char *ee = ng_json_escape(lim.has_effort ? lim.effort : "");
    char *se = ng_json_escape(status ? status : "unknown");
    char *xe = ng_json_escape(err ? err : "");
    if (asprintf(&out,
                 "{\"schema\":\"nanobot.run.v1\",\"ok\":true,\"action\":\"status\","
                 "\"id\":\"%s\",\"status\":\"%s\",\"pid\":%d,\"alive\":%s,\"seq\":%ld,"
                 "\"error\":\"%s\",\"model\":\"%s\",\"reasoning_effort\":\"%s\","
                 "\"max_tokens\":%d,\"context_tokens\":%d,"
                 "\"compact_explicit\":%d,\"compact_max_refills\":%d,"
                 "\"compact_keep_tail\":%d,\"compact_keep_head\":%d,"
                 "\"compact_shrink_at\":%d,\"compact_preemptive\":%d,\"python\":0}",
                 id, se ? se : "", pid, alive ? "true" : "false", seq, xe ? xe : "",
                 me ? me : "", ee ? ee : "", lim.max_tokens, lim.context_tokens,
                 lim.compact_explicit, lim.compact_max_refills, lim.compact_keep_tail,
                 lim.compact_keep_head, lim.compact_shrink_at, lim.compact_preemptive) < 0)
      out = NULL;
    free(me); free(ee); free(se); free(xe);
  }
  free(status);
  free(err);
  free(meta);
  return out ? out
             : strdup("{\"schema\":\"nanobot.run.v1\",\"ok\":false,\"error\":\"oom\","
                      "\"python\":0}");
}

char *ng_run_list_json(void) {
  char root[700];
  DIR *d;
  struct dirent *de;
  char *body;
  size_t cap = 2048, used;
  int n = 0;
  snprintf(root, sizeof root, "%s/runs", ng_workdir());
  body = malloc(cap);
  if (!body) return NULL;
  used = (size_t)snprintf(body, cap,
                          "{\"schema\":\"nanobot.run.v1\",\"ok\":true,\"action\":\"list\","
                          "\"runs\":[");
  d = opendir(root);
  if (d) {
    while ((de = readdir(d)) != NULL && n < 32) {
      char *st;
      size_t L;
      if (!id_ok(de->d_name)) continue;
      st = ng_run_status_json(de->d_name);
      if (!st) continue;
      L = strlen(st);
      if (used + L + 8 >= cap) {
        char *nb = realloc(body, cap * 2 + L);
        if (!nb) {
          free(st);
          break;
        }
        cap = cap * 2 + L;
        body = nb;
      }
      if (n) body[used++] = ',';
      memcpy(body + used, st, L);
      used += L;
      body[used] = 0;
      free(st);
      n++;
    }
    closedir(d);
  }
  if (used + 32 >= cap) {
    char *nb = realloc(body, used + 32);
    if (nb) body = nb;
  }
  snprintf(body + used, 32, "],\"python\":0}");
  return body;
}

int ng_run_is_route(const char *path, char *id, size_t idsz, char *action,
                    size_t asz, long *since) {
  static const char *prefs[] = {"/api/v1/runs", "/peer/v1/runs", "/api/runs", NULL};
  const char *rest = NULL;
  char seg[64];
  size_t n;
  int i;
  if (id && idsz) id[0] = 0;
  if (action && asz) action[0] = 0;
  if (since) *since = -1;
  if (!path || !id || idsz < 2 || !action || asz < 2) return 0;
  for (i = 0; prefs[i]; i++) {
    size_t pl = strlen(prefs[i]);
    if (!strncmp(path, prefs[i], pl) && (path[pl] == 0 || path[pl] == '/')) {
      rest = path + pl;
      break;
    }
  }
  if (!rest) return 0;
  while (*rest == '/') rest++;
  if (!*rest) return 1;
  n = 0;
  while (rest[n] && rest[n] != '/' && n + 1 < sizeof seg) {
    seg[n] = rest[n];
    n++;
  }
  seg[n] = 0;
  if (!id_ok(seg) || strlen(seg) + 1 > idsz) return 0;
  snprintf(id, idsz, "%s", seg);
  rest += n;
  while (*rest == '/') rest++;
  if (!*rest) return 1;
  n = 0;
  while (rest[n] && rest[n] != '/' && n + 1 < sizeof seg) {
    seg[n] = rest[n];
    n++;
  }
  seg[n] = 0;
  if (strcmp(seg, "events") && strcmp(seg, "stream") && strcmp(seg, "abort") &&
      strcmp(seg, "pause") && strcmp(seg, "resume") && strcmp(seg, "intercept") &&
      strcmp(seg, "filter") && strcmp(seg, "config"))
    return 0;
  if (strlen(seg) + 1 > asz) return 0;
  snprintf(action, asz, "%s", seg);
  rest += n;
  while (*rest == '/') rest++;
  if (!*rest) return 1;
  if ((strcmp(action, "events") != 0 && strcmp(action, "stream") != 0) || !isdigit((unsigned char)*rest))
    return 0;
  if (since) *since = atol(rest);
  while (isdigit((unsigned char)*rest)) rest++;
  return *rest == 0;
}

int ng_run_tail(const char *id, long since, int timeout_ms, ng_run_line_fn fn, void *ud) {
  char path[700];
  int waited = 0;
  int delivered = 0;
  long cursor = since;
  if (!id_ok(id) || !fn) return 0;
  if (run_path(id, "events.jsonl", path, sizeof path) != 0) return 0;
  for (;;) {
    FILE *fp = fopen(path, "r");
    int saw = 0;
    if (fp) {
      char line[8192];
      while (fgets(line, sizeof line, fp)) {
        long seq;
        char *nl;
        if (line[0] != '{') continue;
        seq = line_seq(line);
        if (cursor >= 0 && seq <= cursor) continue;
        nl = strchr(line, '\n');
        if (nl) *nl = 0;
        fn(ud, line);
        if (seq > cursor) cursor = seq;
        delivered++;
        saw = 1;
      }
      fclose(fp);
    }
    {
      char *st = ng_run_status_json(id);
      int terminal = st && (strstr(st, "\"status\":\"done\"") ||
                            strstr(st, "\"status\":\"aborted\"") ||
                            strstr(st, "\"status\":\"error\""));
      free(st);
      if (terminal && !saw) return delivered;
    }
    if (timeout_ms == 0) return delivered;
    if (timeout_ms > 0 && waited >= timeout_ms) return delivered;
    {
      struct timespec ts;
      ts.tv_sec = 0;
      ts.tv_nsec = 50 * 1000 * 1000;
      nanosleep(&ts, NULL);
      waited += 50;
    }
  }
}
