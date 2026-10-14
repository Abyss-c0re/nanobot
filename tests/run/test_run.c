#include "agent.h"
#include "runctl.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;

static void expect(int cond, const char *msg) {
  if (!cond) {
    fprintf(stderr, "FAIL: %s\n", msg);
    fails++;
  }
}

static void on_line(void *ud, const char *line) {
  int *n = ud;
  (void)line;
  (*n)++;
}

static void test_routes(void) {
  char id[64], action[32];
  long since = 0;
  expect(ng_run_is_route("/api/runs", id, sizeof id, action, sizeof action, &since) == 1,
         "collection");
  expect(id[0] == 0 && action[0] == 0 && since == -1, "collection fields");
  expect(ng_run_is_route("/peer/v1/runs/r12/events/4", id, sizeof id, action, sizeof action,
                         &since) == 1,
         "events cursor");
  expect(strcmp(id, "r12") == 0 && strcmp(action, "events") == 0 && since == 4, "cursor fields");
  expect(ng_run_is_route("/api/v1/runs/r12/stream", id, sizeof id, action, sizeof action, &since) == 1 &&
             strcmp(action, "stream") == 0 && since == -1,
         "stream");
  expect(ng_run_is_route("/api/runs/../events", id, sizeof id, action, sizeof action, &since) == 0,
         "reject dotdot");
  expect(ng_run_is_route("/api/chat", id, sizeof id, action, sizeof action, &since) == 0,
         "chat is not a run route");
}

static void test_control(void) {
  char home[] = "/tmp/nanobot-run-XXXXXX";
  char *id;
  ng_run *run;
  ng_run_limits lim;
  char *events;
  char *status;
  char *prompt;
  char *taken;
  int lines = 0;
  expect(mkdtemp(home) != NULL, "temp home");
  ng_set_workdir(home);
  id = ng_run_create("fix the parser", NULL,
                     "{\"rules\":[{\"op\":\"redact\",\"match\":\"sekret\",\"with\":\"***\"},"
                     "{\"op\":\"drop\",\"match\":\"DROPME\"},"
                     "{\"op\":\"drop_kind\",\"kind\":\"thinking\"}]}");
  expect(id != NULL, "create");
  if (!id) return;
  run = ng_run_open(id);
  expect(run != NULL, "open");
  prompt = ng_run_prompt(run);
  expect(prompt && strcmp(prompt, "fix the parser") == 0, "prompt stored");
  free(prompt);
  expect(ng_run_emit(run, "token", "", "alpha sekret beta") == 1, "token emitted");
  expect(ng_run_emit(run, "token", "", "see DROPME") == 0, "drop suppresses the chunk");
  expect(ng_run_emit(run, "token", "", "sekr") == 1, "needle prefix is held");
  expect(ng_run_flush(run) == 0, "flush held prefix");
  expect(ng_run_emit(run, "thinking", "", "hidden") == 0, "thinking kind dropped");
  expect(ng_run_emit(run, "tool_call", "shell", "{\"command\":\"echo hi\"}") == 1, "tool call");
  expect(ng_run_note_code(run, "shell", "echo hi", "hello") == 0, "plain output is not a change");
  expect(ng_run_note_code(run, "shell", "echo hi", "diff --git a/f\n@@ -1 +1 @@\n") == 1,
         "diff output is a code change");
  events = ng_run_events_json(id, -1, 20);
  expect(events && strstr(events, "alpha *** beta"), "redact reached the log");
  expect(events && strstr(events, "sekr"), "flush wrote the held prefix");
  expect(events && !strstr(events, "sekret"), "raw needle stays out of the log");
  expect(events && !strstr(events, "DROPME"), "dropped chunk stays out of the log");
  expect(events && strstr(events, "\"kind\":\"tool_call\""), "tool call recorded");
  expect(events && strstr(events, "\"kind\":\"code_change\""), "code change recorded");
  free(events);
  expect(ng_run_poll(run, 0) == 0, "running control");
  expect(ng_run_request_pause(id) == 0, "pause");
  expect(ng_run_poll(run, 0) == 2, "paused");
  expect(ng_run_request_intercept(id, "stop and explain") == 0, "intercept");
  expect(ng_run_poll(run, 0) == 3, "intercept wins over pause");
  taken = ng_run_take_intercept(run);
  expect(taken && strcmp(taken, "stop and explain") == 0, "intercept text");
  free(taken);
  expect(ng_run_poll(run, 0) == 2, "still paused after take");
  expect(ng_run_request_resume(id) == 0, "resume");
  expect(ng_run_poll(run, 0) == 0, "resumed");
  expect(ng_run_request_abort(id) == 0, "abort");
  expect(ng_run_poll(run, 0) == 1, "aborted");
  ng_run_limits_clear(&lim);
  snprintf(lim.model, sizeof lim.model, "local-model");
  lim.has_effort = 1;
  snprintf(lim.effort, sizeof lim.effort, "xHigh");
  lim.max_tokens = 1234;
  lim.context_tokens = 8000;
  lim.compact_explicit = 1;
  lim.compact_max_refills = 4;
  lim.compact_keep_tail = 3;
  lim.compact_keep_head = 1;
  lim.compact_preemptive = 1;
  expect(ng_run_set_limits(id, &lim) == 0, "set limits");
  expect(ng_run_load_limits(run, &lim) == 0, "load limits");
  expect(strcmp(lim.model, "local-model") == 0, "model stored");
  expect(lim.has_effort == 1 && strcmp(lim.effort, "xhigh") == 0, "effort canon");
  expect(lim.max_tokens == 1234 && lim.context_tokens == 8000, "token caps");
  expect(lim.compact_explicit == 1 && lim.compact_max_refills == 4 && lim.compact_keep_tail == 3,
         "compact settings");
  {
    ng_agent_cfg cfg;
    ng_agent_cfg_init(&cfg);
    expect(ng_run_apply(&cfg, run) == 0, "apply limits");
    expect(cfg.model && strcmp(cfg.model, "local-model") == 0, "apply model");
    expect(cfg.reasoning_effort && strcmp(cfg.reasoning_effort, "xhigh") == 0, "apply effort");
    expect(cfg.max_tokens == 1234 && cfg.context_tokens == 8000, "apply tokens");
    expect(cfg.compact_explicit == 1 && cfg.compact_max_refills == 4 &&
               cfg.compact_keep_tail == 3 && cfg.compact_keep_head == 1 &&
               cfg.compact_preemptive == 1,
           "apply compact");
    ng_agent_cfg_free(&cfg);
  }
  lim.has_effort = 1;
  snprintf(lim.effort, sizeof lim.effort, "bad token");
  expect(ng_run_set_limits(id, &lim) != 0, "bad effort rejected");
  expect(ng_run_mark_running(run) == 0, "mark running");
  status = ng_run_status_json(id);
  expect(status && strstr(status, "\"status\":\"running\"") && strstr(status, "\"alive\":true"),
         "status alive");
  expect(status && strstr(status, "local-model") && strstr(status, "1234"), "status shows limits");
  free(status);
  lines = 0;
  expect(ng_run_tail(id, 0, 0, on_line, &lines) >= 1, "tail reads one pass");
  expect(ng_run_finish(run, "aborted", "aborted") == 0, "finish");
  status = ng_run_status_json(id);
  expect(status && strstr(status, "\"status\":\"aborted\""), "finished aborted");
  free(status);
  expect(ng_run_open("../x") == NULL, "open rejects traversal");
  expect(ng_run_request_abort("../x") != 0, "control rejects traversal");
  ng_run_close(run);
  free(id);
}

int main(void) {
  test_routes();
  test_control();
  if (fails) {
    fprintf(stderr, "%d run checks failed\n", fails);
    return 1;
  }
  return 0;
}
