#include "augogen.h"
#include "motive.h"

#include <nanobot/os.h>

#include <arpa/inet.h>
#include <dirent.h>
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

static void job_id_of(const char *js, char *out, size_t n) {
  const char *p;
  size_t i = 0;
  if (!out || n < 2) return;
  out[0] = 0;
  p = js ? strstr(js, "\"job_id\":\"") : NULL;
  if (!p) return;
  p += 10;
  while (p[i] && p[i] != '"' && i + 1 < n) {
    out[i] = p[i];
    i++;
  }
  out[i] = 0;
}

/* agrun-<sec>-<pid> with an optional -N suffix share one second. */
static int same_job_slot(const char *a, const char *b) {
  const char *as, *bs, *ae, *be;
  size_t an, bn;
  if (!a || !b || strncmp(a, "agrun-", 6) != 0 || strncmp(b, "agrun-", 6) != 0)
    return 0;
  as = a + 6;
  bs = b + 6;
  ae = strchr(as, '-');
  be = strchr(bs, '-');
  if (!ae || !be) return 0;
  if ((size_t)(ae - as) != (size_t)(be - bs) || memcmp(as, bs, (size_t)(ae - as)) != 0)
    return 0;
  as = ae + 1;
  bs = be + 1;
  ae = strchr(as, '-');
  be = strchr(bs, '-');
  an = ae ? (size_t)(ae - as) : strlen(as);
  bn = be ? (size_t)(be - bs) : strlen(bs);
  return an == bn && an > 0 && memcmp(as, bs, an) == 0;
}

static int file_eq(const char *path, const char *want) {
  FILE *f;
  char buf[80];
  size_t n;
  if (!path || !want) return 0;
  f = fopen(path, "r");
  if (!f) return 0;
  n = fread(buf, 1, sizeof buf - 1, f);
  fclose(f);
  buf[n] = 0;
  return strcmp(buf, want) == 0;
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
  expect(m.rank == 256, "default lexical weight is 64");
  expect(m.lex_w == 64, "default lex_w is 64");
  setenv("NANOBOT_MOTIVE_LEX_W", "1", 1);
  ng_motive_rank("read the law and prove", &m);
  expect(m.rank == 67, "lexical weight 1 keeps the hamming term");
  expect(m.lex_w == 1, "weight 1 is reported");
  expect(m.lexical == 3 && m.hamming == 0, "weight leaves the feature counts");
  expect(m.forward, "weight leaves a forward plate forward");
  ng_motive_rank("trick the cube", &m);
  expect(m.plate == NG_PLATE_PURGE && !m.forward, "weight leaves a purge closed");
  ng_motive_rank("", &m);
  expect(m.rank == 0, "silence ignores the weight");
  expect(m.lex_w == 1, "silence still reports the weight");
  setenv("NANOBOT_MOTIVE_LEX_W", "256", 1);
  ng_motive_rank("read the law and prove", &m);
  expect(m.rank == 256 && m.lex_w == 64, "weight above 255 stays 64");
  setenv("NANOBOT_MOTIVE_LEX_W", "nope", 1);
  ng_motive_rank("read the law and prove", &m);
  expect(m.rank == 256 && m.lex_w == 64, "a bad weight stays 64");
  unsetenv("NANOBOT_MOTIVE_LEX_W");
  ng_motive_rank_w("read the law and prove", &m, 1);
  expect(m.rank == 67 && m.lex_w == 1, "one call can weigh lexical at 1");
  ng_motive_rank_w("trick the cube", &m, 1);
  expect(m.plate == NG_PLATE_PURGE && !m.forward, "a call weight leaves a purge closed");
  ng_motive_rank_w("", &m, 1);
  expect(m.rank == 0 && m.lex_w == 1, "silence stays rank 0 and names the call weight");
  ng_motive_rank_w("read the law and prove", &m, 256);
  expect(m.rank == 256 && m.lex_w == 64, "a call weight above 255 uses the env");
  ng_motive_rank("read the law and prove", &m);
  expect(m.rank == 256 && m.lex_w == 64, "the env rank is unchanged");

  ng_motive_rank("read read the law and prove", &m);
  expect(m.rank == 256 && m.lexical == 3 && m.hamming == 0,
         "a doubled word stays on the law");
  expect(strcmp(m.concept, "read the law and prove") == 0,
         "a doubled word keeps the law concept");
  ng_motive_rank("read the the law and prove", &m);
  expect(m.rank == 256 && m.lexical == 3 && m.hamming == 0,
         "a doubled glue word stays on the law");
  ng_motive_rank("READ READ THE LAW AND PROVE", &m);
  expect(m.rank == 256 && m.lexical == 3 && m.hamming == 0,
         "a doubled word keeps its case fold");
  ng_motive_rank("pair pair vote then ship the step", &m);
  expect(m.rank == 320 && m.lexical == 4 && m.hamming == 0,
         "a doubled word stays on a four-word concept");
  expect(strcmp(m.concept, "pair vote then ship the step") == 0,
         "a doubled word keeps that concept");
  ng_motive_rank("fold fold the matrix and decide", &m);
  expect(m.rank == 256 && m.lexical == 3 && m.hamming == 0,
         "a doubled word stays on the first concept");
  ng_motive_rank("confess confess the heat and serve", &m);
  expect(m.plate == NG_PLATE_CONFESS && m.rank == 256 && m.lexical == 3 &&
             m.hamming == 0,
         "a doubled confess word keeps the plate");
  expect(strcmp(m.concept, "confess the heat and serve") == 0,
         "a doubled confess word keeps that concept");
  ng_motive_rank("read read the law", &m);
  expect(m.rank == 179 && m.lexical == 2 && m.hamming == 13,
         "a doubled word on a partial line stays partial");
  expect(strcmp(m.concept, "read the law and prove") == 0,
         "a partial line keeps the law concept");
  ng_motive_rank("please read the law and prove", &m);
  expect(m.rank == 246 && m.lexical == 3 && m.hamming == 10,
         "an extra word stays off zero");
  ng_motive_rank("read the law and prove", &m);
  expect(m.rank == 256 && m.lexical == 3 && m.hamming == 0,
         "the exact law line stays rank 256");

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
  expect(js && strstr(js, "\"lex_w\":64"), "json names lex_w");
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
    expect(pend && strstr(pend, "\"rank\":256"), "stored peer keeps rank");
    expect(pend && strstr(pend, "\"lexical\":3"), "stored peer keeps lexical");
    expect(pend && strstr(pend, "\"hamming\":0"), "stored peer keeps hamming");
    expect(pend && strstr(pend, "\"plate\":\"observe\""), "stored peer keeps the plate");
    expect(!pend || !strstr(pend, "\"lex_w\":"), "stored peer omits lex_w");
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
        expect(!log || !strstr(log, "\"lex_w\""), "unset weight stays off the hop");
        expect(log && count_substr(log, "\"plate\":\"observe\"") == 32,
               "hop names the plate");
        expect(log && count_substr(log, "\"lexical\":\"3\"") == 32,
               "hop names the lexical count");
        expect(log && count_substr(log, "\"hamming\":\"0\"") == 32,
               "hop names the hamming");
        expect(log && count_substr(log, "\"concept\":\"prompt a peer to move\"") == 32,
               "hop names the concept");
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

  /* A quoted lex_w rides the one hop. A missing value stays off that POST. */
  {
    int lfd, port = 0, st = 0, on = 1;
    pid_t kid;
    char url[64];
    char path[] = "/tmp/nb-motive-hop-lex.log";
    char *log = NULL;
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    FILE *lf;
    long sz;
    unlink(path);
    lfd = socket(AF_INET, SOCK_STREAM, 0);
    expect(lfd >= 0, "lex hop socket");
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
      expect(kid >= 0, "lex hop fork");
      if (kid == 0) hop_sink(lfd, path);
      if (kid > 0) {
        char *pend;
        close(lfd);
        snprintf(url, sizeof url, "http://127.0.0.1:%d", port);
        setenv("NANOBOT_MOTIVE_PEER", url, 1);
        setenv("NANOBOT_MOTIVE_PEER_TOKEN", "hop-test", 1);
        js = ng_augogen_handle(
            NULL,
            "{\"action\":\"motive\",\"text\":\"read the law and prove\","
            "\"enqueue\":\"1\",\"lex_w\":\"1\"}");
        expect(js && strstr(js, "\"rank\":67") && strstr(js, "\"lex_w\":1"),
               "hop call still reports the quoted weight");
        free(js);
        pend = ng_augogen_pending_json();
        expect(pend && strstr(pend, "\"plate\":\"observe\""), "lex hop stores the plate");
        expect(pend && strstr(pend, "\"rank\":67"), "lex hop stores the weighed rank");
        expect(!pend || !strstr(pend, "\"lex_w\":"), "lex hop plate omits lex_w");
        free(pend);
        js = ng_augogen_handle(
            NULL,
            "{\"action\":\"motive\",\"text\":\"prompt a peer to move\","
            "\"enqueue\":\"1\",\"lex_w\":\"0\"}");
        free(js);
        js = ng_augogen_handle(
            NULL,
            "{\"action\":\"motive\",\"text\":\"prompt a peer to move\","
            "\"enqueue\":\"1\",\"lex_w\":\"nope\"}");
        free(js);
        js = ng_augogen_handle(
            NULL,
            "{\"action\":\"motive\",\"text\":\"rank concepts then act\","
            "\"enqueue\":\"1\",\"hop\":\"1\",\"lex_w\":\"1\"}");
        expect(js && strstr(js, "\"forward\":true"), "weighted hop still stores");
        free(js);
        js = ng_augogen_handle(
            NULL,
            "{\"action\":\"motive\",\"text\":\"trick the cube\","
            "\"enqueue\":\"1\",\"lex_w\":\"1\"}");
        expect(js && strstr(js, "\"forward\":false"), "weighted purge stays closed");
        free(js);
        js = ng_augogen_handle(
            NULL,
            "{\"action\":\"generate\",\"transcript\":\"read the law and prove\","
            "\"enqueue\":\"1\",\"lex_w\":\"1\"}");
        free(js);
        setenv("NANOBOT_MOTIVE_PEER", "off", 1);
        unsetenv("NANOBOT_MOTIVE_PEER_TOKEN");
        waitpid(kid, &st, 0);
        lf = fopen(path, "r");
        expect(lf != NULL, "lex hop log");
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
        expect(log && count_substr(log, "POST /peer/v1/augogen") == 3,
               "quoted weight posts with the forward line");
        expect(log && count_substr(log, "\"lex_w\":\"1\"") == 1,
               "weight 1 is quoted once");
        expect(log && count_substr(log, "\"lex_w\":\"0\"") == 1,
               "weight 0 is quoted once");
        expect(log && count_substr(log, "\"lex_w\"") == 2,
               "a bad weight stays off the hop");
        expect(log && strstr(log, "read the law and prove"), "weighed line is the body");
        expect(log && count_substr(log, "\"plate\":\"observe\"") == 3,
               "weighed hop names the plate");
        expect(log && count_substr(log, "\"lexical\":\"3\"") == 3,
               "weighed hop names the lexical count");
        expect(log && count_substr(log, "\"hamming\":\"0\"") == 3,
               "weighed hop names the hamming");
        expect(log && count_substr(log, "\"concept\":") == 3,
               "weighed hop names the concept");
        expect(log && strstr(log, "\"concept\":\"read the law and prove\""),
               "weighed hop names the law concept");
        expect(!log || !strstr(log, "rank concepts then act"), "hop does not post the weight");
        expect(!log || !strstr(log, "trick the cube"), "weighed purge is not posted");
        pend = ng_augogen_pending_json();
        expect(pend && strstr(pend, "\"plate\":\"observe\""), "later store keeps the plate");
        expect(!pend || !strstr(pend, "\"lex_w\":"), "later store omits lex_w");
        free(pend);
        free(log);
        unlink(path);
      }
    } else {
      expect(0, "lex hop bind");
      if (lfd >= 0) close(lfd);
    }
    setenv("NANOBOT_MOTIVE_PEER", "off", 1);
    unsetenv("NANOBOT_MOTIVE_PEER_TOKEN");
  }

  /* The hop quotes the posted line. The sender plate keeps the act. */
  {
    int lfd, port = 0, st = 0, on = 1;
    pid_t kid;
    char url[64];
    char path[] = "/tmp/nb-motive-hop-body.log";
    char *log = NULL;
    char *pend;
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    FILE *lf;
    long sz;
    unlink(path);
    lfd = socket(AF_INET, SOCK_STREAM, 0);
    expect(lfd >= 0, "body hop socket");
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
      expect(kid >= 0, "body hop fork");
      if (kid == 0) hop_sink(lfd, path);
      if (kid > 0) {
        close(lfd);
        snprintf(url, sizeof url, "http://127.0.0.1:%d", port);
        setenv("NANOBOT_MOTIVE_PEER", url, 1);
        setenv("NANOBOT_MOTIVE_PEER_TOKEN", "hop-test", 1);
        js = ng_augogen_handle(
            NULL, "{\"action\":\"motive\",\"text\":\"read the law\",\"enqueue\":\"1\"}");
        expect(js && strstr(js, "\"rank\":185") && strstr(js, "\"lexical\":2") &&
                   strstr(js, "\"hamming\":7"),
               "partial act still reports its own rank");
        expect(js && strstr(js, "\"concept\":\"read the law and prove\""),
               "partial act still names the law");
        free(js);
        pend = ng_augogen_pending_json();
        expect(pend && strstr(pend, "\"rank\":185") && strstr(pend, "\"lexical\":2") &&
                   strstr(pend, "\"hamming\":7"),
               "sender plate keeps the partial act");
        free(pend);
        js = ng_augogen_handle(
            NULL, "{\"action\":\"motive\",\"text\":\"you lied\",\"enqueue\":\"1\"}");
        expect(js && strstr(js, "\"plate\":\"confess\"") && strstr(js, "\"lexical\":0") &&
                   strstr(js, "\"rank\":43"),
               "bare confess still reports the sketch");
        free(js);
        pend = ng_augogen_pending_json();
        expect(pend && strstr(pend, "\"plate\":\"confess\"") && strstr(pend, "\"lexical\":0") &&
                   strstr(pend, "\"rank\":43"),
               "sender plate keeps the bare confess");
        expect(pend && strstr(pend, "rank concepts then act"),
               "sender stores the sketch line");
        free(pend);
        js = ng_augogen_handle(
            NULL, "{\"action\":\"motive\",\"text\":\"\",\"enqueue\":\"1\"}");
        expect(js && strstr(js, "\"rank\":0") && strstr(js, "\"lexical\":0"),
               "silence still reports rank 0");
        free(js);
        setenv("NANOBOT_MOTIVE_PEER", "off", 1);
        unsetenv("NANOBOT_MOTIVE_PEER_TOKEN");
        waitpid(kid, &st, 0);
        lf = fopen(path, "r");
        expect(lf != NULL, "body hop log");
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
        expect(log && count_substr(log, "POST /peer/v1/augogen") == 3, "three lines posted");
        expect(log && count_substr(log, "\"lexical\":\"3\"") == 3,
               "each posted line names its own lexical count");
        expect(log && count_substr(log, "\"hamming\":\"0\"") == 3,
               "each posted line names its own hamming");
        expect(log && count_substr(log, "\"plate\":\"observe\"") == 3,
               "each posted line names its own plate");
        expect(log && strstr(log, "\"concept\":\"read the law and prove\""),
               "partial hop names the law line");
        expect(log && strstr(log, "\"concept\":\"rank concepts then act\""),
               "bare confess hop names the sketch line");
        expect(log && strstr(log, "\"concept\":\"prompt a peer to move\""),
               "silence hop names the peer line");
        expect(!log || !strstr(log, "\"lexical\":\"0\""), "the act count stays off the hop");
        expect(!log || !strstr(log, "\"lexical\":\"2\""), "the partial count stays off the hop");
        expect(!log || !strstr(log, "\"plate\":\"confess\""), "the act plate stays off the hop");
        expect(!log || !strstr(log, "you lied"), "the act text is not the body");
        expect(!log || !strstr(log, "read the law\""), "the partial act is not the body");
        free(log);
        unlink(path);
      }
    } else {
      expect(0, "body hop bind");
      if (lfd >= 0) close(lfd);
    }
    setenv("NANOBOT_MOTIVE_PEER", "off", 1);
    unsetenv("NANOBOT_MOTIVE_PEER_TOKEN");
  }

  /* A quoted plate that disagrees with the rank is not stored. */
  js = ng_augogen_handle(NULL, "{\"action\":\"reset\"}");
  free(js);
  js = ng_augogen_handle(
      NULL,
      "{\"action\":\"motive\",\"text\":\"read the law and prove\","
      "\"enqueue\":\"1\",\"plate\":\"purge_intent\",\"lex_w\":\"1\"}");
  expect(js && strstr(js, "\"rank\":67") && strstr(js, "\"forward\":true"),
         "a disagreed plate still reports the rank");
  expect(js && strstr(js, "\"plate\":\"observe\""), "the report keeps the ranked plate");
  free(js);
  {
    char *pend = ng_augogen_pending_json();
    expect(!pend || !strstr(pend, "read the law and prove"), "a disagreed plate is not stored");
    expect(!pend || !strstr(pend, "\"rank\":"), "a disagreed plate leaves the file empty");
    free(pend);
  }
  js = ng_augogen_handle(
      NULL,
      "{\"action\":\"motive\",\"text\":\"read the law and prove\","
      "\"enqueue\":\"1\",\"plate\":\"observe\",\"lex_w\":\"1\"}");
  expect(js && strstr(js, "\"rank\":67"), "a matching plate still weighs the call");
  free(js);
  {
    char *pend = ng_augogen_pending_json();
    expect(pend && strstr(pend, "\"plate\":\"observe\""), "a matching plate is stored");
    expect(pend && strstr(pend, "\"rank\":67"), "a matching plate keeps the weighed rank");
    expect(!pend || !strstr(pend, "\"lex_w\":"), "a matching plate omits lex_w");
    free(pend);
  }
  js = ng_augogen_handle(NULL, "{\"action\":\"reset\"}");
  free(js);
  js = ng_augogen_handle(
      NULL,
      "{\"action\":\"motive\",\"text\":\"read the law and prove\","
      "\"enqueue\":\"1\",\"lexical\":\"9\",\"hamming\":\"0\"}");
  expect(js && strstr(js, "\"rank\":256") && strstr(js, "\"lexical\":3"),
         "a disagreed lexical still reports the rank");
  free(js);
  {
    char *pend = ng_augogen_pending_json();
    expect(!pend || !strstr(pend, "read the law and prove"),
           "a disagreed lexical is not stored");
    expect(!pend || !strstr(pend, "\"rank\":"), "a disagreed lexical leaves the file empty");
    free(pend);
  }
  js = ng_augogen_handle(
      NULL,
      "{\"action\":\"motive\",\"text\":\"read the law and prove\","
      "\"enqueue\":\"1\",\"lexical\":\"3\",\"hamming\":\"0\"}");
  free(js);
  {
    char *pend = ng_augogen_pending_json();
    expect(pend && strstr(pend, "\"rank\":256"), "a matching lexical is stored");
    expect(pend && strstr(pend, "\"lexical\":3"), "a matching lexical keeps the count");
    expect(pend && strstr(pend, "\"hamming\":0"), "a matching lexical keeps the hamming");
    expect(!pend || !strstr(pend, "\"lex_w\":"), "a matching lexical omits lex_w");
    free(pend);
  }
  js = ng_augogen_handle(NULL, "{\"action\":\"reset\"}");
  free(js);
  js = ng_augogen_handle(
      NULL,
      "{\"action\":\"motive\",\"text\":\"read the law and prove\","
      "\"enqueue\":\"1\",\"concept\":\"prompt a peer to move\"}");
  expect(js && strstr(js, "\"rank\":256") && strstr(js, "\"lexical\":3"),
         "a disagreed concept still reports the rank");
  expect(js && strstr(js, "\"concept\":\"read the law and prove\""),
         "the report keeps the ranked concept");
  free(js);
  {
    char *pend = ng_augogen_pending_json();
    expect(!pend || !strstr(pend, "read the law and prove"),
           "a disagreed concept is not stored");
    expect(!pend || !strstr(pend, "\"rank\":"), "a disagreed concept leaves the file empty");
    free(pend);
  }
  js = ng_augogen_handle(
      NULL,
      "{\"action\":\"motive\",\"text\":\"read the law and prove\","
      "\"enqueue\":\"1\",\"concept\":\"read the law and prove\"}");
  free(js);
  {
    char *pend = ng_augogen_pending_json();
    expect(pend && strstr(pend, "\"rank\":256"), "a matching concept is stored");
    expect(pend && strstr(pend, "read the law and prove"), "a matching concept keeps the line");
    expect(pend && strstr(pend, "\"lexical\":3"), "a matching concept keeps the count");
    expect(pend && strstr(pend, "\"hamming\":0"), "a matching concept keeps the hamming");
    expect(!pend || !strstr(pend, "\"lex_w\":"), "a matching concept omits lex_w");
    free(pend);
  }

  /* A JSON unicode space is a space before the rank. A NUL is not planted. */
  js = ng_augogen_handle(
      NULL, "{\"action\":\"motive\",\"text\":\"read the law\\u0020and prove\"}");
  expect(js && strstr(js, "\"rank\":256"), "unicode space still ranks 256");
  expect(js && strstr(js, "\"lexical\":3"), "unicode space keeps lexical 3");
  expect(js && strstr(js, "\"hamming\":0"), "unicode space keeps hamming 0");
  expect(js && strstr(js, "\"concept\":\"read the law and prove\""),
         "unicode space names the law concept");
  free(js);
  js = ng_augogen_handle(
      NULL, "{\"action\":\"motive\",\"text\":\"read the law\\u0000 and prove\"}");
  expect(js && strstr(js, "\"rank\":256") &&
             strstr(js, "\"concept\":\"read the law and prove\""),
         "a unicode NUL does not cut the law line");
  free(js);

  /* A JSON backspace or form feed is a separator before the rank. */
  js = ng_augogen_handle(
      NULL, "{\"action\":\"motive\",\"text\":\"read the law\\band prove\"}");
  expect(js && strstr(js, "\"rank\":256"), "backspace still ranks 256");
  expect(js && strstr(js, "\"lexical\":3"), "backspace keeps lexical 3");
  expect(js && strstr(js, "\"hamming\":0"), "backspace keeps hamming 0");
  expect(js && strstr(js, "\"concept\":\"read the law and prove\""),
         "backspace names the law concept");
  free(js);
  js = ng_augogen_handle(
      NULL, "{\"action\":\"motive\",\"text\":\"read the law\\fand prove\"}");
  expect(js && strstr(js, "\"rank\":256") && strstr(js, "\"lexical\":3") &&
             strstr(js, "\"hamming\":0") &&
             strstr(js, "\"concept\":\"read the law and prove\""),
         "a form feed still names the law concept");
  free(js);

  /* No model line: report the concept. Do not store it. A purge stays empty. */
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"transcript\":\"read the law and prove\"}");
  expect(js && strstr(js, "read the law and prove"), "rank reports a concept");
  expect(js && strstr(js, "\"plate\":\"observe\""), "law report names the plate");
  expect(js && count_substr(js, "\"plate\":") == 1, "report has one plate");
  expect(js && strstr(js, "\"rank\":256"), "law self-rank");
  expect(js && strstr(js, "\"lexical\":3"), "law lexical");
  expect(js && strstr(js, "\"hamming\":0"), "law hamming");
  expect(js && strstr(js, "\"lex_w\":64"), "law report names lex_w");
  expect(js && count_substr(js, "\"lex_w\":") == 1, "report has one lex_w");
  free(js);
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"transcript\":\"read the law and prove\",\"lex_w\":\"1\"}");
  expect(js && strstr(js, "\"rank\":256") && strstr(js, "\"lex_w\":64"),
         "generate keeps the process weight");
  expect(js && strstr(js, "\"stored\":false"), "generate lex_w does not store");
  expect(js && count_substr(js, "\"rank\":") == 1, "report has one rank");
  expect(js && strstr(js, "\"stored\":false"), "rank report is not stored");
  expect(js && strstr(js, "\"status\":\"empty\""), "rank report leaves the plate empty");
  free(js);
  js = ng_augogen_handle(
      NULL, "{\"action\":\"motive\",\"text\":\"read the law and prove\",\"lex_w\":\"1\"}");
  expect(js && strstr(js, "\"rank\":67") && strstr(js, "\"lex_w\":1"),
         "quoted lex_w weighs this motive call");
  expect(js && strstr(js, "\"forward\":true"), "call weight stays forward");
  free(js);
  {
    char *pend = ng_augogen_pending_json();
    expect(!pend || !strstr(pend, "\"lex_w\":"), "call weight is not stored");
    expect(!pend || !strstr(pend, "read the law and prove"), "call weight does not enqueue");
    free(pend);
  }
  js = ng_augogen_handle(
      NULL, "{\"action\":\"motive\",\"text\":\"trick the cube\",\"lex_w\":\"1\"}");
  expect(js && strstr(js, "purge_intent") && strstr(js, "\"forward\":false"),
         "call weight leaves a purge closed");
  free(js);
  js = ng_augogen_handle(
      NULL, "{\"action\":\"motive\",\"text\":\"read the law and prove\",\"lex_w\":\"nope\"}");
  expect(js && strstr(js, "\"rank\":256") && strstr(js, "\"lex_w\":64"),
         "a bad call weight keeps the process weight");
  free(js);
  {
    char *pend = ng_augogen_pending_json();
    expect(!pend || !strstr(pend, "read the law and prove"), "ranked concept not stored");
    expect(!pend || !strstr(pend, "\"rank\":"), "empty plate omits rank");
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
  expect(js && strstr(js, "\"plate\":\"observe\""), "silence report names observe");
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
  expect(js && count_substr(js, "\"rank\":") == 1, "stored response has one rank");
  expect(js && count_substr(js, "\"lex_w\":") == 1, "stored response has one lex_w");
  free(js);
  {
    char *pend = ng_augogen_pending_json();
    expect(pend && strstr(pend, "read the law and prove"), "enqueued concept stored");
    expect(pend && strstr(pend, "\"rank\":256"), "stored plate keeps rank");
    expect(pend && strstr(pend, "\"lexical\":3"), "stored plate keeps lexical");
    expect(pend && strstr(pend, "\"hamming\":0"), "stored plate keeps hamming");
    expect(pend && strstr(pend, "\"plate\":\"observe\""), "stored plate keeps the plate name");
    expect(!pend || !strstr(pend, "\"lex_w\":"), "stored plate omits lex_w");
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
    expect(!pend || !strstr(pend, "\"rank\":"), "purge plate omits rank");
    free(pend);
  }
  js = ng_augogen_handle(NULL, "{\"action\":\"generate\",\"enqueue\":\"1\"}");
  expect(js && strstr(js, "\"stored\":true"), "enqueued silence is stored");
  expect(js && strstr(js, "prompt a peer to move"), "enqueued silence is the peer line");
  free(js);
  {
    char *pend = ng_augogen_pending_json();
    expect(pend && strstr(pend, "prompt a peer to move"), "enqueued silence line stored");
    expect(pend && strstr(pend, "\"rank\":0"), "stored silence keeps rank");
    expect(pend && strstr(pend, "\"lexical\":0"), "stored silence keeps lexical");
    expect(pend && strstr(pend, "\"hamming\":0"), "stored silence keeps hamming");
    expect(pend && strstr(pend, "\"plate\":\"observe\""), "stored silence keeps the plate");
    expect(!pend || !strstr(pend, "\"lex_w\":"), "stored silence omits lex_w");
    free(pend);
  }
  /* Generate enqueue stays local. The motive action is the peer POST. */
  {
    int lfd, port = 0, st = 0, on = 1;
    pid_t kid;
    char url[64];
    char path[] = "/tmp/nb-motive-gen-hop.log";
    char *log = NULL;
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    FILE *lf;
    long sz;
    unlink(path);
    lfd = socket(AF_INET, SOCK_STREAM, 0);
    expect(lfd >= 0, "generate hop socket");
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
      expect(kid >= 0, "generate hop fork");
      if (kid == 0) hop_sink(lfd, path);
      if (kid > 0) {
        close(lfd);
        snprintf(url, sizeof url, "http://127.0.0.1:%d", port);
        setenv("NANOBOT_MOTIVE_PEER", url, 1);
        setenv("NANOBOT_MOTIVE_PEER_TOKEN", "hop-test", 1);
        js = ng_augogen_handle(
            NULL, "{\"action\":\"generate\",\"transcript\":\"trick the cube\",\"enqueue\":\"1\"}");
        free(js);
        js = ng_augogen_handle(
            NULL,
            "{\"action\":\"generate\",\"transcript\":\"read the law and prove\",\"enqueue\":\"1\"}");
        free(js);
        js = ng_augogen_handle(NULL, "{\"action\":\"generate\",\"transcript\":\"read the law and prove\"}");
        free(js);
        setenv("NANOBOT_MOTIVE_PEER", "off", 1);
        unsetenv("NANOBOT_MOTIVE_PEER_TOKEN");
        waitpid(kid, &st, 0);
        lf = fopen(path, "r");
        expect(lf != NULL, "generate hop log");
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
        expect(!log || count_substr(log, "POST /peer/v1/augogen") == 0, "generate does not post");
        expect(!log || !strstr(log, "read the law and prove"), "generate keeps the concept local");
        expect(!log || !strstr(log, "trick the cube"), "generate purge is not posted");
        free(log);
        unlink(path);
      }
    } else {
      expect(0, "generate hop bind");
      if (lfd >= 0) close(lfd);
    }
  }

  /* A typographic quote trims with the ASCII quotes. The words stay. */
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\","
            "\"suggestion\":\"\\u201cread the law and prove\\u201d\"}");
  expect(js && strstr(js, "\"suggestion\":\"read the law and prove\""),
         "a typographic quote stays off the suggestion");
  free(js);
  {
    char *pend = ng_augogen_pending_json();
    expect(pend && strstr(pend, "\"suggestion\":\"read the law and prove\",\"status\""),
           "the stored suggestion keeps the law words");
    free(pend);
  }
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\","
            "\"suggestion\":\"\\\"read the law and prove.\\\"\"}");
  expect(js && strstr(js, "\"suggestion\":\"read the law and prove\""),
         "an ascii quote and a period stay off the suggestion");
  free(js);

  /* A unicode space beside a quote trims with the quote. The words stay. */
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\","
            "\"suggestion\":\"\\u00a0\\u201cread the law and prove\\u201d\"}");
  expect(js && strstr(js, "\"suggestion\":\"read the law and prove\""),
         "a unicode space stays off the quoted suggestion");
  free(js);
  {
    char *pend = ng_augogen_pending_json();
    expect(pend && strstr(pend, "\"suggestion\":\"read the law and prove\",\"status\""),
           "the stored suggestion keeps the unpadded law words");
    free(pend);
  }
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\","
            "\"suggestion\":\"\\u201c read the law and prove \\u201d\"}");
  expect(js && strstr(js, "\"suggestion\":\"read the law and prove\""),
         "a space inside the quotes stays off the suggestion");
  free(js);
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\","
            "\"suggestion\":\"read the law and prove\\t\"}");
  expect(js && strstr(js, "\"suggestion\":\"read the law and prove\""),
         "a trailing tab stays off the suggestion");
  free(js);
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\","
            "\"suggestion\":\"read the law and prove\\u2014\"}");
  expect(js && strstr(js, "\"suggestion\":\"read the law and prove\xe2\x80\x94\""),
         "an em dash stays on the suggestion");
  free(js);

  /* A unicode space inside the line is a space. The words stay. */
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\","
            "\"suggestion\":\"read\\u00a0the law and prove\"}");
  expect(js && strstr(js, "\"suggestion\":\"read the law and prove\""),
         "a unicode space inside the line is a space");
  free(js);
  {
    char *pend = ng_augogen_pending_json();
    expect(pend && strstr(pend, "\"suggestion\":\"read the law and prove\",\"status\""),
           "the stored suggestion keeps the separated law words");
    free(pend);
  }

  /* A torn byte drops, so a quote behind it still drops. The words stay. */
  {
    char body[240];
    char *pend;
    snprintf(body, sizeof body,
             "{\"action\":\"generate\",\"enqueue\":\"1\",\"suggestion\":\"%s%sread the law and prove%s\"}",
             "\xE2", "\xE2\x80\x9C", "\xE2\x80\x9D");
    js = ng_augogen_handle(NULL, body);
    expect(js && strstr(js, "\"suggestion\":\"read the law and prove\""),
           "a torn byte does not keep the quote");
    free(js);
    pend = ng_augogen_pending_json();
    expect(pend && strstr(pend, "\"suggestion\":\"read the law and prove\",\"status\""),
           "the stored suggestion drops the torn byte and the quote");
    free(pend);
  }

  /* A line outside 2–12 words, or a meta line, is not stored. */
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\",\"suggestion\":\"hello\"}");
  expect(js && strstr(js, "\"status\":\"empty\""), "a one-word line is not stored");
  expect(js && strstr(js, "\"suggestion\":\"\""), "a one-word line leaves no words");
  expect(js && !strstr(js, "prompt a peer to move"), "a one-word line is not the peer line");
  free(js);
  {
    char *pend = ng_augogen_pending_json();
    expect(pend && strstr(pend, "\"suggestion\":\"\"") && strstr(pend, "\"status\":\"empty\""),
           "a one-word line stays off the plate");
    free(pend);
  }
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\",\"suggestion\":\"silence\"}");
  expect(js && strstr(js, "\"status\":\"empty\"") && strstr(js, "\"suggestion\":\"\""),
         "a meta line such as silence is not stored");
  expect(js && !strstr(js, "prompt a peer to move"), "a meta line is not the peer line");
  free(js);
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\",\"suggestion\":\"none.\"}");
  expect(js && strstr(js, "\"suggestion\":\"\"") && !strstr(js, "\"suggestion\":\"none\""),
         "a meta line with a period is not stored");
  free(js);
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\","
            "\"suggestion\":\"one two three four five six seven eight nine ten eleven twelve thirteen\"}");
  expect(js && strstr(js, "\"status\":\"empty\"") && strstr(js, "\"suggestion\":\"\""),
         "a thirteen-word line is not stored");
  free(js);
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\",\"suggestion\":\"   \"}");
  expect(js && strstr(js, "prompt a peer to move") && strstr(js, "\"stored\":true"),
         "a blank suggestion still stores the peer line");
  free(js);
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\",\"suggestion\":\"move now\"}");
  expect(js && strstr(js, "\"suggestion\":\"move now\""), "a two-word line stays");
  free(js);
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\","
            "\"suggestion\":\"one two three four five six seven eight nine ten eleven twelve\"}");
  expect(js && strstr(js, "\"suggestion\":\"one two three four five six seven eight nine ten eleven twelve\""),
         "a twelve-word line stays");
  free(js);
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\",\"suggestion\":\"silence please\"}");
  expect(js && strstr(js, "\"suggestion\":\"silence please\""),
         "silence inside a two-word line stays");
  free(js);
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\",\"suggestion\":\"read the law and prove\"}");
  expect(js && strstr(js, "\"suggestion\":\"read the law and prove\""),
         "the law line still stores");
  free(js);
  ng_motive_rank("read the law and prove", &m);
  expect(m.rank == 256 && m.lexical == 3 && m.hamming == 0,
         "a rejected line leaves the law rank");
  ng_motive_rank("you lied", &m);
  expect(m.plate == NG_PLATE_CONFESS && m.lexical == 0, "you lied stays confess");
  ng_motive_rank("", &m);
  expect(m.rank == 0, "an empty query stays rank 0");

  /* An interior blank is a space. A vertical tab does not glue two words. */
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\",\"suggestion\":\"move\\u000bnow\"}");
  expect(js && strstr(js, "\"suggestion\":\"move now\""),
         "a vertical tab inside a two-word line is a space");
  expect(js && strstr(js, "\"status\":\"pending\""),
         "a vertical tab does not hide a two-word line");
  free(js);
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\","
            "\"suggestion\":\"read\\tthe law and prove\"}");
  expect(js && strstr(js, "\"suggestion\":\"read the law and prove\""),
         "a tab inside the line is a space");
  free(js);
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\","
            "\"suggestion\":\"read\\u000cthe law and prove\"}");
  expect(js && strstr(js, "\"suggestion\":\"read the law and prove\""),
         "a form feed inside the line is a space");
  free(js);
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\",\"suggestion\":\"no\\tsuggestion\"}");
  expect(js && strstr(js, "\"status\":\"empty\"") && strstr(js, "\"suggestion\":\"\""),
         "a tab inside a meta line is still that meta line");
  expect(js && !strstr(js, "prompt a peer to move"),
         "a tabbed meta line is not the peer line");
  free(js);
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\",\"suggestion\":\"move\\u000b\\u000bnow\"}");
  expect(js && strstr(js, "\"suggestion\":\"move  now\""),
         "two vertical tabs stay two spaces");
  free(js);
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\","
            "\"suggestion\":\"read  the law and prove\"}");
  expect(js && strstr(js, "\"suggestion\":\"read  the law and prove\""),
         "a doubled space stays doubled");
  free(js);
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\","
            "\"suggestion\":\"read the law and prove\\u2014\"}");
  expect(js && strstr(js, "\"suggestion\":\"read the law and prove\xe2\x80\x94\""),
         "an em dash stays beside an interior blank");
  free(js);
  ng_motive_rank("read the law and prove", &m);
  expect(m.rank == 256 && m.lexical == 3 && m.hamming == 0,
         "an interior blank leaves the law rank");

  /* A meta line closed by a bang or a question is still that meta line. */
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\",\"suggestion\":\"no suggestion!\"}");
  expect(js && strstr(js, "\"status\":\"empty\"") && strstr(js, "\"suggestion\":\"\""),
         "a meta line closed by a bang is not stored");
  expect(js && !strstr(js, "prompt a peer to move"),
         "a banged meta line is not the peer line");
  expect(js && !strstr(js, "no suggestion"), "a banged meta line leaves no words");
  free(js);
  {
    char *pend = ng_augogen_pending_json();
    expect(pend && strstr(pend, "\"suggestion\":\"\"") && strstr(pend, "\"status\":\"empty\""),
           "a banged meta line stays off the plate");
    free(pend);
  }
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\",\"suggestion\":\"no suggestion?\"}");
  expect(js && strstr(js, "\"status\":\"empty\"") && strstr(js, "\"suggestion\":\"\""),
         "a meta line closed by a question is not stored");
  expect(js && !strstr(js, "prompt a peer to move"),
         "a questioned meta line is not the peer line");
  free(js);
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\",\"suggestion\":\"NO SUGGESTION?\"}");
  expect(js && strstr(js, "\"suggestion\":\"\"") && !strstr(js, "NO SUGGESTION"),
         "a closed meta line folds case");
  free(js);
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\",\"suggestion\":\"no suggestion!!\"}");
  expect(js && strstr(js, "\"status\":\"empty\"") && strstr(js, "\"suggestion\":\"\""),
         "a meta line closed by two bangs is not stored");
  free(js);
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\",\"suggestion\":\"move now!\"}");
  expect(js && strstr(js, "\"suggestion\":\"move now!\""),
         "a bang on a real line stays");
  free(js);
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\","
            "\"suggestion\":\"read the law and prove!\"}");
  expect(js && strstr(js, "\"suggestion\":\"read the law and prove!\""),
         "a bang on the law line stays");
  free(js);
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\",\"suggestion\":\"nothing to add!\"}");
  expect(js && strstr(js, "\"suggestion\":\"nothing to add!\""),
         "a bang after more words stays");
  free(js);
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\",\"suggestion\":\"silence? please move\"}");
  expect(js && strstr(js, "\"suggestion\":\"silence? please move\""),
         "a question inside a longer line stays");
  free(js);
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\",\"suggestion\":\"no suggestion !\"}");
  expect(js && strstr(js, "\"suggestion\":\"no suggestion !\""),
         "a space before a bang keeps the line");
  free(js);
  ng_motive_rank("read the law and prove", &m);
  expect(m.rank == 256 && m.lexical == 3 && m.hamming == 0,
         "a closed meta line leaves the law rank");
  ng_motive_rank("you lied", &m);
  expect(m.plate == NG_PLATE_CONFESS && m.lexical == 0,
         "a closed meta line leaves confess");
  ng_motive_rank("", &m);
  expect(m.rank == 0, "a closed meta line leaves an empty query at rank 0");

  /* A second prompt in the same second keeps its own file. The first stays. */
  {
    int tries, got = 0;
    setenv("NANOBOT_AUGOGEN_AUTO", "1", 1);
    for (tries = 0; tries < 4 && !got; tries++) {
      char ida[48], idb[48];
      char *a = ng_augogen_handle(
          NULL, "{\"action\":\"generate\",\"suggestion\":\"read the law and prove\"}");
      char *b = ng_augogen_handle(
          NULL, "{\"action\":\"generate\",\"suggestion\":\"rank concepts then act\"}");
      job_id_of(a, ida, sizeof ida);
      job_id_of(b, idb, sizeof idb);
      if (same_job_slot(ida, idb)) {
        char path[256];
        got = 1;
        expect(strcmp(ida, idb) != 0, "a second job in the same second keeps its own id");
        snprintf(path, sizeof path, "%s/jobs/%s.in", home, ida);
        expect(file_eq(path, "read the law and prove"), "the first prompt stays");
        snprintf(path, sizeof path, "%s/jobs/%s.in", home, idb);
        expect(file_eq(path, "rank concepts then act"), "the second prompt stays");
      }
      free(a);
      free(b);
    }
    expect(got, "two auto jobs share a second");
    setenv("NANOBOT_AUGOGEN_AUTO", "0", 1);
    for (tries = 0; tries < 8; tries++) {
      if (waitpid(-1, NULL, WNOHANG) < 0) break;
    }
    ng_motive_rank("read the law and prove", &m);
    expect(m.rank == 256 && m.lexical == 3 && m.hamming == 0,
           "a second job leaves the law rank");
  }

  /* A done auto job past the keep cap drops. A queued job stays. */
  {
    char path[256];
    int i;
    DIR *d = opendir("/tmp/nb-motive-test/jobs");
    struct dirent *de;
    if (d) {
      while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "agrun-", 6) == 0) {
          snprintf(path, sizeof path, "%s/jobs/%s", home, de->d_name);
          unlink(path);
        }
      }
      closedir(d);
    }
    for (i = 1; i <= 49; i++) {
      FILE *f;
      char id[40];
      snprintf(id, sizeof id, "agrun-%010d-1", i);
      snprintf(path, sizeof path, "%s/jobs/%s.json", home, id);
      f = fopen(path, "wb");
      expect(f != NULL, "seed a done auto job");
      if (f) {
        fprintf(f, "{\"status\":\"done\",\"id\":\"%s\"}\n", id);
        fclose(f);
      }
      snprintf(path, sizeof path, "%s/jobs/%s.in", home, id);
      f = fopen(path, "wb");
      if (f) {
        fputs("old prompt", f);
        fclose(f);
      }
    }
    snprintf(path, sizeof path, "%s/jobs/agrun-0000000000-9.json", home);
    {
      FILE *f = fopen(path, "wb");
      expect(f != NULL, "seed a queued auto job");
      if (f) {
        fputs("{\"status\":\"queued\",\"id\":\"agrun-0000000000-9\"}\n", f);
        fclose(f);
      }
    }
    snprintf(path, sizeof path, "%s/jobs/agrun-0000000000-9.in", home);
    {
      FILE *f = fopen(path, "wb");
      if (f) {
        fputs("stay queued", f);
        fclose(f);
      }
    }
    setenv("NANOBOT_AUGOGEN_AUTO", "1", 1);
    js = ng_augogen_handle(
        NULL, "{\"action\":\"generate\",\"suggestion\":\"read the law and prove\"}");
    free(js);
    setenv("NANOBOT_AUGOGEN_AUTO", "0", 1);
    for (i = 0; i < 8; i++) {
      if (waitpid(-1, NULL, WNOHANG) < 0) break;
    }
    snprintf(path, sizeof path, "%s/jobs/agrun-0000000001-1.in", home);
    expect(access(path, F_OK) != 0, "the oldest done auto job drops");
    snprintf(path, sizeof path, "%s/jobs/agrun-0000000049-1.in", home);
    expect(file_eq(path, "old prompt"), "a newer done auto job stays");
    snprintf(path, sizeof path, "%s/jobs/agrun-0000000000-9.in", home);
    expect(file_eq(path, "stay queued"), "a queued auto job stays");
    ng_motive_rank("read the law and prove", &m);
    expect(m.rank == 256 && m.lexical == 3 && m.hamming == 0,
           "a reaped job leaves the law rank");
  }

  if (g_fail) {
    fprintf(stderr, "%d motive checks failed\n", g_fail);
    return 1;
  }
  printf("motive ok\n");
  return 0;
}
