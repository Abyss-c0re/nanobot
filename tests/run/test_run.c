#include "agent.h"
#include "api_share.h"
#include "auth.h"
#include "http.h"
#include "hub_local.h"
#include "runctl.h"
#include "shell.h"
#include "util.h"

#include <arpa/inet.h>
#include <dirent.h>
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

static int on_line(void *ud, const char *line) {
  int *n = ud;
  if (!line) return 0;
  (*n)++;
  return 0;
}

static int stop_line(void *ud, const char *line) {
  int *n = ud;
  if (!line) return 0;
  (*n)++;
  return -1;
}

static int ping_once(void *ud, const char *line) {
  int *n = ud;
  if (line) return 0;
  (*n)++;
  return -1;
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

static int hub_collect(int port, const char *path, char *buf, size_t bufsz) {
  int cfd = -1;
  int got = 0;
  int i;
  struct sockaddr_in addr;
  char req[640];
  buf[0] = 0;
  snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: loop\r\n\r\n", path);
  for (i = 0; i < 50; i++) {
    cfd = socket(AF_INET, SOCK_STREAM, 0);
    if (cfd < 0) return -1;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (connect(cfd, (struct sockaddr *)&addr, sizeof addr) == 0) break;
    close(cfd);
    cfd = -1;
    usleep(20000);
  }
  if (cfd < 0) return -1;
  {
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  }
  if (send(cfd, req, strlen(req), 0) < 0) {
    close(cfd);
    return -1;
  }
  while (got < (int)bufsz - 1) {
    ssize_t r = recv(cfd, buf + got, bufsz - 1 - (size_t)got, 0);
    if (r <= 0) break;
    got += (int)r;
    buf[got] = 0;
    if (strstr(buf, ": ping")) break;
  }
  close(cfd);
  return got;
}

static void test_hub_since(void) {
  char home[] = "/tmp/nanobot-hubs-XXXXXX";
  char file[720];
  char q[160];
  char buf[2048];
  const char *line1 = "{\"k\":\"aa\"}\n";
  const char *body = "{\"k\":\"aa\"}\n{\"k\":\"bb\"}\n";
  long off;
  int port = -1;
  int lfd;
  pid_t pid;
  struct sockaddr_in addr;
  socklen_t al = sizeof addr;
  FILE *f;
  if (!mkdtemp(home)) {
    expect(0, "hub since home");
    return;
  }
  ng_set_workdir(home);
  snprintf(file, sizeof file, "%s/hub", home);
  mkdir(file, 0700);
  snprintf(file, sizeof file, "%s/hub/events.jsonl", home);
  f = fopen(file, "w");
  expect(f != NULL, "hub events file");
  if (!f) return;
  fputs(body, f);
  fclose(f);
  off = (long)strlen(line1);
  expect(off == 11, "first event is 11 bytes");
  lfd = socket(AF_INET, SOCK_STREAM, 0);
  expect(lfd >= 0, "hub since socket");
  if (lfd < 0) return;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) == 0 &&
      getsockname(lfd, (struct sockaddr *)&addr, &al) == 0)
    port = ntohs(addr.sin_port);
  close(lfd);
  expect(port > 0, "hub since port");
  if (port <= 0) return;
  pid = fork();
  expect(pid >= 0, "hub since fork");
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
  snprintf(q, sizeof q, "/hub/v1/events?preview_since=4&since=%ld", off);
  expect(hub_collect(port, q, buf, sizeof buf) > 0, "later since connects");
  expect(strstr(buf, "{\"k\":\"bb\"}") != NULL, "later since keeps the second event");
  expect(strstr(buf, "{\"k\":\"aa\"}") == NULL, "later since skips the first event");
  expect(strstr(buf, ":\"aa\"}") == NULL, "later since does not tear the first event");
  snprintf(q, sizeof q, "/hub/v1/events?since=%ld&preview_since=4", off);
  expect(hub_collect(port, q, buf, sizeof buf) > 0, "leading since connects");
  expect(strstr(buf, "{\"k\":\"bb\"}") != NULL, "leading since keeps the second event");
  expect(strstr(buf, "{\"k\":\"aa\"}") == NULL, "leading since skips the first event");
  snprintf(q, sizeof q, "/hub/v1/events?since=%ld", off);
  expect(hub_collect(port, q, buf, sizeof buf) > 0, "lone since connects");
  expect(strstr(buf, "{\"k\":\"bb\"}") != NULL, "lone since keeps the second event");
  expect(strstr(buf, "{\"k\":\"aa\"}") == NULL, "lone since skips the first event");
  expect(hub_collect(port, "/hub/v1/events?preview_since=4", buf, sizeof buf) > 0,
         "longer key connects");
  expect(strstr(buf, "{\"k\":\"aa\"}") != NULL, "a longer key keeps the first event");
  expect(strstr(buf, "{\"k\":\"bb\"}") != NULL, "a longer key keeps the second event");
  expect(hub_collect(port, "/hub/v1/events", buf, sizeof buf) > 0, "plain events connect");
  expect(strstr(buf, "{\"k\":\"aa\"}") != NULL && strstr(buf, "{\"k\":\"bb\"}") != NULL,
         "a plain stream keeps both events");
  kill(-pid, SIGTERM);
  waitpid(pid, NULL, 0);
}

static void test_tail_stop(void) {
  char home[] = "/tmp/nanobot-run-XXXXXX";
  char *id;
  ng_run *run;
  int lines = 0;
  expect(mkdtemp(home) != NULL, "stop home");
  ng_set_workdir(home);
  id = ng_run_create("tail stop", NULL, NULL);
  expect(id != NULL, "stop create");
  if (!id) return;
  run = ng_run_open(id);
  expect(run != NULL, "stop open");
  if (!run) {
    free(id);
    return;
  }
  expect(ng_run_emit(run, "token", "", "one") == 1, "stop emit one");
  expect(ng_run_emit(run, "token", "", "two") == 1, "stop emit two");
  ng_run_close(run);
  expect(ng_run_tail(id, -1, 5000, stop_line, &lines) == 1, "tail stops after a failed write");
  expect(lines == 1, "second record stays unread");
  free(id);
}

static int shell_stop_now(void) { return 1; }

static struct timespec cap_t0;

static int shell_stop_after_window(void) {
  struct timespec n;
  long ms;
  clock_gettime(CLOCK_MONOTONIC, &n);
  ms = (long)(n.tv_sec - cap_t0.tv_sec) * 1000L + (n.tv_nsec - cap_t0.tv_nsec) / 1000000L;
  return ms >= 700;
}

static void cap_alarm(int sig) {
  (void)sig;
  _exit(86);
}

static void test_shell_abort_kills(void) {
  char home[] = "/tmp/nanobot-run-XXXXXX";
  struct timespec a, b;
  long ms;
  ng_cmd_result cr;
  expect(mkdtemp(home) != NULL, "shell home");
  ng_set_workdir(home);
  ng_shell_set_stop(shell_stop_now);
  clock_gettime(CLOCK_MONOTONIC, &a);
  cr = ng_run_command("sleep 30", 5);
  clock_gettime(CLOCK_MONOTONIC, &b);
  ng_shell_set_stop(NULL);
  ms = (long)(b.tv_sec - a.tv_sec) * 1000L + (b.tv_nsec - a.tv_nsec) / 1000000L;
  expect(cr.exit_code == 130, "abort kills the shell");
  expect(ms < 1500, "killed shell does not wait out the timeout");
  ng_cmd_result_free(&cr);
}

static void test_shell_cap_deadline(void) {
  char home[] = "/tmp/nanobot-run-XXXXXX";
  struct timespec a, b;
  struct sigaction sa, old;
  long ms;
  ng_cmd_result cr;
  expect(mkdtemp(home) != NULL, "cap home");
  ng_set_workdir(home);
  ng_shell_set_stop(NULL);
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = cap_alarm;
  sigaction(SIGALRM, &sa, &old);
  alarm(8);
  clock_gettime(CLOCK_MONOTONIC, &a);
  cr = ng_run_command("yes", 2);
  clock_gettime(CLOCK_MONOTONIC, &b);
  alarm(0);
  sigaction(SIGALRM, &old, NULL);
  ms = (long)(b.tv_sec - a.tv_sec) * 1000L + (b.tv_nsec - a.tv_nsec) / 1000000L;
  expect(cr.exit_code == 124, "capture cap still hits the deadline");
  expect(ms >= 1500 && ms < 5000, "capped shell returns on the deadline");
  expect(cr.output && strlen(cr.output) <= ng_out_max() + 32, "capped output stays bounded");
  ng_cmd_result_free(&cr);
}

static void test_shell_cap_abort(void) {
  char home[] = "/tmp/nanobot-run-XXXXXX";
  struct timespec a, b;
  struct sigaction sa, old;
  long ms;
  ng_cmd_result cr;
  expect(mkdtemp(home) != NULL, "cap abort home");
  ng_set_workdir(home);
  clock_gettime(CLOCK_MONOTONIC, &cap_t0);
  ng_shell_set_stop(shell_stop_after_window);
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = cap_alarm;
  sigaction(SIGALRM, &sa, &old);
  alarm(6);
  clock_gettime(CLOCK_MONOTONIC, &a);
  cr = ng_run_command("yes", 30);
  clock_gettime(CLOCK_MONOTONIC, &b);
  alarm(0);
  sigaction(SIGALRM, &old, NULL);
  ng_shell_set_stop(NULL);
  ms = (long)(b.tv_sec - a.tv_sec) * 1000L + (b.tv_nsec - a.tv_nsec) / 1000000L;
  expect(cr.exit_code == 130, "abort still applies after the capture cap");
  expect(ms >= 500 && ms < 2500, "abort after the cap returns");
  expect(cr.output && strlen(cr.output) <= ng_out_max() + 8, "abort after the cap keeps the cap");
  ng_cmd_result_free(&cr);
}

static void test_shell_eof_reap(void) {
  char home[] = "/tmp/nanobot-run-XXXXXX";
  struct timespec a, b;
  struct sigaction sa, old;
  long ms;
  ng_cmd_result cr;
  expect(mkdtemp(home) != NULL, "eof home");
  ng_set_workdir(home);
  ng_shell_set_stop(NULL);
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = cap_alarm;
  sigaction(SIGALRM, &sa, &old);
  alarm(4);
  cr = ng_run_command("exit 42", 5);
  alarm(0);
  expect(cr.exit_code == 42, "eof reap keeps the status");
  ng_cmd_result_free(&cr);
  clock_gettime(CLOCK_MONOTONIC, &a);
  alarm(4);
  cr = ng_run_command("sleep 2 >/dev/null 2>&1 &", 5);
  clock_gettime(CLOCK_MONOTONIC, &b);
  alarm(0);
  ms = (long)(b.tv_sec - a.tv_sec) * 1000L + (b.tv_nsec - a.tv_nsec) / 1000000L;
  expect(cr.exit_code == 0, "a background shell still returns its status");
  expect(ms < 1500, "a background shell returns before the child finishes");
  ng_cmd_result_free(&cr);
  clock_gettime(CLOCK_MONOTONIC, &a);
  alarm(8);
  cr = ng_run_command("exec >/dev/null 2>&1; sleep 30", 2);
  clock_gettime(CLOCK_MONOTONIC, &b);
  alarm(0);
  sigaction(SIGALRM, &old, NULL);
  ms = (long)(b.tv_sec - a.tv_sec) * 1000L + (b.tv_nsec - a.tv_nsec) / 1000000L;
  expect(cr.exit_code == 124, "a live child after EOF still hits the deadline");
  expect(ms >= 1500 && ms < 5000, "EOF with a live child waits for the deadline");
  ng_cmd_result_free(&cr);
}

static void test_tail_ping(void) {
  char home[] = "/tmp/nanobot-run-XXXXXX";
  char *id;
  ng_run *run;
  int pings = 0;
  struct timespec a, b;
  long ms;
  expect(mkdtemp(home) != NULL, "ping home");
  ng_set_workdir(home);
  id = ng_run_create("tail ping", NULL, NULL);
  expect(id != NULL, "ping create");
  if (!id) return;
  run = ng_run_open(id);
  expect(run != NULL, "ping open");
  if (run) ng_run_close(run);
  clock_gettime(CLOCK_MONOTONIC, &a);
  expect(ng_run_tail(id, -1, 5000, ping_once, &pings) == 0, "idle ping stops the tail");
  clock_gettime(CLOCK_MONOTONIC, &b);
  ms = (long)(b.tv_sec - a.tv_sec) * 1000L + (b.tv_nsec - a.tv_nsec) / 1000000L;
  expect(pings == 1, "one idle ping");
  expect(ms >= 800 && ms < 1800, "idle ping is about one second");
  free(id);
}

static void test_cap_regular_fd(void) {
  char path[] = "/tmp/ng-cap-XXXXXX";
  char path_app[] = "/tmp/ng-app-XXXXXX";
  int fd, afd, p[2];
  struct stat st;
  char b[4];
  fd = mkstemp(path);
  expect(fd >= 0, "cap fd");
  if (fd < 0) return;
  expect(write(fd, "0123456789", 10) == 10, "cap write");
  expect(ng_cap_regular_fd(fd, 100) == 0, "under cap stays");
  expect(write(fd, "abcdefghij", 10) == 10, "second write");
  expect(ng_cap_regular_fd(fd, 10) == 1, "at cap the file is cut");
  expect(fstat(fd, &st) == 0 && st.st_size == 0, "cut file is empty");
  expect(write(fd, "Z", 1) == 1, "write after cut");
  expect(lseek(fd, 0, SEEK_SET) == 0, "rewind");
  memset(b, 0, sizeof b);
  expect(read(fd, b, 3) == 1 && b[0] == 'Z', "new byte is at the start");
  close(fd);
  unlink(path);
  afd = mkstemp(path_app);
  expect(afd >= 0, "append template");
  if (afd >= 0) {
    expect(write(afd, "0123456789abcdefghij", 20) == 20, "append seed");
    close(afd);
    afd = open(path_app, O_WRONLY | O_APPEND);
    expect(afd >= 0, "append fd");
    expect(ng_cap_regular_fd(afd, 10) == 1, "append file is cut");
    expect(write(afd, "Q", 1) == 1, "append write after cut");
    close(afd);
    afd = open(path_app, O_RDONLY);
    memset(b, 0, sizeof b);
    expect(afd >= 0 && read(afd, b, 3) == 1 && b[0] == 'Q',
           "append write starts at the front");
    if (afd >= 0) close(afd);
    unlink(path_app);
  }
  expect(pipe(p) == 0, "pipe");
  expect(ng_cap_regular_fd(p[1], 1) == 0, "a pipe is not cut");
  close(p[0]);
  close(p[1]);
}

static void test_lean_info_skips_refresh(void) {
  ng_agent_cfg agent;
  ng_session session;
  char home[] = "/tmp/ng-info-XXXXXX";
  struct timespec a, b;
  long ms;
  int rc;
  int was_lean;

  was_lean = ng_is_lean();
  setenv("NANOBOT_LEAN", "0", 1);
  ng_limits_init();
  expect(ng_is_lean() == 0, "host status may refresh");
  expect(ng_status_ensure() == 1, "host status ensure stays on");

  setenv("NANOBOT_LEAN", "1", 1);
  ng_limits_init();
  expect(ng_is_lean() == 1, "lean flag");
  expect(ng_status_ensure() == 0, "lean status ensure is off");

  expect(mkdtemp(home) != NULL, "info home");
  ng_set_workdir(home);
  setenv("NANOBOT_API_SHARE", "1", 1);
  memset(&agent, 0, sizeof agent);
  agent.base_url = strdup("https://api.x.ai");
  ng_session_init(&session);
  session.access_token = strdup("sealed-access");
  session.refresh_token = strdup("sealed-refresh");
  session.expires_at = time(NULL) + 7200;
  agent.session = &session;
  expect(ng_api_share_local_exit_ok(&agent) == 1, "valid seal is an exit");

  session.expires_at = 1;
  signal(SIGALRM, SIG_DFL);
  alarm(3);
  clock_gettime(CLOCK_MONOTONIC, &a);
  rc = ng_api_share_local_exit_ok(&agent);
  clock_gettime(CLOCK_MONOTONIC, &b);
  alarm(0);
  ms = (long)(b.tv_sec - a.tv_sec) * 1000L + (b.tv_nsec - a.tv_nsec) / 1000000L;
  expect(rc == 0, "expired lean seal is not an exit");
  expect(ms < 500, "expired lean seal does not fork a refresh");

  ng_session_free(&session);
  free(agent.base_url);
  if (was_lean) setenv("NANOBOT_LEAN", "1", 1);
  else unsetenv("NANOBOT_LEAN");
  ng_limits_init();
}

static int file_has(const char *path, const char *needle) {
  size_t n = 0;
  char *b = ng_read_file(path, &n);
  int hit;
  if (!b) return 0;
  hit = strstr(b, needle) != NULL;
  free(b);
  return hit;
}

static void test_lean_log_live_path(void) {
  char dir[] = "/tmp/ng-live-XXXXXX";
  char path[128], moved[160];
  struct stat st;
  int was_lean;
  int stderr_on;
  int i;
  int nfd;

  was_lean = ng_is_lean();
  stderr_on = ng_log_stderr_enabled();
  ng_log_set_stderr(0);
  expect(mkdtemp(dir) != NULL, "live log home");
  snprintf(path, sizeof path, "%s/nanobot.log", dir);
  snprintf(moved, sizeof moved, "%s/nanobot.log.1", dir);

  setenv("NANOBOT_LEAN", "0", 1);
  ng_limits_init();
  ng_log_init(path);
  ng_log("HOSTONE");
  expect(rename(path, moved) == 0, "host rename aside");
  ng_log_sync_live();
  ng_log("HOSTTWO");
  expect(file_has(moved, "HOSTONE"), "host archive keeps the first line");
  expect(file_has(moved, "HOSTTWO"), "host keeps the open log inode");
  expect(!file_has(path, "HOSTTWO"), "host sync leaves the renamed inode");

  setenv("NANOBOT_LEAN", "1", 1);
  ng_limits_init();
  ng_log_init(path);
  ng_log("LEANONE");
  ng_log("LEANTWO");
  expect(file_has(path, "LEANONE") && file_has(path, "LEANTWO"),
         "a short lean log is not cut");
  expect(rename(path, moved) == 0, "lean rename aside");
  nfd = open(path, O_CREAT | O_WRONLY, 0644);
  expect(nfd >= 0, "live log path");
  if (nfd >= 0) close(nfd);
  expect(ng_log_fd_is_live() == 0, "renamed log fd is not the live path");
  expect(stat(moved, &st) == 0, "renamed log stays");
  {
    off_t kept = st.st_size;
    ng_log_sync_live();
    expect(ng_log_fd_is_live() == 1, "listener reopens the live path");
    expect(stat(path, &st) == 0 && st.st_size == 0, "reopen writes nothing");
    expect(stat(moved, &st) == 0 && st.st_size == kept, "reopen keeps the renamed bytes");
  }
  ng_log("LEANTHREE");
  expect(file_has(path, "LEANTHREE"), "lean reopens the live log path");
  expect(file_has(moved, "LEANONE"), "renamed log keeps its bytes");
  expect(!file_has(moved, "LEANTHREE"), "renamed log does not take the new line");

  ng_log("LEANSEEDSTART xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
  for (i = 0; i < 180; i++)
    ng_log("m xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
  ng_log("LEANLOGEND");
  expect(file_has(moved, "LEANSEEDSTART") || file_has(path, "LEANSEEDSTART"),
         "the seed line is archived or still live");
  expect(file_has(path, "LEANLOGEND"), "the newest line is on the live path");
  expect(stat(moved, &st) == 0 && (size_t)st.st_size >= ng_log_max(),
         "the renamed sibling keeps a full segment");

  ng_log_set_stderr(stderr_on);
  if (was_lean) setenv("NANOBOT_LEAN", "1", 1);
  else unsetenv("NANOBOT_LEAN");
  ng_limits_init();
  unlink(path);
  unlink(moved);
  rmdir(dir);
}

static void test_worker_title(void) {
  char path[] = "/tmp/bin/nanobot";
  char home_flag[] = "--home";
  char home_val[] = "/tmp/nanobot";
  char port_flag[] = "--port";
  char port_val[] = "8787";
  char *av[] = {path, home_flag, home_val, port_flag, port_val, NULL};
  char joined[160];
  char short_path[] = "nb";
  char *short_av[] = {short_path, NULL};
  ng_set_proc_argv(av);
  ng_mark_http_worker();
  snprintf(joined, sizeof joined, "%s %s %s %s %s", path, home_flag, home_val,
           port_flag, port_val);
  expect(strncmp(path, "nb-http", 7) == 0, "worker title is nb-http");
  expect(strstr(joined, "nanobot --home") == NULL, "worker line is not a second listener");
  expect(strstr(joined, "/bin/nanobot ") == NULL, "worker line drops the listener path");
  expect(strcmp(home_flag, "--home") == 0, "listener flags stay on the other arguments");
  ng_set_proc_argv(short_av);
  ng_mark_http_worker();
  expect(strcmp(short_path, "nb") == 0, "a short argv0 stays");
}

static int proc_is_zombie(pid_t pid) {
  char path[64];
  char buf[256];
  char *r;
  FILE *f;
  snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
  f = fopen(path, "r");
  if (!f) return 0;
  if (!fgets(buf, sizeof buf, f)) {
    fclose(f);
    return 0;
  }
  fclose(f);
  r = strrchr(buf, ')');
  return r && r[1] == ' ' && r[2] == 'Z';
}

static void test_on_worker_exit(int s) {
  int st;
  (void)s;
  while (waitpid(-1, &st, WNOHANG) > 0) ng_http_worker_reaped();
}

static void test_worker_slot_before_exit(void) {
  struct sigaction sa, old;
  pid_t pid;
  int i, z = 0;
  ng_http_worker_slots_reset();
  ng_http_arm_worker();
  ng_http_worker_fork_failed();
  expect(ng_http_worker_slots() == 0, "a failed fork releases the slot");

  memset(&sa, 0, sizeof sa);
  sa.sa_handler = test_on_worker_exit;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  if (sigaction(SIGCHLD, &sa, &old) != 0) {
    expect(0, "worker slot handler installs");
    return;
  }
  ng_http_worker_slots_reset();
  ng_http_arm_worker();
  pid = fork();
  if (pid < 0) {
    ng_http_worker_fork_failed();
    sigaction(SIGCHLD, &old, NULL);
    expect(0, "worker slot fork");
    return;
  }
  if (pid == 0) {
    signal(SIGCHLD, SIG_DFL);
    ng_http_worker_child();
    _exit(0);
  }
  for (i = 0; i < 200; i++) {
    if (proc_is_zombie(pid)) {
      z = 1;
      break;
    }
    usleep(1000);
  }
  if (!z) {
    int st = 0;
    ng_http_worker_parent();
    waitpid(pid, &st, 0);
    if (ng_http_worker_slots() > 0) ng_http_worker_reaped();
    sigaction(SIGCHLD, &old, NULL);
    expect(0, "the worker exited while the slot is held");
    return;
  }
  expect(ng_http_worker_slots() == 1, "a fast worker still holds one slot");
  ng_http_worker_parent();
  expect(ng_http_worker_slots() == 0, "reap releases that slot");
  {
    int st = 0;
    waitpid(pid, &st, WNOHANG);
  }
  sigaction(SIGCHLD, &old, NULL);
}

static void write_orphan(const char *jdir, const char *id, const char *status) {
  char path[192];
  FILE *f;
  snprintf(path, sizeof path, "%s/%s.json", jdir, id);
  f = fopen(path, "wb");
  expect(f != NULL, "seed an orphan job");
  if (!f) return;
  fprintf(f, "{\"id\":\"%s\",\"status\":\"%s\",\"kind\":\"prompt\"}\n", id, status);
  fclose(f);
}

static int orphan_fields(const char *jdir, const char *id, char *st_out, size_t st_n,
                         char *err, size_t err_n) {
  char path[192];
  char buf[400];
  FILE *f;
  size_t n;
  char *st;
  char *e;
  if (st_out && st_n) st_out[0] = 0;
  if (err && err_n) err[0] = 0;
  snprintf(path, sizeof path, "%s/%s.json", jdir, id);
  f = fopen(path, "rb");
  if (!f) return 0;
  n = fread(buf, 1, sizeof buf - 1, f);
  fclose(f);
  buf[n] = 0;
  st = ng_json_get_string(buf, "status");
  e = ng_json_get_string(buf, "error");
  if (st_out && st_n && st) snprintf(st_out, st_n, "%s", st);
  if (err && err_n && e) snprintf(err, err_n, "%s", e);
  free(st);
  free(e);
  return 1;
}

static void test_auto_orphan(void) {
  char jdir[] = "/tmp/nb-orphan-test";
  char st[32];
  char err[64];
  mkdir(jdir, 0700);
  write_orphan(jdir, "agrun-0000000001-1", "queued");
  write_orphan(jdir, "17920893130001", "queued");
  write_orphan(jdir, "agrun-0000000002-1", "done");
  write_orphan(jdir, "nope-1", "queued");
  ng_jobs_mark_orphans(jdir);
  expect(orphan_fields(jdir, "agrun-0000000001-1", st, sizeof st, err, sizeof err) &&
             strcmp(st, "error") == 0 && strcmp(err, "orphan_restart") == 0,
         "a queued auto job left by a restart is marked error");
  expect(orphan_fields(jdir, "17920893130001", st, sizeof st, err, sizeof err) &&
             strcmp(st, "error") == 0 && strcmp(err, "orphan_restart") == 0,
         "a queued digit job left by a restart is marked error");
  expect(orphan_fields(jdir, "agrun-0000000002-1", st, sizeof st, err, sizeof err) &&
             strcmp(st, "done") == 0 && err[0] == 0,
         "a done auto job stays done");
  expect(orphan_fields(jdir, "nope-1", st, sizeof st, err, sizeof err) &&
             strcmp(st, "queued") == 0 && err[0] == 0,
         "a name that is not a job id stays queued");
}

static void wipe_jobs(const char *jdir) {
  DIR *d;
  struct dirent *de;
  mkdir(jdir, 0700);
  d = opendir(jdir);
  if (!d) return;
  while ((de = readdir(d)) != NULL) {
    char path[192];
    if (de->d_name[0] == '.') continue;
    if (snprintf(path, sizeof path, "%s/%s", jdir, de->d_name) >= (int)sizeof path)
      continue;
    unlink(path);
  }
  closedir(d);
}

static void write_side(const char *jdir, const char *id, const char *ext, const char *body) {
  char path[192];
  FILE *f;
  snprintf(path, sizeof path, "%s/%s%s", jdir, id, ext);
  f = fopen(path, "wb");
  if (!f) return;
  fputs(body, f);
  fclose(f);
}

static int job_exists(const char *jdir, const char *id, const char *ext) {
  char path[192];
  snprintf(path, sizeof path, "%s/%s%s", jdir, id, ext);
  return access(path, F_OK) == 0;
}

static void test_auto_cap(void) {
  char jdir[] = "/tmp/nb-cap-test";
  char edirs[] = "/tmp/nb-cap-error";
  char id[32];
  char st[32];
  char err[64];
  int i;
  wipe_jobs(jdir);
  for (i = 1; i <= 49; i++) {
    snprintf(id, sizeof id, "%016d", i);
    write_orphan(jdir, id, "done");
    write_side(jdir, id, ".in", "old");
    snprintf(id, sizeof id, "agrun-%010d-1", i);
    write_orphan(jdir, id, "done");
    write_side(jdir, id, ".in", "old");
  }
  write_orphan(jdir, "0000000000000000", "queued");
  write_side(jdir, "0000000000000000", ".in", "stay");
  write_orphan(jdir, "agrun-0000000000-9", "queued");
  write_side(jdir, "agrun-0000000000-9", ".in", "stay");
  write_orphan(jdir, "0000000000000050", "running");
  write_orphan(jdir, "agrun-0000000050-1", "running");
  ng_jobs_gc(jdir);
  expect(!job_exists(jdir, "0000000000000001", ".json") &&
             !job_exists(jdir, "0000000000000001", ".in"),
         "the oldest done digit job drops");
  expect(job_exists(jdir, "0000000000000049", ".json") &&
             job_exists(jdir, "0000000000000049", ".in"),
         "a newer done digit job stays");
  expect(orphan_fields(jdir, "0000000000000000", st, sizeof st, err, sizeof err) &&
             strcmp(st, "queued") == 0 &&
             job_exists(jdir, "0000000000000000", ".in"),
         "a queued digit job stays");
  expect(orphan_fields(jdir, "0000000000000050", st, sizeof st, err, sizeof err) &&
             strcmp(st, "running") == 0,
         "a running digit job stays");
  expect(!job_exists(jdir, "agrun-0000000001-1", ".json") &&
             !job_exists(jdir, "agrun-0000000001-1", ".in"),
         "the oldest done auto job drops");
  expect(job_exists(jdir, "agrun-0000000049-1", ".json") &&
             job_exists(jdir, "agrun-0000000049-1", ".in"),
         "a newer done auto job stays");
  expect(orphan_fields(jdir, "agrun-0000000000-9", st, sizeof st, err, sizeof err) &&
             strcmp(st, "queued") == 0 &&
             job_exists(jdir, "agrun-0000000000-9", ".in"),
         "a queued auto job stays");
  expect(orphan_fields(jdir, "agrun-0000000050-1", st, sizeof st, err, sizeof err) &&
             strcmp(st, "running") == 0,
         "a running auto job stays");

  wipe_jobs(edirs);
  for (i = 1; i <= 49; i++) {
    snprintf(id, sizeof id, "agrun-%010d-1", i);
    write_orphan(edirs, id, "error");
    write_side(edirs, id, ".in", "old");
  }
  write_orphan(edirs, "agrun-0000000000-9", "queued");
  ng_jobs_gc(edirs);
  expect(!job_exists(edirs, "agrun-0000000001-1", ".json") &&
             !job_exists(edirs, "agrun-0000000001-1", ".in"),
         "the oldest error auto job drops");
  expect(orphan_fields(edirs, "agrun-0000000049-1", st, sizeof st, err, sizeof err) &&
             strcmp(st, "error") == 0,
         "a newer error auto job stays");
  expect(orphan_fields(edirs, "agrun-0000000000-9", st, sizeof st, err, sizeof err) &&
             strcmp(st, "queued") == 0,
         "a queued auto job stays beside an error cap");
}

int main(void) {
  test_cap_regular_fd();
  test_auto_orphan();
  test_auto_cap();
  test_worker_title();
  test_worker_slot_before_exit();
  test_lean_info_skips_refresh();
  test_lean_log_live_path();
  test_routes();
  test_control();
  test_long_record();
  test_oversize_filter();
  test_list_newest();
  test_tail_stop();
  test_shell_abort_kills();
  test_shell_cap_deadline();
  test_shell_cap_abort();
  test_shell_eof_reap();
  test_tail_ping();
  test_hub_closed_client();
  test_hub_since();
  if (fails) {
    fprintf(stderr, "%d run checks failed\n", fails);
    return 1;
  }
  return 0;
}
