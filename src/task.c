#include "task.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <ctype.h>
#include <unistd.h>

#define TASK_DIR_REL "tasks"
#define TASK_ACTIVE "active.json"
#define TASK_MAX_STEPS 24
#define TASK_MAX_NOTE 400
#define TASK_EXTRA_TURNS 12
#define TASK_HARD_MAX 36

static void tasks_dir(char *out, size_t n) {
  snprintf(out, n, "%s/%s", ng_workdir(), TASK_DIR_REL);
  mkdir(out, 0755);
}

static void active_path(char *out, size_t n) {
  char d[640];
  tasks_dir(d, sizeof d);
  snprintf(out, n, "%s/%s", d, TASK_ACTIVE);
}

static char *load_active(void) {
  char p[700];
  active_path(p, sizeof p);
  return ng_read_file(p, NULL);
}

static int save_active(const char *json) {
  char p[700];
  active_path(p, sizeof p);
  if (!json) return -1;
  return ng_write_file(p, json, strlen(json));
}

static void archive_active(const char *json) {
  char d[640], dest[700];
  tasks_dir(d, sizeof d);
  snprintf(dest, sizeof dest, "%s/done-%ld.json", d, (long)time(NULL));
  if (json) ng_write_file(dest, json, strlen(json));
}

/* naive field helpers on our small JSON */
static char *field_str(const char *j, const char *key) {
  return ng_json_get_string(j, key);
}

static int field_has_status(const char *j, const char *want) {
  char *s = field_str(j, "status");
  int ok = (s && strcmp(s, want) == 0);
  free(s);
  return ok;
}

int ng_task_is_open(void) {
  char *j = load_active();
  if (!j || !j[0]) { free(j); return 0; }
  int open = field_has_status(j, "planned") || field_has_status(j, "active");
  free(j);
  return open;
}

int ng_task_extra_turns(void) {
  return TASK_EXTRA_TURNS;
}

int ng_task_hard_max_turns(void) {
  int m = ng_max_turns() + TASK_EXTRA_TURNS;
  if (m > TASK_HARD_MAX) m = TASK_HARD_MAX;
  if (m < 4) m = 4;
  return m;
}

/* Count "done":true vs step objects roughly */
static void count_steps(const char *j, int *total, int *done) {
  *total = 0;
  *done = 0;
  if (!j) return;
  const char *p = strstr(j, "\"steps\"");
  if (!p) return;
  p = strchr(p, '[');
  if (!p) return;
  p++;
  while (*p && *p != ']') {
    while (*p && *p != '{' && *p != ']') p++;
    if (*p != '{') break;
    const char *end = strchr(p, '}');
    if (!end) break;
    (*total)++;
    if (strstr(p, "\"done\":true") || strstr(p, "\"done\": true"))
      (*done)++;
    p = end + 1;
  }
}

char *ng_task_reminder_text(void) {
  char *j = load_active();
  if (!j || !j[0]) { free(j); return NULL; }
  if (!(field_has_status(j, "planned") || field_has_status(j, "active"))) {
    free(j);
    return NULL;
  }
  char *goal = field_str(j, "goal");
  char *id = field_str(j, "status");
  int tot = 0, done = 0;
  count_steps(j, &tot, &done);
  char *out = NULL;
  asprintf(&out,
    "[ACTIVE TASK — do not stop until finished or blocked]\n"
    "status=%s  steps_done=%d/%d\n"
    "goal: %s\n"
    "Instructions:\n"
    "1) Prefer run_terminal_command / mcp tools to make real progress.\n"
    "2) Call task_step_done after each completed step.\n"
    "3) Call task_done with a short summary only when the goal is fully met.\n"
    "4) Call task_block with a reason only if truly stuck (dead end).\n"
    "5) Do NOT claim done without task_done. Keep working this turn.\n"
    "State JSON (truncated): %.800s",
    id ? id : "?", done, tot, goal ? goal : "(no goal)", j);
  free(goal); free(id); free(j);
  return out;
}

char *ng_task_openai_tools_fragment(void) {
  /* Leading comma + tool objects for splicing into tools array */
  return strdup(
    ",{\"type\":\"function\",\"function\":{"
    "\"name\":\"task_plan\","
    "\"description\":\"Create or replace the active multi-step task plan. Use for any goal needing more than one action. Steps are ordered work items.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"goal\":{\"type\":\"string\",\"description\":\"What success looks like\"},"
    "\"steps\":{\"type\":\"string\",\"description\":\"Ordered steps separated by newlines or semicolons\"}"
    "},\"required\":[\"goal\"]}}}"
    ",{\"type\":\"function\",\"function\":{"
    "\"name\":\"task_start\","
    "\"description\":\"Mark the planned task as active and begin executing steps. Call after task_plan.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{}}}}"
    ",{\"type\":\"function\",\"function\":{"
    "\"name\":\"task_step_done\","
    "\"description\":\"Mark one plan step complete after you actually finished it.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"index\":{\"type\":\"integer\",\"description\":\"0-based step index\"},"
    "\"note\":{\"type\":\"string\",\"description\":\"optional evidence / result\"}"
    "},\"required\":[\"index\"]}}}"
    ",{\"type\":\"function\",\"function\":{"
    "\"name\":\"task_done\","
    "\"description\":\"Mark the entire active task finished. Only after the goal is truly complete.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"summary\":{\"type\":\"string\",\"description\":\"What was achieved\"}"
    "},\"required\":[\"summary\"]}}}"
    ",{\"type\":\"function\",\"function\":{"
    "\"name\":\"task_block\","
    "\"description\":\"Declare a dead end: cannot complete the task with available tools/access. Stops the self-reminder loop.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"reason\":{\"type\":\"string\"}"
    "},\"required\":[\"reason\"]}}}"
    ",{\"type\":\"function\",\"function\":{"
    "\"name\":\"task_status\","
    "\"description\":\"Read the active task plan and progress.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{}}}}"
  );
}

static char *json_get_int_str(const char *args, const char *key, int *out) {
  char pat[64];
  snprintf(pat, sizeof pat, "\"%s\"", key);
  const char *p = strstr(args ? args : "", pat);
  if (!p) return NULL;
  p = strchr(p + strlen(pat), ':');
  if (!p) return NULL;
  p++;
  while (*p == ' ' || *p == '\t') p++;
  if (!(*p == '-' || (*p >= '0' && *p <= '9'))) return NULL;
  *out = atoi(p);
  return (char *)p;
}

static void split_steps(const char *steps_raw, char lines[][240], int *nlines) {
  *nlines = 0;
  if (!steps_raw || !steps_raw[0]) return;
  char buf[2048];
  snprintf(buf, sizeof buf, "%s", steps_raw);
  /* normalize ; to newlines */
  for (char *q = buf; *q; q++)
    if (*q == ';') *q = '\n';
  char *save = NULL;
  char *tok = strtok_r(buf, "\n", &save);
  while (tok && *nlines < TASK_MAX_STEPS) {
    while (*tok == ' ' || *tok == '\t') tok++;
    size_t L = strlen(tok);
    while (L && (tok[L - 1] == ' ' || tok[L - 1] == '\t' || tok[L - 1] == '\r'))
      tok[--L] = 0;
    if (L) {
      snprintf(lines[*nlines], 240, "%s", tok);
      (*nlines)++;
    }
    tok = strtok_r(NULL, "\n", &save);
  }
}

static char *tool_plan(const char *args) {
  char *goal = ng_json_get_string(args, "goal");
  char *steps = ng_json_get_string(args, "steps");
  if (!goal || !goal[0]) {
    free(goal); free(steps);
    return strdup("error: task_plan needs goal");
  }
  char lines[TASK_MAX_STEPS][240];
  int n = 0;
  split_steps(steps, lines, &n);
  if (n == 0) {
    snprintf(lines[0], 240, "Investigate and complete: %.200s", goal);
    n = 1;
  }
  char id[40];
  snprintf(id, sizeof id, "t%ld", (long)time(NULL));
  /* build JSON */
  size_t cap = 4096;
  char *json = malloc(cap);
  if (!json) { free(goal); free(steps); return strdup("oom"); }
  int o = snprintf(json, cap,
    "{\"id\":\"%s\",\"status\":\"planned\",\"goal\":", id);
  char *eg = ng_json_escape(goal);
  o += snprintf(json + o, cap - (size_t)o, "\"%s\",\"steps\":[", eg ? eg : "");
  free(eg);
  for (int i = 0; i < n; i++) {
    char *es = ng_json_escape(lines[i]);
    o += snprintf(json + o, cap - (size_t)o,
                  "%s{\"i\":%d,\"text\":\"%s\",\"done\":false}",
                  i ? "," : "", i, es ? es : "");
    free(es);
  }
  o += snprintf(json + o, cap - (size_t)o,
                "],\"updated\":%ld,\"notes\":[]}", (long)time(NULL));
  (void)o;
  save_active(json);
  char *reply = NULL;
  asprintf(&reply,
           "ok: planned task %s with %d steps (status=planned). Call task_start next.\n%s",
           id, n, json);
  free(json); free(goal); free(steps);
  return reply ? reply : strdup("ok planned");
}

static char *extract_steps_array(const char *j) {
  const char *sp = strstr(j, "\"steps\"");
  if (!sp) return strdup("[]");
  const char *arr = strchr(sp, '[');
  if (!arr) return strdup("[]");
  int depth = 0;
  const char *t;
  for (t = arr; *t; t++) {
    if (*t == '[') depth++;
    else if (*t == ']') {
      depth--;
      if (depth == 0) {
        t++;
        break;
      }
    }
  }
  size_t n = (size_t)(t - arr);
  char *out = malloc(n + 1);
  if (!out) return strdup("[]");
  memcpy(out, arr, n);
  out[n] = 0;
  return out;
}

static char *rebuild_task(const char *id, const char *status, const char *goal,
                          const char *steps_json, const char *extra_kv) {
  char *eg = ng_json_escape(goal ? goal : "");
  char *out = NULL;
  asprintf(&out,
    "{\"id\":\"%s\",\"status\":\"%s\",\"goal\":\"%s\",\"steps\":%s,\"updated\":%ld%s}",
    id && id[0] ? id : "task",
    status ? status : "active",
    eg ? eg : "",
    steps_json && steps_json[0] ? steps_json : "[]",
    (long)time(NULL),
    extra_kv ? extra_kv : "");
  free(eg);
  return out;
}

static char *tool_start(void) {
  char *j = load_active();
  if (!j || !j[0]) {
    free(j);
    return strdup("error: no plan — call task_plan first");
  }
  char *goal = field_str(j, "goal");
  char *id = field_str(j, "id");
  char *steps = extract_steps_array(j);
  free(j);
  char *out = rebuild_task(id, "active", goal, steps, ",\"notes\":[]");
  free(steps);
  if (!out) {
    free(goal); free(id);
    return strdup("oom");
  }
  save_active(out);
  char *reply = NULL;
  asprintf(&reply,
    "ok: task ACTIVE. Execute steps in order; after each real action call "
    "task_step_done. When goal met call task_done; if impossible call task_block.\n"
    "goal: %s\n%s",
    goal ? goal : "?", out);
  free(out); free(goal); free(id);
  return reply ? reply : strdup("ok active");
}

static char *tool_step_done(const char *args) {
  int idx = -1;
  json_get_int_str(args, "index", &idx);
  char *note = ng_json_get_string(args, "note");
  if (idx < 0) {
    free(note);
    return strdup("error: task_step_done needs index>=0");
  }
  char *j = load_active();
  if (!j) {
    free(note);
    return strdup("error: no active task");
  }
  char *goal = field_str(j, "goal");
  char *id = field_str(j, "id");
  char *steps = extract_steps_array(j);
  free(j);
  /* Parse steps array objects and flip done for matching i */
  /* Simple approach: for each {"i":N,...} if N==idx set done true via string rebuild */
  char rebuilt[8192];
  int o = 0;
  rebuilt[o++] = '[';
  rebuilt[o] = 0;
  int tot = 0, done_n = 0;
  const char *p = steps ? steps : "[]";
  if (*p == '[') p++;
  int first = 1;
  while (*p && *p != ']' && tot < TASK_MAX_STEPS) {
    while (*p && *p != '{' && *p != ']') p++;
    if (*p != '{') break;
    const char *obj = p;
    int depth = 0;
    const char *q;
    for (q = p; *q; q++) {
      if (*q == '{') depth++;
      else if (*q == '}') {
        depth--;
        if (depth == 0) { q++; break; }
      }
    }
    size_t olen = (size_t)(q - obj);
    char objbuf[512];
    if (olen >= sizeof objbuf) olen = sizeof objbuf - 1;
    memcpy(objbuf, obj, olen);
    objbuf[olen] = 0;
    int si = -1;
    const char *ip = strstr(objbuf, "\"i\":");
    if (ip) si = atoi(ip + 4);
    int is_done = (strstr(objbuf, "\"done\":true") || strstr(objbuf, "\"done\": true")) ? 1 : 0;
    char *text = ng_json_get_string(objbuf, "text");
    if (si == idx) is_done = 1;
    if (is_done) done_n++;
    tot++;
    char *et = ng_json_escape(text ? text : "");
    o += snprintf(rebuilt + o, sizeof rebuilt - (size_t)o,
                  "%s{\"i\":%d,\"text\":\"%s\",\"done\":%s}",
                  first ? "" : ",", si >= 0 ? si : (tot - 1),
                  et ? et : "", is_done ? "true" : "false");
    first = 0;
    free(et); free(text);
    p = q;
  }
  if (o < (int)sizeof rebuilt - 2) {
    rebuilt[o++] = ']';
    rebuilt[o] = 0;
  }
  free(steps);
  char *out = rebuild_task(id, "active", goal, rebuilt, ",\"notes\":[]");
  if (out) save_active(out);
  char *reply = NULL;
  asprintf(&reply,
           "ok: step %d marked done (%d/%d complete). note=%s\n"
           "If more steps remain, keep working. If goal fully met, call task_done.",
           idx, done_n, tot, note ? note : "");
  free(out); free(goal); free(id); free(note);
  return reply ? reply : strdup("ok step");
}

static char *tool_done(const char *args) {
  char *summary = ng_json_get_string(args, "summary");
  char *j = load_active();
  if (!j) {
    free(summary);
    return strdup("error: no active task");
  }
  char *eg = ng_json_escape(summary ? summary : "done");
  char *id = field_str(j, "id");
  char *goal = field_str(j, "goal");
  char *out = NULL;
  asprintf(&out,
    "{\"id\":\"%s\",\"status\":\"done\",\"goal\":\"%s\",\"summary\":\"%s\",\"updated\":%ld}",
    id ? id : "task", goal ? goal : "", eg ? eg : "", (long)time(NULL));
  free(eg);
  if (out) {
    archive_active(out);
    /* clear active */
    char p[700];
    active_path(p, sizeof p);
    unlink(p);
  }
  char *reply = NULL;
  asprintf(&reply, "ok: task DONE. summary=%s", summary ? summary : "");
  free(j); free(summary); free(id); free(goal); free(out);
  return reply ? reply : strdup("ok done");
}

static char *tool_block(const char *args) {
  char *reason = ng_json_get_string(args, "reason");
  char *j = load_active();
  if (!j) {
    free(reason);
    return strdup("error: no active task");
  }
  char *eg = ng_json_escape(reason ? reason : "blocked");
  char *id = field_str(j, "id");
  char *goal = field_str(j, "goal");
  char *out = NULL;
  asprintf(&out,
    "{\"id\":\"%s\",\"status\":\"blocked\",\"goal\":\"%s\",\"reason\":\"%s\",\"updated\":%ld}",
    id ? id : "task", goal ? goal : "", eg ? eg : "", (long)time(NULL));
  free(eg);
  if (out) {
    archive_active(out);
    char p[700];
    active_path(p, sizeof p);
    unlink(p);
  }
  char *reply = NULL;
  asprintf(&reply, "ok: task BLOCKED (dead end). reason=%s", reason ? reason : "");
  free(j); free(reason); free(id); free(goal); free(out);
  return reply ? reply : strdup("ok blocked");
}

static char *tool_status(void) {
  char *j = load_active();
  if (!j || !j[0]) {
    free(j);
    return strdup("no active task");
  }
  int tot = 0, done = 0;
  count_steps(j, &tot, &done);
  char *st = field_str(j, "status");
  char *goal = field_str(j, "goal");
  char *reply = NULL;
  asprintf(&reply, "status=%s steps=%d/%d goal=%s\n%s",
           st ? st : "?", done, tot, goal ? goal : "?", j);
  free(j); free(st); free(goal);
  return reply ? reply : strdup("?");
}

char *ng_task_try_tool(const char *name, const char *args_json) {
  if (!name) return NULL;
  const char *a = args_json ? args_json : "{}";
  if (!strcmp(name, "task_plan")) return tool_plan(a);
  if (!strcmp(name, "task_start")) return tool_start();
  if (!strcmp(name, "task_step_done")) return tool_step_done(a);
  if (!strcmp(name, "task_done")) return tool_done(a);
  if (!strcmp(name, "task_block")) return tool_block(a);
  if (!strcmp(name, "task_status")) return tool_status();
  return NULL;
}
