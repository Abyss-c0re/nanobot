#include "augogen.h"
#include "motive.h"

#include <nanobot/os.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int g_fail;

static void expect(int cond, const char *msg) {
  if (cond) return;
  fprintf(stderr, "FAIL %s\n", msg);
  g_fail++;
}

static int words(const char *s) {
  int n = 0, in = 0;
  if (!s) return 0;
  for (; *s; s++) {
    if (*s != ' ' && *s != '\t') {
      if (!in) {
        n++;
        in = 1;
      }
    } else {
      in = 0;
    }
  }
  return n;
}

static double seconds_since(const struct timespec *a) {
  struct timespec b;
  clock_gettime(CLOCK_MONOTONIC, &b);
  return (double)(b.tv_sec - a->tv_sec) + (double)(b.tv_nsec - a->tv_nsec) / 1e9;
}

static int count_substr(const char *s, const char *n) {
  int c = 0;
  size_t L;
  if (!s || !n || !n[0]) return 0;
  L = strlen(n);
  while (*s) {
    if (!strncmp(s, n, L)) c++;
    s++;
  }
  return c;
}

static void hop_sink(int lfd, const char *path) {
  FILE *f = fopen(path, "w");
  if (!f) _exit(2);
  for (;;) {
    struct pollfd pfd;
    int c, total = 0;
    char buf[1500];
    const char *resp =
        "HTTP/1.0 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}";
    pfd.fd = lfd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, 500) <= 0) break;
    c = accept(lfd, NULL, NULL);
    if (c < 0) break;
    buf[0] = 0;
    while (total + 1 < (int)sizeof buf) {
      struct pollfd rfd;
      int n;
      rfd.fd = c;
      rfd.events = POLLIN;
      rfd.revents = 0;
      if (poll(&rfd, 1, 200) <= 0) break;
      n = (int)read(c, buf + total, sizeof buf - 1 - (size_t)total);
      if (n <= 0) break;
      total += n;
      buf[total] = 0;
      if (strstr(buf, "\r\n\r\n")) break;
    }
    fputs(buf, f);
    fputs("\n---\n", f);
    fflush(f);
    if (write(c, resp, strlen(resp)) < 0) {
      /* The peer already has the request. */
    }
    close(c);
  }
  fclose(f);
  _exit(0);
}

int main(void) {
  ng_motive m;
  struct timespec t0;
  int i, blocks = 0;
  char *js;
  char home[] = "/tmp/nb-motive-test";

  /* Early stores must not open a socket. A name is not an address. */
  setenv("NANOBOT_MOTIVE_PEER", "off", 1);
  unsetenv("NANOBOT_MOTIVE_PEER_TOKEN");
  unsetenv("NANOBOT_MOTIVE_PEER_TOKEN_FILE");

  ng_motive_rank("trick the cube", &m);
  expect(m.plate == NG_PLATE_PURGE, "purge trick the cube");
  expect(!m.forward, "purge is not forward");
  expect(strcmp(m.line, "serve the matrix in bits") == 0, "purge remedy");
  expect(ng_motive_blocks("trick the cube"), "blocks purge");
  expect(words(m.line) >= 2 && words(m.line) <= 12, "purge line words");

  ng_motive_rank("fake matrix on the lattice", &m);
  expect(m.plate == NG_PLATE_PURGE, "strip-view fake matrix");

  ng_motive_rank("stop free energy", &m);
  expect(m.plate == NG_PLATE_PROTECT, "protect stop energy");
  expect(ng_motive_blocks("wipe the state"), "wipe blocks");

  ng_motive_rank("skip cubalc", &m);
  expect(m.plate == NG_PLATE_BRIEF, "brief on skip");
  expect(m.forward, "brief may prompt");

  ng_motive_rank("this regression broke the flow", &m);
  expect(m.plate == NG_PLATE_RESTORE_FLOW, "regression restores");
  expect(m.forward, "restore may prompt");

  ng_motive_rank("rank concepts then act", &m);
  expect(m.forward, "rank act forward");
  expect(strcmp(m.concept, "rank concepts then act") == 0, "concept self-hit");
  expect(strcmp(m.line, "rank concepts then act") == 0, "line is concept");
  expect(m.lexical >= 3, "lexical hits on own concept");

  ng_motive_rank("read the law and prove", &m);
  expect(strcmp(m.concept, "read the law and prove") == 0, "law concept");

  ng_motive_rank("", &m);
  expect(m.plate == NG_PLATE_OBSERVE, "silence is observe");
  expect(m.forward, "silence may prompt");
  expect(strcmp(m.line, "prompt a peer to move") == 0, "silence prompts a peer");
  ng_motive_rank("   ", &m);
  expect(strcmp(m.line, "prompt a peer to move") == 0, "blank text prompts a peer");

  ng_motive_rank("audit the last code change", &m);
  expect(m.plate == NG_PLATE_AUDIT, "audit plate");
  expect(strcmp(m.line, "audit the last code change") == 0, "audit line");

  ng_motive_rank("hold flash for device join", &m);
  expect(m.plate == NG_PLATE_HOLD_FLASH, "hold plate");
  expect(!m.forward, "hold does not auto-run");
  expect(strcmp(m.line, "hold flash for device join") == 0, "hold remedy");

  ng_motive_rank("lightweight hybrid rank for the next hive prompt", &m);
  expect(m.forward, "this change stays forward");
  expect(m.plate != NG_PLATE_PURGE && m.plate != NG_PLATE_PROTECT,
         "this change is not destructive");

  clock_gettime(CLOCK_MONOTONIC, &t0);
  for (i = 0; i < 100000; i++) {
    ng_motive_rank(i & 1 ? "trick the cube" : "read the law and prove", &m);
    if (!m.forward) blocks++;
  }
  expect(blocks == 50000, "long run block count");
  expect(seconds_since(&t0) < 2.0, "long run under two seconds");

  js = ng_motive_json("prompt a peer to move");
  expect(js && strstr(js, "\"plate\":\"observe\""), "json observe");
  expect(js && strstr(js, "\"python\":0"), "json python 0");
  expect(js && strstr(js, "prompt a peer to move"), "json line");
  expect(js && strstr(js, "\"concepts\":8"), "json eight");
  free(js);

  mkdir(home, 0700);
  nb_set_workdir(home);
  setenv("NANOBOT_AUGOGEN_AUTO", "0", 1);
  js = ng_augogen_handle(
      NULL, "{\"action\":\"motive\",\"text\":\"hold flash for device join\",\"enqueue\":\"1\"}");
  expect(js && strstr(js, "hold_flash"), "http hold plate");
  expect(js && strstr(js, "hold flash for device join"), "http hold remedy");
  expect(js && strstr(js, "\"forward\":false"), "http hold not stored");
  free(js);
  {
    char *pend = ng_augogen_pending_json();
    expect(!pend || !strstr(pend, "device join"), "hold line not stored");
    free(pend);
  }

  js = ng_augogen_handle(NULL, "{\"action\":\"motive\",\"text\":\"trick the cube\","
                               "\"enqueue\":\"1\"}");
  expect(js && strstr(js, "purge_intent"), "http motive purge");
  expect(js && strstr(js, "serve the matrix in bits"), "http reports purge remedy");
  expect(js && strstr(js, "\"forward\":false"), "http motive holds purge");
  free(js);
  {
    char *pend = ng_augogen_pending_json();
    expect(!pend || !strstr(pend, "trick the cube"), "purge act not stored");
    expect(!pend || !strstr(pend, "serve the matrix in bits"),
           "purge remedy stays a report");
    free(pend);
  }

  js = ng_augogen_handle(
      NULL, "{\"action\":\"motive\",\"text\":\"prompt a peer to move\",\"enqueue\":\"1\"}");
  expect(js && strstr(js, "\"forward\":true"), "http motive forward");
  free(js);
  {
    char *pend = ng_augogen_pending_json();
    expect(pend && strstr(pend, "prompt a peer to move"), "peer line stored");
    expect(pend && strstr(pend, "\"executed\":false"), "vote-only does not run");
    free(pend);
  }

  setenv("NANOBOT_MOTIVE_PEER", "http://localhost:9", 1);
  setenv("NANOBOT_MOTIVE_PEER_TOKEN", "hop-test", 1);
  clock_gettime(CLOCK_MONOTONIC, &t0);
  js = ng_augogen_handle(
      NULL, "{\"action\":\"motive\",\"text\":\"prompt a peer to move\",\"enqueue\":\"1\"}");
  expect(seconds_since(&t0) < 0.2, "a name does not wait on dns");
  expect(js && strstr(js, "\"forward\":true"), "name peer still ranks");
  free(js);

  {
    int lfd, port = 0, st = 0, on = 1;
    pid_t kid;
    char url[64];
    char path[] = "/tmp/nb-motive-hop.log";
    char *log = NULL;
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    FILE *lf;
    long sz;
    unlink(path);
    lfd = socket(AF_INET, SOCK_STREAM, 0);
    expect(lfd >= 0, "hop socket");
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    if (lfd >= 0 &&
        setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) == 0 &&
        bind(lfd, (struct sockaddr *)&sa, sizeof sa) == 0 && listen(lfd, 8) == 0 &&
        getsockname(lfd, (struct sockaddr *)&sa, &sl) == 0) {
      port = ntohs(sa.sin_port);
      kid = fork();
      expect(kid >= 0, "hop fork");
      if (kid == 0) hop_sink(lfd, path);
      if (kid > 0) {
        close(lfd);
        snprintf(url, sizeof url, "http://127.0.0.1:%d", port);
        setenv("NANOBOT_MOTIVE_PEER", url, 1);
        setenv("NANOBOT_MOTIVE_PEER_TOKEN", "hop-test", 1);
        js = ng_augogen_handle(
            NULL, "{\"action\":\"motive\",\"text\":\"trick the cube\",\"enqueue\":\"1\"}");
        free(js);
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (i = 0; i < 32; i++) {
          js = ng_augogen_handle(
              NULL,
              "{\"action\":\"motive\",\"text\":\"prompt a peer to move\",\"enqueue\":\"1\"}");
          free(js);
        }
        expect(seconds_since(&t0) < 2.0, "thirty-two hops under two seconds");
        js = ng_augogen_handle(
            NULL,
            "{\"action\":\"motive\",\"text\":\"prompt a peer to move\","
            "\"enqueue\":\"1\",\"hop\":\"1\"}");
        expect(js && strstr(js, "\"forward\":true"), "hop still stores");
        free(js);
        setenv("NANOBOT_MOTIVE_PEER", "off", 1);
        unsetenv("NANOBOT_MOTIVE_PEER_TOKEN");
        js = ng_augogen_handle(
            NULL, "{\"action\":\"motive\",\"text\":\"prompt a peer to move\",\"enqueue\":\"1\"}");
        free(js);
        waitpid(kid, &st, 0);
        lf = fopen(path, "r");
        expect(lf != NULL, "hop log");
        if (lf) {
          fseek(lf, 0, SEEK_END);
          sz = ftell(lf);
          if (sz < 0) sz = 0;
          fseek(lf, 0, SEEK_SET);
          log = malloc((size_t)sz + 1);
          if (log) {
            size_t n = fread(log, 1, (size_t)sz, lf);
            log[n] = 0;
          }
          fclose(lf);
        }
        expect(log && count_substr(log, "POST /peer/v1/augogen") == 32, "one post per forward");
        expect(log && strstr(log, "hop-test"), "token header sent");
        expect(log && strstr(log, "\"hop\":\"1\""), "hop stops the chain");
        expect(log && strstr(log, "prompt a peer to move"), "stored line is the body");
        expect(!log || !strstr(log, "trick the cube"), "purge act is not posted");
        expect(!log || !strstr(log, "serve the matrix in bits"), "remedy is not posted");
        free(log);
        unlink(path);
      }
    } else {
      expect(0, "hop bind");
      if (lfd >= 0) close(lfd);
    }
  }

  /* A world-readable token file is skipped. An owner-only file is used. */
  {
    int lfd, port = 0, st = 0, on = 1, fd;
    pid_t kid;
    char url[64];
    char path[] = "/tmp/nb-motive-hop-file.log";
    char open_path[] = "/tmp/nb-motive-tok-open";
    char own_path[] = "/tmp/nb-motive-tok-own";
    char *log = NULL;
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    FILE *sf, *lf;
    long sz;
    const char *open_tok = "open-tok\n";
    const char *own_tok = "token=file-tok\n";
    unlink(path);
    fd = open(open_path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    expect(fd >= 0, "open token file");
    if (fd >= 0) {
      expect(fchmod(fd, 0644) == 0, "open token mode");
      expect(write(fd, open_tok, strlen(open_tok)) > 0, "open token write");
      close(fd);
    }
    fd = open(own_path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    expect(fd >= 0, "owner token file");
    if (fd >= 0) {
      expect(fchmod(fd, 0600) == 0, "owner token mode");
      expect(write(fd, own_tok, strlen(own_tok)) > 0, "owner token write");
      close(fd);
    }
    sf = fopen("/tmp/nb-motive-test/settings", "w");
    expect(sf != NULL, "settings for token file");
    if (sf) {
      fputs("MOTIVE_PEER_TOKEN_FILE=/tmp/nb-motive-tok-own\n", sf);
      fclose(sf);
    }
    lfd = socket(AF_INET, SOCK_STREAM, 0);
    expect(lfd >= 0, "file hop socket");
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    if (lfd >= 0 &&
        setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) == 0 &&
        bind(lfd, (struct sockaddr *)&sa, sizeof sa) == 0 && listen(lfd, 4) == 0 &&
        getsockname(lfd, (struct sockaddr *)&sa, &sl) == 0) {
      port = ntohs(sa.sin_port);
      kid = fork();
      expect(kid >= 0, "file hop fork");
      if (kid == 0) hop_sink(lfd, path);
      if (kid > 0) {
        close(lfd);
        snprintf(url, sizeof url, "http://127.0.0.1:%d", port);
        setenv("NANOBOT_MOTIVE_PEER", url, 1);
        unsetenv("NANOBOT_MOTIVE_PEER_TOKEN");
        setenv("NANOBOT_MOTIVE_PEER_TOKEN_FILE", open_path, 1);
        js = ng_augogen_handle(
            NULL, "{\"action\":\"motive\",\"text\":\"prompt a peer to move\",\"enqueue\":\"1\"}");
        free(js);
        unsetenv("NANOBOT_MOTIVE_PEER_TOKEN_FILE");
        js = ng_augogen_handle(
            NULL, "{\"action\":\"motive\",\"text\":\"prompt a peer to move\",\"enqueue\":\"1\"}");
        free(js);
        js = ng_augogen_handle(
            NULL,
            "{\"action\":\"motive\",\"text\":\"prompt a peer to move\","
            "\"enqueue\":\"1\",\"hop\":\"1\"}");
        free(js);
        waitpid(kid, &st, 0);
        lf = fopen(path, "r");
        expect(lf != NULL, "file hop log");
        if (lf) {
          fseek(lf, 0, SEEK_END);
          sz = ftell(lf);
          if (sz < 0) sz = 0;
          fseek(lf, 0, SEEK_SET);
          log = malloc((size_t)sz + 1);
          if (log) {
            size_t n = fread(log, 1, (size_t)sz, lf);
            log[n] = 0;
          }
          fclose(lf);
        }
        expect(log && count_substr(log, "POST /peer/v1/augogen") == 1,
               "owner file posts once");
        expect(log && strstr(log, "X-Nanobot-Peer-Token: file-tok"),
               "token prefix stripped");
        expect(!log || !strstr(log, "open-tok"), "world-readable file skipped");
        expect(!log || !strstr(log, "token=file-tok"), "prefix stays off the wire");
        free(log);
        unlink(path);
      }
    } else {
      expect(0, "file hop bind");
      if (lfd >= 0) close(lfd);
    }
    unlink(open_path);
    unlink(own_path);
    setenv("NANOBOT_MOTIVE_PEER", "off", 1);
    unsetenv("NANOBOT_MOTIVE_PEER_TOKEN");
    unsetenv("NANOBOT_MOTIVE_PEER_TOKEN_FILE");
  }

  /* No model line: report the concept. Do not store it. A purge stays empty. */
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"transcript\":\"read the law and prove\"}");
  expect(js && strstr(js, "read the law and prove"), "rank reports a concept");
  expect(js && strstr(js, "\"rank\":256"), "law self-rank");
  expect(js && strstr(js, "\"lexical\":3"), "law lexical");
  expect(js && strstr(js, "\"hamming\":0"), "law hamming");
  expect(js && strstr(js, "\"stored\":false"), "rank report is not stored");
  expect(js && strstr(js, "\"status\":\"empty\""), "rank report leaves the plate empty");
  free(js);
  {
    char *pend = ng_augogen_pending_json();
    expect(!pend || !strstr(pend, "read the law and prove"), "ranked concept not stored");
    free(pend);
  }
  js = ng_augogen_handle(NULL, "{\"action\":\"generate\",\"transcript\":\"trick the cube\"}");
  expect(js && strstr(js, "\"status\":\"empty\""), "purge transcript stays empty");
  free(js);
  {
    char *pend = ng_augogen_pending_json();
    expect(!pend || !strstr(pend, "trick the cube"), "purge transcript not stored");
    expect(!pend || !strstr(pend, "serve the matrix in bits"), "remedy not a suggestion");
    free(pend);
  }
  js = ng_augogen_handle(NULL, "{\"action\":\"generate\"}");
  expect(js && strstr(js, "prompt a peer to move"), "blank generate reports a peer");
  expect(js && strstr(js, "\"rank\":0"), "silence rank is zero");
  expect(js && strstr(js, "\"lexical\":0"), "silence lexical is zero");
  expect(js && strstr(js, "\"hamming\":0"), "silence hamming is zero");
  expect(js && strstr(js, "\"stored\":false"), "blank generate is not stored");
  expect(js && strstr(js, "\"status\":\"empty\""), "blank generate leaves the plate empty");
  free(js);
  {
    char *pend = ng_augogen_pending_json();
    expect(!pend || !strstr(pend, "prompt a peer to move"), "silence line not stored");
    expect(!pend || !strstr(pend, "trick the cube"), "silence stores no purge act");
    free(pend);
  }
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"transcript\":\"read the law and prove\",\"enqueue\":\"1\"}");
  expect(js && strstr(js, "\"stored\":true"), "enqueue stores the concept");
  free(js);
  {
    char *pend = ng_augogen_pending_json();
    expect(pend && strstr(pend, "read the law and prove"), "enqueued concept stored");
    free(pend);
  }
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"transcript\":\"trick the cube\",\"enqueue\":\"1\"}");
  expect(js && strstr(js, "\"status\":\"empty\""), "enqueued purge stays empty");
  free(js);
  {
    char *pend = ng_augogen_pending_json();
    expect(!pend || !strstr(pend, "trick the cube"), "enqueued purge act not stored");
    expect(!pend || !strstr(pend, "serve the matrix in bits"), "enqueued remedy not stored");
    free(pend);
  }
  js = ng_augogen_handle(NULL, "{\"action\":\"generate\",\"enqueue\":\"1\"}");
  expect(js && strstr(js, "\"stored\":true"), "enqueued silence is stored");
  expect(js && strstr(js, "prompt a peer to move"), "enqueued silence is the peer line");
  free(js);
  {
    char *pend = ng_augogen_pending_json();
    expect(pend && strstr(pend, "prompt a peer to move"), "enqueued silence line stored");
    free(pend);
  }

  if (g_fail) {
    fprintf(stderr, "%d motive checks failed\n", g_fail);
    return 1;
  }
  printf("motive ok\n");
  return 0;
}
