#include "subagent.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <dirent.h>
#include <fcntl.h>
#include <time.h>
#include <ctype.h>
#include <signal.h>
#include <errno.h>

extern const char *ng_workdir(void);
extern char *ng_read_file(const char *path, size_t *out_len);
extern int ng_write_file(const char *path, const char *data, size_t n);
extern char *ng_json_get_string(const char *json, const char *key);
extern char *ng_json_escape(const char *s);
extern char *ng_settings_get(const char *key);

static int g_en = 0;
static int g_max = 8;

/* Dual-wire subagent tool plate — machine error/action tokens only. */
static char *sub_err(const char *error, const char *extra_json) {
  char *out = NULL;
  const char *e = error && error[0] ? error : "subagent_failed";
  asprintf(&out,
           "{\"schema\":\"nanobot.subagent.v1\",\"ok\":false,\"error\":\"%s\","
           "\"python\":0%s}",
           e, extra_json ? extra_json : "");
  return out ? out
             : strdup("{\"schema\":\"nanobot.subagent.v1\",\"ok\":false,"
                      "\"error\":\"oom\",\"python\":0}");
}

static char *sub_ok(const char *action, const char *extra_json) {
  char *out = NULL;
  const char *a = action && action[0] ? action : "ok";
  asprintf(&out,
           "{\"schema\":\"nanobot.subagent.v1\",\"ok\":true,\"action\":\"%s\","
           "\"python\":0%s}",
           a, extra_json ? extra_json : "");
  return out ? out : sub_err("oom", NULL);
}

void ng_subagent_configure(int enabled, int max_slots) {
  g_en = enabled ? 1 : 0;
  g_max = max_slots;
  if (g_max < 0) g_max = 0;
  if (g_max > NG_SUBAGENT_MAX) g_max = NG_SUBAGENT_MAX;
}
int ng_subagent_enabled(void) { return g_en; }
int ng_subagent_max(void) { return g_max; }

static void sub_dir(char *out, size_t n) {
  snprintf(out, n, "%s/subagents", ng_workdir());
  mkdir(out, 0755);
}

static void meta_path(char *out, size_t n, const char *id) {
  char d[512];
  sub_dir(d, sizeof d);
  snprintf(out, n, "%s/%s.json", d, id);
}

static void out_path(char *out, size_t n, const char *id) {
  char d[512];
  sub_dir(d, sizeof d);
  snprintf(out, n, "%s/%s.out", d, id);
}

static void in_path(char *out, size_t n, const char *id) {
  char d[512];
  sub_dir(d, sizeof d);
  snprintf(out, n, "%s/%s.in", d, id);
}

static int valid_id(const char *id) {
  size_t L;
  if (!id || !id[0]) return 0;
  L = strlen(id);
  if (L >= NG_SUBAGENT_ID_LEN) return 0;
  for (const char *p = id; *p; p++)
    if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') return 0;
  return 1;
}

static int status_is_live(const char *json) {
  if (!json) return 0;
  if (strstr(json, "\"status\":\"running\"") || strstr(json, "\"status\": \"running\""))
    return 1;
  if (strstr(json, "\"status\":\"queued\"") || strstr(json, "\"status\": \"queued\""))
    return 1;
  return 0;
}

/* True if pid is a live non-zombie process. */
static int pid_is_alive(int pid) {
  char stpath[80], stline[256];
  if (pid <= 1) return 0;
  snprintf(stpath, sizeof stpath, "/proc/%d/stat", pid);
  FILE *sf = fopen(stpath, "r");
  if (!sf) return 0;
  if (!fgets(stline, sizeof stline, sf)) {
    fclose(sf);
    return 0;
  }
  fclose(sf);
  /* format: pid (comm) state ... — state Z = zombie (not alive) */
  char *rp = strrchr(stline, ')');
  if (rp && rp[1] == ' ' && rp[2] == 'Z') return 0;
  if (kill(pid, 0) != 0 && errno == ESRCH) return 0;
  return 1;
}

static int meta_pid(const char *json) {
  char *ps;
  if (!json) return 0;
  ps = strstr(json, "\"pid\":");
  if (!ps) return 0;
  return atoi(ps + 6);
}

static void write_meta(const char *id, const char *type, const char *desc,
                       const char *status, int pid, const char *err) {
  char path[600];
  meta_path(path, sizeof path, id);
  char *et = ng_json_escape(type ? type : "general");
  char *ed = ng_json_escape(desc ? desc : "");
  char *es = ng_json_escape(status ? status : "queued");
  char *ee = ng_json_escape(err ? err : "");
  char buf[1200];
  snprintf(buf, sizeof buf,
    "{\"id\":\"%s\",\"type\":\"%s\",\"description\":\"%s\","
    "\"status\":\"%s\",\"pid\":%d,\"error\":\"%s\"}",
    id, et ? et : "", ed ? ed : "", es ? es : "", pid, ee ? ee : "");
  ng_write_file(path, buf, strlen(buf));
  free(et); free(ed); free(es); free(ee);
}

/* Reap one meta file if process gone; returns 1 if still truly running. */
static int reap_meta_file(const char *path, const char *id) {
  char *meta = ng_read_file(path, NULL);
  char op[600];
  int pid;
  if (!meta) return 0;
  if (!status_is_live(meta)) {
    free(meta);
    return 0;
  }
  pid = meta_pid(meta);
  if (pid > 1 && pid_is_alive(pid)) {
    free(meta);
    return 1;
  }
  /* dead / zombie / never started */
  out_path(op, sizeof op, id);
  if (access(op, R_OK) == 0)
    write_meta(id, "general", "", "done", pid, NULL);
  else
    write_meta(id, "general", "", "error", pid, "exited");
  free(meta);
  return 0;
}

int ng_subagent_reap_all(void) {
  char d[512];
  DIR *dp;
  struct dirent *de;
  int still = 0;
  sub_dir(d, sizeof d);
  dp = opendir(d);
  if (!dp) return 0;
  while ((de = readdir(dp)) != NULL) {
    size_t L = strlen(de->d_name);
    char id[NG_SUBAGENT_ID_LEN];
    char path[600];
    if (L < 6 || strcmp(de->d_name + L - 5, ".json") != 0) continue;
    snprintf(id, sizeof id, "%.*s", (int)(L - 5), de->d_name);
    if (!valid_id(id)) continue;
    snprintf(path, sizeof path, "%s/%s", d, de->d_name);
    if (reap_meta_file(path, id)) still++;
  }
  closedir(dp);
  /* non-blocking wait: clear any direct-child zombies (intermediate forks) */
  while (waitpid(-1, NULL, WNOHANG) > 0) {
  }
  return still;
}

int ng_subagent_running_count(void) {
  /* Always reap first so count matches real processes */
  return ng_subagent_reap_all();
}

char *ng_subagent_spawn(void *agent_cfg, ng_subagent_run_fn run_fn,
                        const char *type, const char *desc, const char *prompt) {
  if (!g_en) return NULL;
  if (!run_fn || !prompt || !prompt[0]) return NULL;
  if (ng_subagent_running_count() >= g_max) return NULL;

  const char *t = type && type[0] ? type : "general";
  if (strcmp(t, "explore") && strcmp(t, "plan") && strcmp(t, "general") &&
      strcmp(t, "general-purpose"))
    t = "general";

  char id[NG_SUBAGENT_ID_LEN];
  snprintf(id, sizeof id, "sa%ld%03d", (long)time(NULL), (int)(getpid() % 1000));

  char ip[600];
  in_path(ip, sizeof ip, id);
  /* cap prompt */
  int maxp = 6000;
  {
    char *s = ng_settings_get("MAX_SUB_PROMPT");
    if (s) { int v = atoi(s); if (v >= 256 && v <= 32000) maxp = v; free(s); }
  }
  size_t plen = strlen(prompt);
  if ((int)plen > maxp) plen = (size_t)maxp;
  ng_write_file(ip, prompt, plen);

  write_meta(id, t, desc, "queued", 0, NULL);

  /* Max wall time for a subagent — self-terminates so main nanobot stays idle. */
  int max_sec = 90;
  {
    char *s = ng_settings_get("SUBAGENT_TIMEOUT_SEC");
    if (!s) s = ng_settings_get("MAX_SUB_SEC");
    if (s) {
      int v = atoi(s);
      if (v >= 15 && v <= 600) max_sec = v;
      free(s);
    }
  }

  /*
   * Double-fork: intermediate exits immediately; worker is reparented to init.
   * Main nanobot never accumulates subagent zombies; worker self-exits after work.
   */
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    write_meta(id, t, desc, "error", 0, "pipe_failed");
    return NULL;
  }
  pid_t p = fork();
  if (p < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    write_meta(id, t, desc, "error", 0, "fork_failed");
    return NULL;
  }
  if (p == 0) {
    /* intermediate */
    close(pipefd[0]);
    pid_t p2 = fork();
    if (p2 < 0) {
      close(pipefd[1]);
      _exit(1);
    }
    if (p2 > 0) {
      /* intermediate exits → parent waitpid reaps us; worker continues */
      close(pipefd[1]);
      _exit(0);
    }
    /* ── worker (grandchild) ─────────────────────────────────────── */
    int mypid = (int)getpid();
    {
      ssize_t w = write(pipefd[1], &mypid, sizeof mypid);
      (void)w;
    }
    close(pipefd[1]);
    setsid();
    /* hard self-timeout: always terminate */
    signal(SIGALRM, SIG_DFL); /* default = kill process */
    alarm((unsigned)max_sec);

    write_meta(id, t, desc, "running", mypid, NULL);
    char *pr = ng_read_file(ip, NULL);
    char *full = pr;
    /* Dual-wire role plate prefix — machine must/forbid tokens (no free-text essay). */
    if (pr) {
      const char *must = "complete_and_stop";
      const char *forbid = "invent";
      if (!strcmp(t, "explore")) {
        must = "facts_via_shell";
        forbid = "invent|destructive";
      } else if (!strcmp(t, "plan")) {
        must = "structure_and_summary";
        forbid = "invent";
      }
      asprintf(&full,
               "{\"schema\":\"nanobot.subagent.v1\",\"ok\":true,"
               "\"role\":\"%s\",\"must\":\"%s\",\"forbid\":\"%s\","
               "\"next\":\"stop\",\"python\":0}\n%s",
               t, must, forbid, pr);
      free(pr);
    }
    char *reply = run_fn(agent_cfg, full ? full : "");
    free(full);
    alarm(0);
    int maxr = 12000;
    {
      char *s = ng_settings_get("MAX_SUB_REPLY");
      if (s) { int v = atoi(s); if (v >= 256 && v <= 64000) maxr = v; free(s); }
    }
    if (reply && (int)strlen(reply) > maxr) reply[maxr] = 0;
    char op[600];
    out_path(op, sizeof op, id);
    if (reply) ng_write_file(op, reply, strlen(reply));
    else ng_write_file(op, "", 0);
    write_meta(id, t, desc, reply ? "done" : "error", mypid,
               reply ? NULL : "empty_reply");
    free(reply);
    /* hard exit — do not return into parent agent stack */
    _exit(reply ? 0 : 1);
  }
  /* parent of intermediate */
  close(pipefd[1]);
  int real_pid = 0;
  {
    ssize_t r = read(pipefd[0], &real_pid, sizeof real_pid);
    if (r != (ssize_t)sizeof real_pid) real_pid = 0;
  }
  close(pipefd[0]);
  /* reap intermediate immediately (never leave zombie under HTTP worker) */
  {
    int st = 0;
    waitpid(p, &st, 0);
  }
  if (real_pid <= 1) {
    write_meta(id, t, desc, "error", 0, "worker_pid_unknown");
    return NULL;
  }
  write_meta(id, t, desc, "running", real_pid, NULL);
  return strdup(id);
}

char *ng_subagent_status_json(const char *id) {
  if (!valid_id(id)) return sub_err("bad_id", NULL);
  char path[600], op[600];
  meta_path(path, sizeof path, id);
  /* reap this id if dead */
  reap_meta_file(path, id);
  char *meta = ng_read_file(path, NULL);
  if (!meta) return sub_err("not_found", NULL);
  out_path(op, sizeof op, id);
  char *out = ng_read_file(op, NULL);
  char *esc = ng_json_escape(out ? out : "");
  /* splice dual-wire schema + result into meta object */
  size_t ml = strlen(meta);
  if (ml && meta[ml - 1] == '}') meta[ml - 1] = 0;
  char *j = NULL;
  asprintf(&j,
           "%s,\"schema\":\"nanobot.subagent.v1\",\"ok\":true,"
           "\"action\":\"status\",\"python\":0,\"result\":\"%s\"}",
           meta, esc ? esc : "");
  free(meta); free(out); free(esc);
  return j ? j : sub_err("oom", NULL);
}

char *ng_subagent_list_json(void) {
  char d[512];
  sub_dir(d, sizeof d);
  DIR *dp = opendir(d);
  char *acc = strdup("[");
  if (!dp) {
    char *e = strdup("[]");
    free(acc);
    return e;
  }
  int first = 1;
  struct dirent *de;
  while ((de = readdir(dp)) != NULL) {
    size_t L = strlen(de->d_name);
    if (L < 6 || strcmp(de->d_name + L - 5, ".json") != 0) continue;
    char id[NG_SUBAGENT_ID_LEN];
    snprintf(id, sizeof id, "%.*s", (int)(L - 5), de->d_name);
    if (!valid_id(id)) continue;
    char *one = ng_subagent_status_json(id);
    if (!one) continue;
    char *nacc = NULL;
    asprintf(&nacc, "%s%s%s", acc, first ? "" : ",", one);
    free(acc); free(one);
    acc = nacc ? nacc : strdup("[]");
    first = 0;
  }
  closedir(dp);
  char *out = NULL;
  asprintf(&out, "%s]", acc ? acc : "[");
  free(acc);
  return out ? out : strdup("[]");
}

int ng_subagent_cancel(const char *id) {
  if (!valid_id(id)) return -1;
  char path[600];
  meta_path(path, sizeof path, id);
  char *meta = ng_read_file(path, NULL);
  if (!meta) return -1;
  char *ps = strstr(meta, "\"pid\":");
  int pid = ps ? atoi(ps + 6) : 0;
  if (pid > 1) {
    kill(pid, SIGTERM);
    sleep(1);
    kill(pid, SIGKILL);
  }
  write_meta(id, "general", "", "cancelled", pid, NULL);
  free(meta);
  return 0;
}

char *ng_subagent_openai_tools_fragment(void) {
  if (!g_en) return strdup("");
  return strdup(
    ",{\"type\":\"function\",\"function\":{"
    "\"name\":\"subagent_spawn\","
    "\"description\":\"Start a light subagent (max budget shared with siblings; same session). Types: general, explore, plan. Returns id to poll.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"prompt\":{\"type\":\"string\"},"
    "\"description\":{\"type\":\"string\"},"
    "\"type\":{\"type\":\"string\",\"description\":\"general|explore|plan\"}"
    "},\"required\":[\"prompt\"]}}}"
    ",{\"type\":\"function\",\"function\":{"
    "\"name\":\"subagent_status\","
    "\"description\":\"Get status/result of a subagent by id.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"id\":{\"type\":\"string\"}"
    "},\"required\":[\"id\"]}}}"
    ",{\"type\":\"function\",\"function\":{"
    "\"name\":\"subagent_list\","
    "\"description\":\"List subagents for this host.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{}}}}"
  );
}

char *ng_subagent_try_tool(void *agent_cfg, ng_subagent_run_fn run_fn,
                           const char *name, const char *args_json) {
  if (!g_en || !name) return NULL;
  if (!strcmp(name, "subagent_spawn")) {
    char *prompt = ng_json_get_string(args_json, "prompt");
    char *desc = ng_json_get_string(args_json, "description");
    char *type = ng_json_get_string(args_json, "type");
    if (!type) type = ng_json_get_string(args_json, "subagent_type");
    if (!prompt || !prompt[0]) {
      free(prompt); free(desc); free(type);
      return sub_err("need_prompt", NULL);
    }
    if (ng_subagent_running_count() >= g_max) {
      char extra[48];
      free(prompt); free(desc); free(type);
      snprintf(extra, sizeof extra, ",\"max\":%d", g_max);
      return sub_err("limit", extra);
    }
    char *id = ng_subagent_spawn(agent_cfg, run_fn, type, desc, prompt);
    free(prompt); free(desc); free(type);
    if (!id) return sub_err("spawn_failed", NULL);
    {
      char *eid = ng_json_escape(id);
      char extra[120];
      char *j;
      snprintf(extra, sizeof extra, ",\"id\":\"%s\",\"poll\":\"subagent_status\"",
               eid ? eid : "");
      j = sub_ok("spawned", extra);
      free(eid);
      free(id);
      return j;
    }
  }
  if (!strcmp(name, "subagent_status")) {
    char *id = ng_json_get_string(args_json, "id");
    char *j = ng_subagent_status_json(id ? id : "");
    free(id);
    return j;
  }
  if (!strcmp(name, "subagent_list")) {
    return ng_subagent_list_json();
  }
  if (!strcmp(name, "subagent_cancel")) {
    char *id = ng_json_get_string(args_json, "id");
    int rc = ng_subagent_cancel(id ? id : "");
    free(id);
    return rc == 0 ? sub_ok("cancelled", NULL) : sub_err("cancel_failed", NULL);
  }
  return NULL;
}
