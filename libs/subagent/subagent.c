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

int ng_subagent_running_count(void) {
  char d[512];
  sub_dir(d, sizeof d);
  DIR *dp = opendir(d);
  if (!dp) return 0;
  int n = 0;
  struct dirent *de;
  while ((de = readdir(dp)) != NULL) {
    size_t L = strlen(de->d_name);
    if (L < 6 || strcmp(de->d_name + L - 5, ".json") != 0) continue;
    char path[600];
    snprintf(path, sizeof path, "%s/%s", d, de->d_name);
    char *j = ng_read_file(path, NULL);
    if (j && status_is_live(j)) n++;
    free(j);
  }
  closedir(dp);
  return n;
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

  pid_t p = fork();
  if (p < 0) {
    write_meta(id, t, desc, "error", 0, "fork failed");
    return NULL;
  }
  if (p == 0) {
    write_meta(id, t, desc, "running", (int)getpid(), NULL);
    char *pr = ng_read_file(ip, NULL);
    /* explore/plan: soft prefix — tools allowed (shell); no further subagents */
    char *full = pr;
    if (pr && !strcmp(t, "explore")) {
      asprintf(&full,
        "[subagent type=explore — USE run_terminal_command to gather facts, "
        "then write a short factual report. Do not invent numbers. "
        "No destructive changes.]\n%s", pr);
      free(pr);
    } else if (pr && !strcmp(t, "plan")) {
      asprintf(&full,
        "[subagent type=plan — reason and structure; use shell only if needed "
        "for a quick check. End with a clear summary.]\n%s", pr);
      free(pr);
    } else if (pr) {
      asprintf(&full,
        "[subagent type=general — complete the assigned part; use shell when "
        "facts are needed; end with a concise summary.]\n%s", pr);
      free(pr);
    }
    char *reply = run_fn(agent_cfg, full ? full : "");
    free(full);
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
    write_meta(id, t, desc, reply ? "done" : "error", (int)getpid(),
               reply ? NULL : "empty reply");
    free(reply);
    _exit(0);
  }
  write_meta(id, t, desc, "running", (int)p, NULL);
  return strdup(id);
}

char *ng_subagent_status_json(const char *id) {
  if (!valid_id(id)) return strdup("{\"error\":\"bad id\"}");
  char path[600], op[600];
  meta_path(path, sizeof path, id);
  char *meta = ng_read_file(path, NULL);
  if (!meta) return strdup("{\"error\":\"not found\"}");
  /* reaping: if pid dead and still running, mark error */
  char *ps = strstr(meta, "\"pid\":");
  if (ps && status_is_live(meta)) {
    int pid = atoi(ps + 6);
    if (pid > 1 && kill(pid, 0) != 0 && errno == ESRCH) {
      /* finished without update? check out */
      out_path(op, sizeof op, id);
      if (access(op, R_OK) == 0)
        write_meta(id, "general", "", "done", pid, NULL);
      else
        write_meta(id, "general", "", "error", pid, "exited");
      free(meta);
      meta = ng_read_file(path, NULL);
    }
  }
  out_path(op, sizeof op, id);
  char *out = ng_read_file(op, NULL);
  char *esc = ng_json_escape(out ? out : "");
  /* splice result into meta roughly */
  size_t ml = strlen(meta);
  if (ml && meta[ml - 1] == '}') meta[ml - 1] = 0;
  char *j = NULL;
  asprintf(&j, "%s,\"result\":\"%s\"}", meta, esc ? esc : "");
  free(meta); free(out); free(esc);
  return j ? j : strdup("{\"error\":\"oom\"}");
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
      return strdup("{\"error\":\"need prompt\"}");
    }
    if (ng_subagent_running_count() >= g_max) {
      free(prompt); free(desc); free(type);
      char *e = NULL;
      asprintf(&e, "{\"error\":\"subagent limit %d (share session)\",\"max\":%d}",
               g_max, g_max);
      return e ? e : strdup("{\"error\":\"limit\"}");
    }
    char *id = ng_subagent_spawn(agent_cfg, run_fn, type, desc, prompt);
    free(prompt); free(desc); free(type);
    if (!id) return strdup("{\"error\":\"spawn failed\"}");
    char *j = NULL;
    asprintf(&j, "{\"ok\":true,\"id\":\"%s\",\"poll\":\"subagent_status\"}", id);
    free(id);
    return j ? j : strdup("{\"ok\":true}");
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
    return rc == 0 ? strdup("{\"ok\":true,\"cancelled\":true}")
                   : strdup("{\"error\":\"cancel failed\"}");
  }
  return NULL;
}
