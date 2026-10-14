#include "agent.h"
#include "hub_local.h"
#include "runctl.h"
#include "util.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
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
  expect(ng_run_request_pause(id) == 0, "pause for slice");
  {
    pid_t child;
    struct timespec t0, t1;
    long ms;
    int rc;
    int st = 0;
    child = fork();
    expect(child >= 0, "slice fork");
    if (child == 0) {
      usleep(30 * 1000);
      if (ng_run_request_abort(id) != 0) _exit(2);
      _exit(0);
    }
    if (child > 0) {
      clock_gettime(CLOCK_MONOTONIC, &t0);
      rc = ng_run_poll(run, 1000);
      clock_gettime(CLOCK_MONOTONIC, &t1);
      ms = (long)(t1.tv_sec - t0.tv_sec) * 1000 +
           (long)(t1.tv_nsec - t0.tv_nsec) / 1000000;
      expect(rc == 1, "abort seen inside the pause wait");
      expect(ms < 170, "pause slice is shorter than 200 ms");
      waitpid(child, &st, 0);
    }
  }
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
  {
    char mp[700];
    char *meta;
    char *p;
    unsigned long long ticks = 0;
    unsigned long long fake;
    FILE *mf;
    snprintf(mp, sizeof mp, "%s/runs/%s/meta.json", ng_workdir(), id);
    meta = ng_read_file(mp, NULL);
    p = meta ? strstr(meta, "\"start_ticks\":") : NULL;
    expect(p != NULL, "start time stored");
    if (p != NULL)
      ticks = strtoull(strchr(p, ':') + 1, NULL, 10);
    expect(ticks > 0, "start time nonzero");
    fake = ticks == 1 ? 2 : 1;
    mf = fopen(mp, "w");
    expect(mf != NULL, "reuse meta");
    if (mf) {
      fprintf(mf,
              "{\"schema\":\"nanobot.run.v1\",\"ok\":true,\"action\":\"run\","
              "\"id\":\"%s\",\"status\":\"running\",\"pid\":%d,\"start_ticks\":%llu,"
              "\"error\":\"\",\"python\":0}\n",
              id, (int)getpid(), fake);
      fclose(mf);
    }
    status = ng_run_status_json(id);
    expect(status && strstr(status, "\"alive\":false"), "reused pid is not alive");
    free(status);
    free(meta);
  }
  {
    pid_t z;
    int st = 0;
    char mp[700];
    FILE *mf;
    z = fork();
    expect(z >= 0, "zombie fork");
    if (z == 0) _exit(0);
    if (z > 0) {
      int spun = 0;
      while (spun < 50) {
        char sb[256];
        FILE *sf;
        char *rp;
        snprintf(mp, sizeof mp, "/proc/%d/stat", (int)z);
        sf = fopen(mp, "r");
        if (!sf) break;
        rp = fgets(sb, sizeof sb, sf) ? strrchr(sb, ')') : NULL;
        fclose(sf);
        if (rp && rp[1] == ' ' && rp[2] == 'Z') break;
        usleep(1000);
        spun++;
      }
      snprintf(mp, sizeof mp, "%s/runs/%s/meta.json", ng_workdir(), id);
      mf = fopen(mp, "w");
      expect(mf != NULL, "zombie meta");
      if (mf) {
        fprintf(mf,
                "{\"schema\":\"nanobot.run.v1\",\"ok\":true,\"action\":\"run\","
                "\"id\":\"%s\",\"status\":\"running\",\"pid\":%d,\"error\":\"\","
                "\"python\":0}\n",
                id, (int)z);
        fclose(mf);
      }
      status = ng_run_status_json(id);
      expect(status && strstr(status, "\"alive\":false"), "zombie is not alive");
      free(status);
      waitpid(z, &st, 0);
    }
  }
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

static void test_long_record(void) {
  char home[] = "/tmp/nanobot-run-XXXXXX";
  char *id;
  ng_run *run;
  char *events;
  char big[2001];
  int lines = 0;
  memset(big, 1, 2000);
  big[2000] = 0;
  expect(mkdtemp(home) != NULL, "long home");
  ng_set_workdir(home);
  id = ng_run_create("long record", NULL, NULL);
  expect(id != NULL, "long create");
  if (!id) return;
  run = ng_run_open(id);
  expect(ng_run_emit(run, "tool_result", "shell", big) == 1, "long emit");
  ng_run_close(run);
  run = ng_run_open(id);
  expect(run != NULL, "reopen after long record");
  expect(ng_run_emit(run, "token", "", "after") == 1, "seq continues");
  events = ng_run_events_json(id, -1, 10);
  expect(events && strlen(events) > 12000, "escaped record stays whole");
  expect(events && strstr(events, "\"seq\":2") != NULL, "second event keeps seq 2");
  expect(events && strstr(events, "after") != NULL, "following token present");
  free(events);
  expect(ng_run_tail(id, -1, 0, on_line, &lines) == 2, "tail returns both records");
  expect(lines == 2, "tail saw both records");
  ng_run_close(run);
  free(id);
}

static void test_oversize_filter(void) {
  char home[] = "/tmp/nanobot-run-XXXXXX";
  char *id;
  ng_run *run;
  char path[700];
  FILE *fp;
  char *events;
  const char *head = "{\"rules\":[{\"op\":\"redact\",\"match\":\"sekret\",\"with\":\"";
  const char *tail = "\"}]}";
  size_t pad;
  size_t i;
  expect(mkdtemp(home) != NULL, "filter home");
  ng_set_workdir(home);
  id = ng_run_create("filter cap", NULL, NULL);
  expect(id != NULL, "filter create");
  if (!id) return;
  run = ng_run_open(id);
  expect(run != NULL, "filter open");
  snprintf(path, sizeof path, "%s/runs/%s/filter.json", ng_workdir(), id);
  pad = 8191 - strlen(head) - strlen(tail);
  fp = fopen(path, "w");
  expect(fp != NULL, "oversize filter");
  if (fp) {
    fputs(head, fp);
    for (i = 0; i < pad; i++) fputc('P', fp);
    fputs(tail, fp);
    fputc('Z', fp);
    fclose(fp);
  }
  expect(ng_run_emit(run, "token", "", "see sekret") == 1, "oversize emit");
  events = ng_run_events_json(id, -1, 10);
  expect(events && strstr(events, "sekret"), "oversize file leaves the loaded filter");
  free(events);
  fp = fopen(path, "w");
  expect(fp != NULL, "fitting filter");
  if (fp) {
    fputs("{\"rules\":[{\"op\":\"redact\",\"match\":\"sekret\",\"with\":\"QQQ\"}]}", fp);
    fclose(fp);
  }
  expect(ng_run_emit(run, "token", "", "see sekret") == 1, "fitting emit");
  events = ng_run_events_json(id, -1, 10);
  expect(events && strstr(events, "QQQ"), "fitting file is parsed");
  free(events);
  ng_run_close(run);
  free(id);
}

static void test_list_newest(void) {
  char home[] = "/tmp/nanobot-run-XXXXXX";
  char *ids[33];
  char *list;
  char path[700];
  char needle[80];
  struct timespec ts[2];
  const char *newest;
  const char *older;
  int i;
  int plates = 0;
  for (i = 0; i < 33; i++) ids[i] = NULL;
  expect(mkdtemp(home) != NULL, "list home");
  ng_set_workdir(home);
  for (i = 0; i < 33; i++) {
    ids[i] = ng_run_create("list me", NULL, NULL);
    expect(ids[i] != NULL, "list create");
  }
  for (i = 0; i < 33; i++) {
    if (!ids[i]) continue;
    snprintf(path, sizeof path, "%s/runs/%s/events.jsonl", ng_workdir(), ids[i]);
    ts[0].tv_sec = 1000000000L + i;
    ts[0].tv_nsec = 0;
    ts[1] = ts[0];
    expect(utimensat(AT_FDCWD, path, ts, 0) == 0, "list mtime");
  }
  list = ng_run_list_json();
  expect(list != NULL, "list json");
  if (ids[32]) {
    snprintf(needle, sizeof needle, "\"id\":\"%s\"", ids[32]);
    expect(list && strstr(list, needle), "newest run is listed");
  }
  if (ids[0]) {
    snprintf(needle, sizeof needle, "\"id\":\"%s\"", ids[0]);
    expect(list && !strstr(list, needle), "oldest run is left out");
  }
  if (list) {
    const char *p = list;
    while ((p = strstr(p, "\"id\":\"")) != NULL) {
      plates++;
      p += 6;
    }
  }
  expect(plates == 32, "list keeps 32 plates");
  if (list && ids[32] && ids[31]) {
    snprintf(needle, sizeof needle, "\"id\":\"%s\"", ids[32]);
    newest = strstr(list, needle);
    snprintf(needle, sizeof needle, "\"id\":\"%s\"", ids[31]);
    older = strstr(list, needle);
    expect(newest && older && newest < older, "newest plate is first");
  }
  free(list);
  for (i = 0; i < 33; i++) free(ids[i]);
}

static int proc_children(pid_t pid) {
  char path[64];
  FILE *fp;
  int n = 0;
  int x;
  snprintf(path, sizeof path, "/proc/%d/task/%d/children", (int)pid, (int)pid);
  fp = fopen(path, "r");
  if (!fp) return -1;
  while (fscanf(fp, "%d", &x) == 1) n++;
  fclose(fp);
  return n;
}

static void test_hub_closed_client(void) {
  char home[] = "/tmp/nanobot-hub-XXXXXX";
  int port = -1;
  int lfd;
  int cfd = -1;
  pid_t pid;
  struct sockaddr_in addr;
  socklen_t al = sizeof addr;
  char req[] = "GET /hub/v1/events HTTP/1.1\r\nHost: loop\r\n\r\n";
  char buf[1024];
  int got = 0;
  int i;
  expect(mkdtemp(home) != NULL, "hub home");
  ng_set_workdir(home);
  lfd = socket(AF_INET, SOCK_STREAM, 0);
  expect(lfd >= 0, "hub probe socket");
  if (lfd < 0) return;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) == 0 &&
      getsockname(lfd, (struct sockaddr *)&addr, &al) == 0)
    port = ntohs(addr.sin_port);
  close(lfd);
  expect(port > 0, "hub port");
  if (port <= 0) return;
  pid = fork();
  expect(pid >= 0, "hub fork");
  if (pid < 0) return;
  if (pid == 0) {
    ng_hub_out_cfg cfg;
    setpgid(0, 0);
    memset(&cfg, 0, sizeof cfg);
    cfg.port_out = port;
    ng_hub_out_serve(&cfg);
    _exit(0);
  }
  setpgid(pid, pid);
  for (i = 0; i < 50; i++) {
    cfd = socket(AF_INET, SOCK_STREAM, 0);
    if (cfd < 0) break;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (connect(cfd, (struct sockaddr *)&addr, sizeof addr) == 0) break;
    close(cfd);
    cfd = -1;
    usleep(20000);
  }
  expect(cfd >= 0, "hub connect");
  if (cfd >= 0) {
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    send(cfd, req, sizeof req - 1, 0);
    while (got < (int)sizeof buf - 1) {
      ssize_t r = recv(cfd, buf + got, sizeof buf - 1 - (size_t)got, 0);
      if (r <= 0) break;
      got += (int)r;
      buf[got] = 0;
      if (strstr(buf, "stream_open")) break;
    }
    expect(strstr(buf, "stream_open") != NULL, "hub stream opened");
    expect(proc_children(pid) >= 1, "hub events child is running");
    close(cfd);
    sleep(2);
    expect(proc_children(pid) == 0, "closed client ends the events child");
  }
  kill(-pid, SIGTERM);
  waitpid(pid, NULL, 0);
}

int main(void) {
  test_routes();
  test_control();
  test_long_record();
  test_oversize_filter();
  test_list_newest();
  test_hub_closed_client();
  if (fails) {
    fprintf(stderr, "%d run checks failed\n", fails);
    return 1;
  }
  return 0;
}
