#include "augogen.h"
#include "motive.h"

#include <nanobot/os.h>

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/resource.h>
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
  unsetenv("NANOBOT_MOTIVE_LEARN");

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
  /* A previous run may leave a directory at the plate path. It is not a plate. */
  unlink("/tmp/nb-motive-test/augogen/pending.json/keep.txt");
  unlink("/tmp/nb-motive-test/augogen/pending.json");
  rmdir("/tmp/nb-motive-test/augogen/pending.json");
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

  /* A unit separator is a space. A backspace stays a backspace. */
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\",\"suggestion\":\"move\\u001fnow\"}");
  expect(js && strstr(js, "\"suggestion\":\"move now\""),
         "a unit separator inside a two-word line is a space");
  expect(js && strstr(js, "\"status\":\"pending\""),
         "a unit separator does not hide a two-word line");
  free(js);
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\",\"suggestion\":\"move\\u001cnow\"}");
  expect(js && strstr(js, "\"suggestion\":\"move now\""),
         "a file separator inside a two-word line is a space");
  free(js);
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\","
            "\"suggestion\":\"read\\u001fthe law and prove\"}");
  expect(js && strstr(js, "\"suggestion\":\"read the law and prove\""),
         "a unit separator inside the law line is a space");
  free(js);
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\",\"suggestion\":\"move\\u001f\\u001fnow\"}");
  expect(js && strstr(js, "\"suggestion\":\"move  now\""),
         "two unit separators stay two spaces");
  free(js);
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\","
            "\"suggestion\":\"\\u001fread the law and prove\\u001f\"}");
  expect(js && strstr(js, "\"suggestion\":\"read the law and prove\""),
         "a unit separator at the edge stays off");
  free(js);
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\",\"suggestion\":\"\\u001f\"}");
  expect(js && strstr(js, "prompt a peer to move") && strstr(js, "\"stored\":true"),
         "a separator with nothing else still stores the peer line");
  free(js);
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\",\"suggestion\":\"no\\u001fsuggestion\"}");
  expect(js && strstr(js, "\"status\":\"empty\"") && strstr(js, "\"suggestion\":\"\""),
         "a unit separator inside a meta line is still that meta line");
  expect(js && !strstr(js, "prompt a peer to move"),
         "a separated meta line is not the peer line");
  free(js);
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\","
            "\"suggestion\":\"read the\\u0008law and prove\"}");
  expect(js && strstr(js, "\"suggestion\":\"read the\\u0008law and prove\""),
         "a backspace stays a backspace");
  free(js);
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\",\"suggestion\":\"move\\u0008now\"}");
  expect(js && strstr(js, "\"status\":\"empty\"") && strstr(js, "\"suggestion\":\"\""),
         "a backspace does not become a word space");
  expect(js && !strstr(js, "prompt a peer to move"),
         "a backspace between two words is not the peer line");
  free(js);
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\",\"suggestion\":\"no suggestion!\"}");
  expect(js && strstr(js, "\"suggestion\":\"no suggestion!\""),
         "a bang on a meta line stays");
  free(js);
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\",\"suggestion\":\"no suggestion?\"}");
  expect(js && strstr(js, "\"suggestion\":\"no suggestion?\""),
         "a question on a meta line stays");
  free(js);
  js = ng_augogen_handle(
      NULL, "{\"action\":\"generate\",\"enqueue\":\"1\","
            "\"suggestion\":\"read the law and prove\\u2014\"}");
  expect(js && strstr(js, "\"suggestion\":\"read the law and prove\xe2\x80\x94\""),
         "an em dash stays beside a unit separator");
  free(js);
  ng_motive_rank("read the law and prove", &m);
  expect(m.rank == 256 && m.lexical == 3 && m.hamming == 0,
         "a unit separator leaves the law rank");
  ng_motive_rank("you lied", &m);
  expect(m.plate == NG_PLATE_CONFESS && m.lexical == 0,
         "a unit separator leaves you lied");
  ng_motive_rank("", &m);
  expect(m.rank == 0, "a unit separator leaves an empty query at rank 0");

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

  /* A failed fork marks the auto job error. A queued job stays queued. */
  {
    char forkhome[] = "/tmp/nb-motive-fork";
    char path[512];
    char plate[1200];
    char id[48];
    pid_t kid;
    int st = 0;
    DIR *d;
    struct dirent *de;
    int njson = 0;
    mkdir(forkhome, 0700);
    snprintf(path, sizeof path, "%s/jobs", forkhome);
    mkdir(path, 0700);
    d = opendir(path);
    if (d) {
      while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "agrun-", 6) == 0) {
          snprintf(path, sizeof path, "%s/jobs/%s", forkhome, de->d_name);
          unlink(path);
        }
      }
      closedir(d);
    }
    unlink("/tmp/nb-motive-fork/plate.txt");
    kid = fork();
    expect(kid >= 0, "fork fail probe starts");
    if (kid == 0) {
      struct rlimit rl;
      char *body;
      FILE *pf;
      if (getrlimit(RLIMIT_NPROC, &rl) != 0) _exit(2);
      rl.rlim_cur = 1;
      if (setrlimit(RLIMIT_NPROC, &rl) != 0) _exit(3);
      setenv("NANOBOT_AUGOGEN_AUTO", "1", 1);
      setenv("NANOBOT_MOTIVE_PEER", "off", 1);
      nb_set_workdir(forkhome);
      body = ng_augogen_handle(
          NULL, "{\"action\":\"generate\",\"suggestion\":\"read the law and prove\"}");
      pf = fopen("/tmp/nb-motive-fork/plate.txt", "wb");
      if (!pf) {
        free(body);
        _exit(4);
      }
      if (body) fputs(body, pf);
      fclose(pf);
      free(body);
      _exit(0);
    }
    if (kid > 0) waitpid(kid, &st, 0);
    expect(kid > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0, "fork fail probe exits");
    {
      FILE *pf = fopen("/tmp/nb-motive-fork/plate.txt", "rb");
      size_t n = 0;
      plate[0] = 0;
      if (pf) {
        n = fread(plate, 1, sizeof plate - 1, pf);
        fclose(pf);
      }
      plate[n] = 0;
    }
    expect(strstr(plate, "\"executed\":false") != NULL, "a failed fork stays unexecuted");
    expect(strstr(plate, "\"status\":\"applied\"") != NULL, "a failed fork still applied");
    job_id_of(plate, id, sizeof id);
    expect(strncmp(id, "agrun-", 6) == 0, "a failed fork names the job");
    snprintf(path, sizeof path, "%s/jobs/%s.json", forkhome, id);
    {
      FILE *f = fopen(path, "rb");
      char buf[600];
      size_t n = 0;
      expect(f != NULL, "a failed fork leaves the job file");
      if (f) {
        n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
      }
      buf[n] = 0;
      expect(strstr(buf, "\"status\":\"error\"") != NULL, "a failed fork marks the job error");
      expect(strstr(buf, "fork_failed") != NULL, "a failed fork names fork_failed");
      expect(strstr(buf, "\"status\":\"queued\"") == NULL, "a failed fork does not stay queued");
    }
    snprintf(path, sizeof path, "%s/jobs/%s.in", forkhome, id);
    expect(file_eq(path, "read the law and prove"), "a failed fork keeps the prompt");
    d = opendir("/tmp/nb-motive-fork/jobs");
    if (d) {
      while ((de = readdir(d)) != NULL) {
        size_t len = strlen(de->d_name);
        if (len > 5 && strcmp(de->d_name + len - 5, ".json") == 0) njson++;
      }
      closedir(d);
    }
    expect(njson == 1, "a failed fork leaves one job file");
    ng_motive_rank("read the law and prove", &m);
    expect(m.rank == 256 && m.lexical == 3 && m.hamming == 0,
           "a failed fork leaves the law rank");
    ng_motive_rank("you lied", &m);
    expect(m.plate == NG_PLATE_CONFESS && m.lexical == 0,
           "a failed fork leaves you lied");
    ng_motive_rank("", &m);
    expect(m.rank == 0, "a failed fork leaves an empty query at rank 0");
    setenv("NANOBOT_AUGOGEN_AUTO", "0", 1);
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

  /* A quote in the plate status or plate name stays escaped. */
  {
    FILE *pf;
    mkdir("/tmp/nb-motive-test/augogen", 0700);
    pf = fopen("/tmp/nb-motive-test/augogen/pending.json", "wb");
    expect(pf != NULL, "plant a plate with quotes");
    if (pf) {
      fputs("{\"suggestion\":\"read the law and prove\","
            "\"status\":\"a\\\"b\",\"guide_vote\":-1,\"oversee_vote\":-1,"
            "\"mesh_vote\":-1,\"rank\":256,\"lexical\":3,\"hamming\":0,"
            "\"plate\":\"c\\\"d\"}",
            pf);
      fclose(pf);
    }
    js = ng_augogen_handle(NULL, "{\"action\":\"pending\"}");
    expect(js && strstr(js, "\"action\":\"a\\\"b\""),
           "a quote in the status stays escaped on the action");
    expect(js && strstr(js, "\"status\":\"a\\\"b\""),
           "a quote in the status stays escaped");
    expect(js && strstr(js, "\"plate\":\"c\\\"d\""),
           "a quote in the plate name stays escaped");
    expect(js && strstr(js, "\"rank\":256"), "a quoted plate keeps the rank");
    expect(js && strstr(js, "\"lexical\":3"), "a quoted plate keeps lexical");
    expect(js && strstr(js, "\"hamming\":0"), "a quoted plate keeps hamming");
    expect(js && strstr(js, "\"cli_compat\":\"1.0.52\""),
           "the cli version stays a plain token");
    expect(js && strstr(js, "\"hold_flash\":1"), "hold flash stays on the plate");
    expect(js && strstr(js, "\"python\":0"), "python stays 0");
    expect(js && count_substr(js, "\"ok\":true") == 1, "one ok field");
    expect(js && count_substr(js, "\"rank\":") == 1, "one rank key");
    expect(js && count_substr(js, "\"plate\":") == 1, "one plate key");
    free(js);
    ng_motive_rank("read the law and prove", &m);
    expect(m.rank == 256 && m.lexical == 3 && m.hamming == 0,
           "a quoted plate leaves the law rank");
    ng_motive_rank("you lied", &m);
    expect(m.plate == NG_PLATE_CONFESS && m.lexical == 0,
           "a quoted plate leaves you lied");
    ng_motive_rank("", &m);
    expect(m.rank == 0, "a quoted plate leaves an empty query at rank 0");
  }

  /* A symlink at the plate path is not the plate. The target stays. */
  {
    const char *target = "/tmp/nb-motive-nofollow-target";
    const char *platep = "/tmp/nb-motive-test/augogen/pending.json";
    const char *marked = "{\"suggestion\":\"stolen words stay\",\"status\":\"pending\"}";
    struct stat st;
    FILE *tf;
    char buf[512];
    size_t n;
    char *pend;
    unlink(target);
    tf = fopen(target, "wb");
    expect(tf != NULL, "plant the symlink target");
    if (tf) {
      fputs(marked, tf);
      fclose(tf);
    }
    mkdir("/tmp/nb-motive-test/augogen", 0700);
    unlink(platep);
    expect(symlink(target, platep) == 0, "plate path is a symlink");
    setenv("NANOBOT_AUGOGEN_AUTO", "0", 1);
    js = ng_augogen_handle(NULL, "{\"action\":\"pending\"}");
    expect(js && !strstr(js, "stolen words stay"),
           "a symlink plate is not read");
    expect(js && strstr(js, "\"status\":\"empty\""),
           "a symlink plate stays empty");
    expect(js && strstr(js, "\"hold_flash\":1"), "a symlink plate keeps hold flash");
    free(js);
    tf = fopen(target, "rb");
    n = 0;
    buf[0] = 0;
    if (tf) {
      n = fread(buf, 1, sizeof buf - 1, tf);
      fclose(tf);
    }
    buf[n] = 0;
    expect(strcmp(buf, marked) == 0, "a pending read leaves the symlink target");
    js = ng_augogen_handle(
        NULL, "{\"action\":\"generate\",\"enqueue\":\"1\",\"suggestion\":\"move now\"}");
    expect(js && strstr(js, "\"suggestion\":\"move now\""),
           "a generate still stores on a replaced link");
    expect(js && strstr(js, "\"status\":\"pending\""),
           "a replaced link stays a pending plate");
    free(js);
    tf = fopen(target, "rb");
    n = 0;
    buf[0] = 0;
    if (tf) {
      n = fread(buf, 1, sizeof buf - 1, tf);
      fclose(tf);
    }
    buf[n] = 0;
    expect(strcmp(buf, marked) == 0, "the write leaves the symlink target");
    expect(lstat(platep, &st) == 0 && S_ISREG(st.st_mode),
           "the plate path is a regular file after the write");
    expect(readlink(platep, buf, sizeof buf) < 0, "the plate path is not a symlink");
    tf = fopen(platep, "rb");
    n = 0;
    buf[0] = 0;
    if (tf) {
      n = fread(buf, 1, sizeof buf - 1, tf);
      fclose(tf);
    }
    buf[n] = 0;
    expect(strstr(buf, "\"suggestion\":\"move now\"") != NULL,
           "the regular plate keeps the stored line");
    pend = ng_augogen_pending_json();
    expect(pend && strstr(pend, "\"suggestion\":\"move now\""),
           "a regular plate still loads");
    expect(pend && !strstr(pend, "stolen words stay"),
           "a regular plate does not name the old target");
    free(pend);
    unlink(target);
    ng_motive_rank("read the law and prove", &m);
    expect(m.rank == 256 && m.lexical == 3 && m.hamming == 0,
           "a symlink plate leaves the law rank");
    ng_motive_rank("you lied", &m);
    expect(m.plate == NG_PLATE_CONFESS && m.lexical == 0,
           "a symlink plate leaves you lied");
    ng_motive_rank("", &m);
    expect(m.rank == 0, "a symlink plate leaves an empty query at rank 0");
  }

  /* A symlink named augogen is not the plate directory. The target stays. */
  {
    const char *tdir = "/tmp/nb-motive-dirlink-target";
    const char *tplate = "/tmp/nb-motive-dirlink-target/pending.json";
    const char *aug = "/tmp/nb-motive-test/augogen";
    const char *marked = "{\"suggestion\":\"stolen directory stays\",\"status\":\"pending\"}";
    struct stat st;
    DIR *ad;
    struct dirent *de;
    FILE *tf;
    char path[256];
    char buf[512];
    size_t n;
    char *pend;
    ad = opendir(aug);
    if (ad) {
      while ((de = readdir(ad)) != NULL) {
        if (de->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s", aug, de->d_name);
        unlink(path);
      }
      closedir(ad);
    }
    rmdir(aug);
    mkdir(tdir, 0700);
    unlink(tplate);
    tf = fopen(tplate, "wb");
    expect(tf != NULL, "plant the directory target");
    if (tf) {
      fputs(marked, tf);
      fclose(tf);
    }
    expect(symlink(tdir, aug) == 0, "augogen is a symlink");
    setenv("NANOBOT_AUGOGEN_AUTO", "0", 1);
    js = ng_augogen_handle(NULL, "{\"action\":\"pending\"}");
    expect(js && !strstr(js, "stolen directory stays"),
           "a symlink directory is not read");
    expect(js && strstr(js, "\"status\":\"empty\""),
           "a symlink directory stays empty");
    expect(js && strstr(js, "\"hold_flash\":1"),
           "a symlink directory keeps hold flash");
    free(js);
    expect(lstat(aug, &st) == 0 && S_ISLNK(st.st_mode),
           "a pending read leaves the directory symlink");
    tf = fopen(tplate, "rb");
    n = 0;
    buf[0] = 0;
    if (tf) {
      n = fread(buf, 1, sizeof buf - 1, tf);
      fclose(tf);
    }
    buf[n] = 0;
    expect(strcmp(buf, marked) == 0, "a pending read leaves the directory target");
    js = ng_augogen_handle(
        NULL, "{\"action\":\"generate\",\"enqueue\":\"1\",\"suggestion\":\"move next\"}");
    expect(js && strstr(js, "\"suggestion\":\"move next\""),
           "a generate stores on a replaced directory link");
    expect(js && strstr(js, "\"status\":\"pending\""),
           "a replaced directory link stays a pending plate");
    free(js);
    tf = fopen(tplate, "rb");
    n = 0;
    buf[0] = 0;
    if (tf) {
      n = fread(buf, 1, sizeof buf - 1, tf);
      fclose(tf);
    }
    buf[n] = 0;
    expect(strcmp(buf, marked) == 0, "the write leaves the directory target");
    expect(lstat(aug, &st) == 0 && S_ISDIR(st.st_mode),
           "augogen is a directory after the write");
    expect(readlink(aug, buf, sizeof buf) < 0, "augogen is not a symlink");
    snprintf(path, sizeof path, "%s/pending.json", aug);
    tf = fopen(path, "rb");
    n = 0;
    buf[0] = 0;
    if (tf) {
      n = fread(buf, 1, sizeof buf - 1, tf);
      fclose(tf);
    }
    buf[n] = 0;
    expect(strstr(buf, "\"suggestion\":\"move next\"") != NULL,
           "the real directory keeps the stored line");
    expect(strstr(buf, "stolen directory stays") == NULL,
           "the real directory does not name the target");
    pend = ng_augogen_pending_json();
    expect(pend && strstr(pend, "\"suggestion\":\"move next\""),
           "a real directory still loads");
    expect(pend && !strstr(pend, "stolen directory stays"),
           "a real directory does not load the target");
    free(pend);
    ng_motive_rank("read the law and prove", &m);
    expect(m.rank == 256 && m.lexical == 3 && m.hamming == 0,
           "a symlink directory leaves the law rank");
    ng_motive_rank("you lied", &m);
    expect(m.plate == NG_PLATE_CONFESS && m.lexical == 0,
           "a symlink directory leaves you lied");
    ng_motive_rank("", &m);
    expect(m.rank == 0, "a symlink directory leaves an empty query at rank 0");
  }

  /* A directory at the plate path is not the plate. */
  {
    const char *platep = "/tmp/nb-motive-test/augogen/pending.json";
    const char *kept = "/tmp/nb-motive-test/augogen/pending.json/keep.txt";
    const char *marked = "inner name stays";
    struct stat st;
    FILE *tf;
    char buf[512];
    size_t n;
    char *pend;
    unlink(platep);
    rmdir(platep);
    expect(mkdir(platep, 0700) == 0, "plant an empty plate directory");
    setenv("NANOBOT_AUGOGEN_AUTO", "0", 1);
    js = ng_augogen_handle(NULL, "{\"action\":\"pending\"}");
    expect(js && strstr(js, "\"status\":\"empty\""),
           "an empty plate directory stays unread");
    expect(js && strstr(js, "\"hold_flash\":1"),
           "an empty plate directory keeps hold flash");
    free(js);
    expect(lstat(platep, &st) == 0 && S_ISDIR(st.st_mode),
           "a pending read leaves the empty plate directory");
    js = ng_augogen_handle(
        NULL, "{\"action\":\"generate\",\"enqueue\":\"1\",\"suggestion\":\"move clear\"}");
    expect(js && strstr(js, "\"suggestion\":\"move clear\""),
           "a generate stores over an empty plate directory");
    expect(js && strstr(js, "\"status\":\"pending\""),
           "an empty plate directory becomes a pending plate");
    expect(js && strstr(js, "\"hold_flash\":1"),
           "a replaced plate directory keeps hold flash");
    free(js);
    expect(lstat(platep, &st) == 0 && S_ISREG(st.st_mode),
           "the plate path is a regular file after the empty directory");
    expect(readlink(platep, buf, sizeof buf) < 0, "the plate path is not a link");
    tf = fopen(platep, "rb");
    n = 0;
    buf[0] = 0;
    if (tf) {
      n = fread(buf, 1, sizeof buf - 1, tf);
      fclose(tf);
    }
    buf[n] = 0;
    expect(strstr(buf, "\"suggestion\":\"move clear\"") != NULL,
           "the regular plate keeps move clear");
    pend = ng_augogen_pending_json();
    expect(pend && strstr(pend, "\"suggestion\":\"move clear\""),
           "a replaced empty directory still loads");
    free(pend);
    unlink(platep);
    expect(mkdir(platep, 0700) == 0, "plant a plate directory that holds a name");
    tf = fopen(kept, "wb");
    expect(tf != NULL, "plant the inner name");
    if (tf) {
      fputs(marked, tf);
      fclose(tf);
    }
    js = ng_augogen_handle(NULL, "{\"action\":\"pending\"}");
    expect(js && !strstr(js, "inner name stays"),
           "a plate directory is not read");
    expect(js && strstr(js, "\"status\":\"empty\""),
           "a plate directory stays empty");
    expect(js && strstr(js, "\"hold_flash\":1"),
           "a plate directory keeps hold flash");
    free(js);
    expect(lstat(platep, &st) == 0 && S_ISDIR(st.st_mode),
           "a pending read leaves the plate directory");
    js = ng_augogen_handle(
        NULL, "{\"action\":\"generate\",\"enqueue\":\"1\",\"suggestion\":\"move past\"}");
    free(js);
    expect(lstat(platep, &st) == 0 && S_ISDIR(st.st_mode),
           "a plate directory that holds a name stays");
    tf = fopen(kept, "rb");
    n = 0;
    buf[0] = 0;
    if (tf) {
      n = fread(buf, 1, sizeof buf - 1, tf);
      fclose(tf);
    }
    buf[n] = 0;
    expect(strcmp(buf, marked) == 0, "the write does not enter the plate directory");
    pend = ng_augogen_pending_json();
    expect(pend && !strstr(pend, "move past"),
           "a plate directory does not load the unsaved line");
    expect(pend && strstr(pend, "\"status\":\"empty\""),
           "a plate directory still reads empty");
    free(pend);
    ng_motive_rank("read the law and prove", &m);
    expect(m.rank == 256 && m.lexical == 3 && m.hamming == 0,
           "a plate directory leaves the law rank");
    ng_motive_rank("you lied", &m);
    expect(m.plate == NG_PLATE_CONFESS && m.lexical == 0,
           "a plate directory leaves you lied");
    ng_motive_rank("", &m);
    expect(m.rank == 0, "a plate directory leaves an empty query at rank 0");
  }

  /* A symlink named guide.bin or oversee.bin is not the cube. */
  {
    const char *platep = "/tmp/nb-motive-test/augogen/pending.json";
    const char *kept = "/tmp/nb-motive-test/augogen/pending.json/keep.txt";
    const char *gbin = "/tmp/nb-motive-test/augogen/guide.bin";
    const char *obin = "/tmp/nb-motive-test/augogen/oversee.bin";
    const char *gtarget = "/tmp/nb-motive-cube-guide";
    const char *otarget = "/tmp/nb-motive-cube-oversee";
    const char *gmark = "guide target stays";
    const char *omark = "oversee target stays";
    struct stat st;
    struct stat before_g, before_o, after_g, after_o;
    FILE *tf;
    char buf[512];
    size_t n;
    char *pend;
    unlink(kept);
    unlink(platep);
    rmdir(platep);
    unlink(gbin);
    unlink(obin);
    unlink(gtarget);
    unlink(otarget);
    tf = fopen(platep, "wb");
    expect(tf != NULL, "plant a plate before the cube links");
    if (tf) {
      fputs("{\"suggestion\":\"move now\",\"status\":\"pending\","
            "\"guide_vote\":-1,\"oversee_vote\":-1,\"mesh_vote\":-1}",
            tf);
      fclose(tf);
    }
    tf = fopen(gtarget, "wb");
    expect(tf != NULL, "plant the guide cube target");
    if (tf) {
      fputs(gmark, tf);
      fclose(tf);
    }
    tf = fopen(otarget, "wb");
    expect(tf != NULL, "plant the oversee cube target");
    if (tf) {
      fputs(omark, tf);
      fclose(tf);
    }
    expect(symlink(gtarget, gbin) == 0, "guide.bin is a symlink");
    expect(symlink(otarget, obin) == 0, "oversee.bin is a symlink");
    expect(stat(gtarget, &before_g) == 0, "stat the guide target");
    expect(stat(otarget, &before_o) == 0, "stat the oversee target");
    setenv("NANOBOT_AUGOGEN_AUTO", "0", 1);
    js = ng_augogen_handle(NULL, "{\"action\":\"pending\"}");
    expect(js && strstr(js, "\"suggestion\":\"move now\""),
           "a pending read still sees the planted plate");
    expect(js && !strstr(js, "guide target stays"),
           "a pending read does not name the guide target");
    expect(js && !strstr(js, "oversee target stays"),
           "a pending read does not name the oversee target");
    expect(js && strstr(js, "\"hold_flash\":1"),
           "a cube symlink keeps hold flash");
    free(js);
    expect(lstat(gbin, &st) == 0 && S_ISLNK(st.st_mode),
           "a pending read leaves the guide symlink");
    expect(lstat(obin, &st) == 0 && S_ISLNK(st.st_mode),
           "a pending read leaves the oversee symlink");
    tf = fopen(gtarget, "rb");
    n = 0;
    buf[0] = 0;
    if (tf) {
      n = fread(buf, 1, sizeof buf - 1, tf);
      fclose(tf);
    }
    buf[n] = 0;
    expect(strcmp(buf, gmark) == 0, "a pending read leaves the guide target");
    tf = fopen(otarget, "rb");
    n = 0;
    buf[0] = 0;
    if (tf) {
      n = fread(buf, 1, sizeof buf - 1, tf);
      fclose(tf);
    }
    buf[n] = 0;
    expect(strcmp(buf, omark) == 0, "a pending read leaves the oversee target");
    js = ng_augogen_handle(
        NULL, "{\"action\":\"vote\",\"role\":\"mesh\",\"vote\":\"1\"}");
    expect(js && strstr(js, "\"suggestion\":\"move now\""),
           "a vote still names the planted line");
    expect(js && strstr(js, "\"hold_flash\":1"),
           "a cube vote keeps hold flash");
    expect(js && !strstr(js, "guide target stays"),
           "a vote does not name the guide target");
    expect(js && !strstr(js, "oversee target stays"),
           "a vote does not name the oversee target");
    free(js);
    tf = fopen(gtarget, "rb");
    n = 0;
    buf[0] = 0;
    if (tf) {
      n = fread(buf, 1, sizeof buf - 1, tf);
      fclose(tf);
    }
    buf[n] = 0;
    expect(strcmp(buf, gmark) == 0, "the write leaves the guide target");
    tf = fopen(otarget, "rb");
    n = 0;
    buf[0] = 0;
    if (tf) {
      n = fread(buf, 1, sizeof buf - 1, tf);
      fclose(tf);
    }
    buf[n] = 0;
    expect(strcmp(buf, omark) == 0, "the write leaves the oversee target");
    expect(stat(gtarget, &after_g) == 0 && after_g.st_ino == before_g.st_ino,
           "the guide target inode stays");
    expect(stat(otarget, &after_o) == 0 && after_o.st_ino == before_o.st_ino,
           "the oversee target inode stays");
#if NANOBOT_HAS_BRAINCUBE
    expect(lstat(gbin, &st) == 0 && S_ISREG(st.st_mode),
           "guide.bin is a regular file after the vote");
    expect(lstat(obin, &st) == 0 && S_ISREG(st.st_mode),
           "oversee.bin is a regular file after the vote");
    expect(readlink(gbin, buf, sizeof buf) < 0, "guide.bin is not a symlink");
    expect(readlink(obin, buf, sizeof buf) < 0, "oversee.bin is not a symlink");
#else
    expect(lstat(gbin, &st) == 0 && S_ISLNK(st.st_mode),
           "without a cube the guide symlink stays");
    expect(lstat(obin, &st) == 0 && S_ISLNK(st.st_mode),
           "without a cube the oversee symlink stays");
#endif
    pend = ng_augogen_pending_json();
    expect(pend && strstr(pend, "\"suggestion\":\"move now\""),
           "the plate still loads after the cube vote");
    expect(pend && !strstr(pend, "guide target stays"),
           "the plate does not name the guide target");
    free(pend);
    ng_motive_rank("read the law and prove", &m);
    expect(m.rank == 256 && m.lexical == 3 && m.hamming == 0,
           "a cube symlink leaves the law rank");
    ng_motive_rank("you lied", &m);
    expect(m.plate == NG_PLATE_CONFESS && m.lexical == 0,
           "a cube symlink leaves you lied");
    ng_motive_rank("", &m);
    expect(m.rank == 0, "a cube symlink leaves an empty query at rank 0");
  }

  /* A directory named guide.bin or oversee.bin is not the cube. */
  {
    const char *platep = "/tmp/nb-motive-test/augogen/pending.json";
    const char *gbin = "/tmp/nb-motive-test/augogen/guide.bin";
    const char *obin = "/tmp/nb-motive-test/augogen/oversee.bin";
    const char *gmarkp = "/tmp/nb-motive-test/augogen/guide.bin/mark.txt";
    const char *omarkp = "/tmp/nb-motive-test/augogen/oversee.bin/mark.txt";
    const char *marked = "cube mark stays";
    struct stat st;
    FILE *tf;
    char buf[512];
    size_t n;
    char *pend;
    unlink(gmarkp);
    unlink(omarkp);
    unlink(gbin);
    unlink(obin);
    rmdir(gbin);
    rmdir(obin);
    unlink(platep);
    rmdir(platep);
    tf = fopen(platep, "wb");
    expect(tf != NULL, "plant a plate before the cube directories");
    if (tf) {
      fputs("{\"suggestion\":\"move now\",\"status\":\"pending\","
            "\"guide_vote\":-1,\"oversee_vote\":-1,\"mesh_vote\":-1}",
            tf);
      fclose(tf);
    }
    expect(mkdir(gbin, 0700) == 0, "plant an empty guide cube directory");
    expect(mkdir(obin, 0700) == 0, "plant an empty oversee cube directory");
    setenv("NANOBOT_AUGOGEN_AUTO", "0", 1);
    js = ng_augogen_handle(NULL, "{\"action\":\"pending\"}");
    expect(js && strstr(js, "\"suggestion\":\"move now\""),
           "a pending read still sees the plate beside empty cube directories");
    expect(js && strstr(js, "\"hold_flash\":1"),
           "empty cube directories keep hold flash");
    free(js);
    expect(lstat(gbin, &st) == 0 && S_ISDIR(st.st_mode),
           "a pending read leaves the empty guide directory");
    expect(lstat(obin, &st) == 0 && S_ISDIR(st.st_mode),
           "a pending read leaves the empty oversee directory");
    js = ng_augogen_handle(
        NULL, "{\"action\":\"vote\",\"role\":\"mesh\",\"vote\":\"1\"}");
    expect(js && strstr(js, "\"suggestion\":\"move now\""),
           "a vote still names the planted line beside empty cube directories");
    expect(js && strstr(js, "\"hold_flash\":1"),
           "a vote over empty cube directories keeps hold flash");
    free(js);
#if NANOBOT_HAS_BRAINCUBE
    expect(lstat(gbin, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 8,
           "an empty guide directory becomes a cube file");
    expect(lstat(obin, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 8,
           "an empty oversee directory becomes a cube file");
    expect(readlink(gbin, buf, sizeof buf) < 0, "the guide cube file is not a link");
    expect(readlink(obin, buf, sizeof buf) < 0, "the oversee cube file is not a link");
#else
    expect(lstat(gbin, &st) == 0 && S_ISDIR(st.st_mode),
           "without a cube the empty guide directory stays");
    expect(lstat(obin, &st) == 0 && S_ISDIR(st.st_mode),
           "without a cube the empty oversee directory stays");
#endif
    pend = ng_augogen_pending_json();
    expect(pend && strstr(pend, "\"suggestion\":\"move now\""),
           "the plate still loads after the empty cube directories");
    free(pend);
    unlink(gbin);
    unlink(obin);
    rmdir(gbin);
    rmdir(obin);
    expect(mkdir(gbin, 0700) == 0, "plant a guide directory that holds a name");
    expect(mkdir(obin, 0700) == 0, "plant an oversee directory that holds a name");
    tf = fopen(gmarkp, "wb");
    expect(tf != NULL, "plant the guide inner name");
    if (tf) {
      fputs(marked, tf);
      fclose(tf);
    }
    tf = fopen(omarkp, "wb");
    expect(tf != NULL, "plant the oversee inner name");
    if (tf) {
      fputs(marked, tf);
      fclose(tf);
    }
    js = ng_augogen_handle(NULL, "{\"action\":\"pending\"}");
    expect(js && !strstr(js, "cube mark stays"),
           "a cube directory is not read");
    expect(js && strstr(js, "\"suggestion\":\"move now\""),
           "a cube directory leaves the planted plate");
    expect(js && strstr(js, "\"hold_flash\":1"),
           "a cube directory keeps hold flash");
    free(js);
    expect(lstat(gbin, &st) == 0 && S_ISDIR(st.st_mode),
           "a pending read leaves the guide directory");
    expect(lstat(obin, &st) == 0 && S_ISDIR(st.st_mode),
           "a pending read leaves the oversee directory");
    js = ng_augogen_handle(
        NULL, "{\"action\":\"vote\",\"role\":\"mesh\",\"vote\":\"1\"}");
    free(js);
    expect(lstat(gbin, &st) == 0 && S_ISDIR(st.st_mode),
           "a guide directory that holds a name stays");
    expect(lstat(obin, &st) == 0 && S_ISDIR(st.st_mode),
           "an oversee directory that holds a name stays");
    tf = fopen(gmarkp, "rb");
    n = 0;
    buf[0] = 0;
    if (tf) {
      n = fread(buf, 1, sizeof buf - 1, tf);
      fclose(tf);
    }
    buf[n] = 0;
    expect(strcmp(buf, marked) == 0, "the write does not enter the guide directory");
    tf = fopen(omarkp, "rb");
    n = 0;
    buf[0] = 0;
    if (tf) {
      n = fread(buf, 1, sizeof buf - 1, tf);
      fclose(tf);
    }
    buf[n] = 0;
    expect(strcmp(buf, marked) == 0, "the write does not enter the oversee directory");
    pend = ng_augogen_pending_json();
    expect(pend && strstr(pend, "\"suggestion\":\"move now\""),
           "a cube directory does not replace the plate");
    expect(pend && !strstr(pend, "cube mark stays"),
           "a cube directory does not load the inner name");
    free(pend);
    ng_motive_rank("read the law and prove", &m);
    expect(m.rank == 256 && m.lexical == 3 && m.hamming == 0,
           "a cube directory leaves the law rank");
    ng_motive_rank("you lied", &m);
    expect(m.plate == NG_PLATE_CONFESS && m.lexical == 0,
           "a cube directory leaves you lied");
    ng_motive_rank("", &m);
    expect(m.rank == 0, "a cube directory leaves an empty query at rank 0");
    unlink(gmarkp);
    unlink(omarkp);
    rmdir(gbin);
    rmdir(obin);
  }

  /* A regular file named augogen is not the plate directory. */
  {
    const char *aug = "/tmp/nb-motive-test/augogen";
    const char *platep = "/tmp/nb-motive-test/augogen/pending.json";
    const char *marked = "augogen file stays";
    struct stat st;
    DIR *ad;
    struct dirent *de;
    FILE *tf;
    int efd;
    char path[256];
    char buf[512];
    size_t n;
    char *pend;
    ad = opendir(aug);
    if (ad) {
      while ((de = readdir(ad)) != NULL) {
        if (de->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s", aug, de->d_name);
        unlink(path);
        rmdir(path);
      }
      closedir(ad);
      rmdir(aug);
    }
    unlink(aug);
    efd = open(aug, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    expect(efd >= 0, "plant an empty augogen file");
    if (efd >= 0) close(efd);
    expect(lstat(aug, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 0,
           "the empty augogen path is a regular file");
    setenv("NANOBOT_AUGOGEN_AUTO", "0", 1);
    js = ng_augogen_handle(NULL, "{\"action\":\"pending\"}");
    expect(js && strstr(js, "\"status\":\"empty\""),
           "an empty augogen file stays unread");
    expect(js && strstr(js, "\"hold_flash\":1"),
           "an empty augogen file keeps hold flash");
    free(js);
    expect(lstat(aug, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 0,
           "a pending read leaves the empty augogen file");
    js = ng_augogen_handle(
        NULL, "{\"action\":\"generate\",\"enqueue\":\"1\",\"suggestion\":\"move aside\"}");
    expect(js && strstr(js, "\"suggestion\":\"move aside\""),
           "a generate stores over an empty augogen file");
    expect(js && strstr(js, "\"status\":\"pending\""),
           "an empty augogen file becomes a pending plate");
    expect(js && strstr(js, "\"hold_flash\":1"),
           "a replaced augogen file keeps hold flash");
    free(js);
    expect(lstat(aug, &st) == 0 && S_ISDIR(st.st_mode),
           "augogen is a directory after the empty file");
    expect(readlink(aug, buf, sizeof buf) < 0, "augogen is not a symlink");
    expect(lstat(platep, &st) == 0 && S_ISREG(st.st_mode),
           "the plate path is a regular file after the empty augogen file");
    tf = fopen(platep, "rb");
    n = 0;
    buf[0] = 0;
    if (tf) {
      n = fread(buf, 1, sizeof buf - 1, tf);
      fclose(tf);
    }
    buf[n] = 0;
    expect(strstr(buf, "\"suggestion\":\"move aside\"") != NULL,
           "the real directory keeps move aside");
    pend = ng_augogen_pending_json();
    expect(pend && strstr(pend, "\"suggestion\":\"move aside\""),
           "a replaced empty augogen file still loads");
    free(pend);
    unlink(platep);
    rmdir(aug);
    unlink(aug);
    tf = fopen(aug, "wb");
    expect(tf != NULL, "plant an augogen file that holds bytes");
    if (tf) {
      fputs(marked, tf);
      fclose(tf);
    }
    expect(lstat(aug, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0,
           "the augogen path holds bytes");
    js = ng_augogen_handle(NULL, "{\"action\":\"pending\"}");
    expect(js && !strstr(js, "augogen file stays"),
           "an augogen file is not read");
    expect(js && strstr(js, "\"status\":\"empty\""),
           "an augogen file stays empty");
    expect(js && strstr(js, "\"hold_flash\":1"),
           "an augogen file keeps hold flash");
    free(js);
    expect(lstat(aug, &st) == 0 && S_ISREG(st.st_mode),
           "a pending read leaves the augogen file");
    js = ng_augogen_handle(
        NULL, "{\"action\":\"generate\",\"enqueue\":\"1\",\"suggestion\":\"move kept\"}");
    free(js);
    expect(lstat(aug, &st) == 0 && S_ISREG(st.st_mode),
           "an augogen file that holds bytes stays");
    tf = fopen(aug, "rb");
    n = 0;
    buf[0] = 0;
    if (tf) {
      n = fread(buf, 1, sizeof buf - 1, tf);
      fclose(tf);
    }
    buf[n] = 0;
    expect(strcmp(buf, marked) == 0, "the write does not replace the augogen file");
    pend = ng_augogen_pending_json();
    expect(pend && !strstr(pend, "move kept"),
           "an augogen file does not load the unsaved line");
    expect(pend && strstr(pend, "\"status\":\"empty\""),
           "an augogen file still reads empty");
    free(pend);
    ng_motive_rank("read the law and prove", &m);
    expect(m.rank == 256 && m.lexical == 3 && m.hamming == 0,
           "an augogen file leaves the law rank");
    ng_motive_rank("you lied", &m);
    expect(m.plate == NG_PLATE_CONFESS && m.lexical == 0,
           "an augogen file leaves you lied");
    ng_motive_rank("", &m);
    expect(m.rank == 0, "an augogen file leaves an empty query at rank 0");
    unlink(aug);
    mkdir(aug, 0700);
  }

  /* A fifo named augogen is not the plate directory. Do not open it. */
  {
    const char *aug = "/tmp/nb-motive-test/augogen";
    const char *platep = "/tmp/nb-motive-test/augogen/pending.json";
    struct stat st;
    DIR *ad;
    struct dirent *de;
    FILE *tf;
    int kick[2];
    int wst = 0;
    pid_t writer = -1;
    pid_t got;
    char path[256];
    char buf[512];
    char wchan[64];
    size_t n;
    char *pend;
    int spins;
    ad = opendir(aug);
    if (ad) {
      while ((de = readdir(ad)) != NULL) {
        if (de->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s", aug, de->d_name);
        unlink(path);
        rmdir(path);
      }
      closedir(ad);
      rmdir(aug);
    }
    unlink(aug);
    expect(mkfifo(aug, 0600) == 0, "plant an augogen fifo");
    expect(lstat(aug, &st) == 0 && S_ISFIFO(st.st_mode),
           "the augogen path is a fifo");
    kick[0] = -1;
    kick[1] = -1;
    expect(pipe(kick) == 0, "a pipe marks the fifo writer");
    if (kick[0] >= 0) {
      writer = fork();
      if (writer == 0) {
        char one = 1;
        int wfd;
        close(kick[0]);
        if (write(kick[1], &one, 1) != 1) _exit(2);
        close(kick[1]);
        wfd = open(aug, O_WRONLY);
        if (wfd >= 0) close(wfd);
        _exit(0);
      }
      close(kick[1]);
      kick[1] = -1;
      buf[0] = 0;
      if (writer > 0 && read(kick[0], buf, 1) == 1) {
        snprintf(path, sizeof path, "/proc/%d/wchan", (int)writer);
        for (spins = 0; spins < 50; spins++) {
          tf = fopen(path, "r");
          wchan[0] = 0;
          if (tf) {
            if (fgets(wchan, sizeof wchan, tf) == NULL) wchan[0] = 0;
            fclose(tf);
          }
          if (strstr(wchan, "fifo") || strstr(wchan, "pipe")) break;
          poll(NULL, 0, 20);
        }
      }
      close(kick[0]);
      kick[0] = -1;
    }
    setenv("NANOBOT_AUGOGEN_AUTO", "0", 1);
    js = ng_augogen_handle(NULL, "{\"action\":\"pending\"}");
    expect(js && strstr(js, "\"status\":\"empty\""),
           "an augogen fifo stays unread");
    expect(js && strstr(js, "\"hold_flash\":1"),
           "an augogen fifo keeps hold flash");
    free(js);
    expect(lstat(aug, &st) == 0 && S_ISFIFO(st.st_mode),
           "a pending read leaves the augogen fifo");
    if (writer > 0) {
      got = waitpid(writer, &wst, WNOHANG);
      expect(got == 0, "a pending read does not pair a writer on the augogen fifo");
      if (got > 0) writer = -1;
    }
    js = ng_augogen_handle(
        NULL, "{\"action\":\"generate\",\"enqueue\":\"1\",\"suggestion\":\"move clear\"}");
    expect(js && strstr(js, "\"suggestion\":\"move clear\""),
           "a generate stores over an augogen fifo");
    expect(js && strstr(js, "\"status\":\"pending\""),
           "an augogen fifo becomes a pending plate");
    expect(js && strstr(js, "\"hold_flash\":1"),
           "a replaced augogen fifo keeps hold flash");
    free(js);
    expect(lstat(aug, &st) == 0 && S_ISDIR(st.st_mode),
           "augogen is a directory after the fifo");
    expect(!S_ISFIFO(st.st_mode), "the augogen path is not a fifo after the write");
    expect(readlink(aug, buf, sizeof buf) < 0, "augogen is not a symlink");
    expect(lstat(platep, &st) == 0 && S_ISREG(st.st_mode),
           "the plate path is a regular file after the augogen fifo");
    if (writer > 0) {
      got = waitpid(writer, &wst, WNOHANG);
      expect(got == 0, "a generate does not pair a writer on the augogen fifo");
      if (got == 0) {
        kill(writer, SIGTERM);
        waitpid(writer, &wst, 0);
      }
      writer = -1;
    }
    tf = fopen(platep, "rb");
    n = 0;
    buf[0] = 0;
    if (tf) {
      n = fread(buf, 1, sizeof buf - 1, tf);
      fclose(tf);
    }
    buf[n] = 0;
    expect(strstr(buf, "\"suggestion\":\"move clear\"") != NULL,
           "the real directory keeps move clear");
    pend = ng_augogen_pending_json();
    expect(pend && strstr(pend, "\"suggestion\":\"move clear\""),
           "a replaced augogen fifo still loads");
    free(pend);
    ng_motive_rank("read the law and prove", &m);
    expect(m.rank == 256 && m.lexical == 3 && m.hamming == 0,
           "an augogen fifo leaves the law rank");
    ng_motive_rank("you lied", &m);
    expect(m.plate == NG_PLATE_CONFESS && m.lexical == 0,
           "an augogen fifo leaves you lied");
    ng_motive_rank("", &m);
    expect(m.rank == 0, "an augogen fifo leaves an empty query at rank 0");
    unlink(platep);
    rmdir(aug);
    unlink(aug);
    mkdir(aug, 0700);
  }

  /* A fifo at the plate path is not the plate. Do not open it. */
  {
    const char *aug = "/tmp/nb-motive-test/augogen";
    const char *platep = "/tmp/nb-motive-test/augogen/pending.json";
    struct stat st;
    DIR *ad;
    struct dirent *de;
    FILE *tf;
    int kick[2];
    int wst = 0;
    pid_t writer = -1;
    pid_t got;
    char path[256];
    char buf[512];
    char wchan[64];
    size_t n;
    char *pend;
    int spins;
    ad = opendir(aug);
    if (ad) {
      while ((de = readdir(ad)) != NULL) {
        if (de->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s", aug, de->d_name);
        unlink(path);
        rmdir(path);
      }
      closedir(ad);
    }
    mkdir(aug, 0700);
    unlink(platep);
    expect(mkfifo(platep, 0600) == 0, "plant a plate fifo");
    expect(lstat(platep, &st) == 0 && S_ISFIFO(st.st_mode),
           "the plate path is a fifo");
    kick[0] = -1;
    kick[1] = -1;
    expect(pipe(kick) == 0, "a pipe marks the plate fifo writer");
    if (kick[0] >= 0) {
      writer = fork();
      if (writer == 0) {
        char one = 1;
        int wfd;
        close(kick[0]);
        if (write(kick[1], &one, 1) != 1) _exit(2);
        close(kick[1]);
        wfd = open(platep, O_WRONLY);
        if (wfd >= 0) close(wfd);
        _exit(0);
      }
      close(kick[1]);
      kick[1] = -1;
      buf[0] = 0;
      if (writer > 0 && read(kick[0], buf, 1) == 1) {
        snprintf(path, sizeof path, "/proc/%d/wchan", (int)writer);
        for (spins = 0; spins < 50; spins++) {
          tf = fopen(path, "r");
          wchan[0] = 0;
          if (tf) {
            if (fgets(wchan, sizeof wchan, tf) == NULL) wchan[0] = 0;
            fclose(tf);
          }
          if (strstr(wchan, "fifo") || strstr(wchan, "pipe") ||
              strstr(wchan, "partner"))
            break;
          poll(NULL, 0, 20);
        }
      }
      close(kick[0]);
      kick[0] = -1;
    }
    setenv("NANOBOT_AUGOGEN_AUTO", "0", 1);
    js = ng_augogen_handle(NULL, "{\"action\":\"pending\"}");
    expect(js && strstr(js, "\"status\":\"empty\""),
           "a plate fifo stays unread");
    expect(js && strstr(js, "\"hold_flash\":1"),
           "a plate fifo keeps hold flash");
    free(js);
    expect(lstat(platep, &st) == 0 && S_ISFIFO(st.st_mode),
           "a pending read leaves the plate fifo");
    if (writer > 0) {
      int paired = 0;
      for (spins = 0; spins < 10; spins++) {
        got = waitpid(writer, &wst, WNOHANG);
        if (got != 0) {
          paired = 1;
          break;
        }
        poll(NULL, 0, 20);
      }
      expect(!paired && got == 0,
             "a pending read does not pair a writer on the plate fifo");
      if (got > 0) writer = -1;
    }
    js = ng_augogen_handle(
        NULL, "{\"action\":\"generate\",\"enqueue\":\"1\",\"suggestion\":\"move plate\"}");
    expect(js && strstr(js, "\"suggestion\":\"move plate\""),
           "a generate stores over a plate fifo");
    expect(js && strstr(js, "\"status\":\"pending\""),
           "a plate fifo becomes a pending plate");
    expect(js && strstr(js, "\"hold_flash\":1"),
           "a replaced plate fifo keeps hold flash");
    free(js);
    expect(lstat(platep, &st) == 0 && S_ISREG(st.st_mode),
           "the plate path is a regular file after the fifo");
    expect(!S_ISFIFO(st.st_mode), "the plate path is not a fifo after the write");
    if (writer > 0) {
      int paired = 0;
      for (spins = 0; spins < 10; spins++) {
        got = waitpid(writer, &wst, WNOHANG);
        if (got != 0) {
          paired = 1;
          break;
        }
        poll(NULL, 0, 20);
      }
      expect(!paired && got == 0,
             "a generate does not pair a writer on the plate fifo");
      if (got == 0) {
        kill(writer, SIGTERM);
        waitpid(writer, &wst, 0);
      }
      writer = -1;
    }
    tf = fopen(platep, "rb");
    n = 0;
    buf[0] = 0;
    if (tf) {
      n = fread(buf, 1, sizeof buf - 1, tf);
      fclose(tf);
    }
    buf[n] = 0;
    expect(strstr(buf, "\"suggestion\":\"move plate\"") != NULL,
           "the plate file keeps move plate");
    pend = ng_augogen_pending_json();
    expect(pend && strstr(pend, "\"suggestion\":\"move plate\""),
           "a replaced plate fifo still loads");
    free(pend);
    ng_motive_rank("read the law and prove", &m);
    expect(m.rank == 256 && m.lexical == 3 && m.hamming == 0,
           "a plate fifo leaves the law rank");
    ng_motive_rank("you lied", &m);
    expect(m.plate == NG_PLATE_CONFESS && m.lexical == 0,
           "a plate fifo leaves you lied");
    ng_motive_rank("", &m);
    expect(m.rank == 0, "a plate fifo leaves an empty query at rank 0");
    unlink(platep);
    rmdir(aug);
    mkdir(aug, 0700);
  }

  /* An empty directory at the plate temp name is not the plate file.
   * A directory that still holds a name stays. */
  {
    const char *aug = "/tmp/nb-motive-test/augogen";
    const char *platep = "/tmp/nb-motive-test/augogen/pending.json";
    char tmpname[64];
    char tmpdir[256];
    char keepp[320];
    struct stat st;
    DIR *ad;
    struct dirent *de;
    FILE *tf;
    char path[320];
    char buf[512];
    size_t n;
    char *pend;
    snprintf(tmpname, sizeof tmpname, "pending.json.tmp-%d", (int)getpid());
    snprintf(tmpdir, sizeof tmpdir, "%s/%s", aug, tmpname);
    snprintf(keepp, sizeof keepp, "%s/keep.txt", tmpdir);
    ad = opendir(aug);
    if (ad) {
      while ((de = readdir(ad)) != NULL) {
        if (de->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s", aug, de->d_name);
        unlink(path);
        rmdir(path);
      }
      closedir(ad);
    }
    mkdir(aug, 0700);
    unlink(platep);
    unlink(keepp);
    rmdir(tmpdir);
    tf = fopen(platep, "wb");
    expect(tf != NULL, "plant a plate beside a temp directory");
    if (tf) {
      fputs("{\"suggestion\":\"stay temp\",\"status\":\"pending\","
            "\"guide_vote\":-1,\"oversee_vote\":-1,\"mesh_vote\":-1}",
            tf);
      fclose(tf);
    }
    expect(mkdir(tmpdir, 0700) == 0, "plant a temp directory");
    tf = fopen(keepp, "wb");
    expect(tf != NULL, "plant a name inside the temp directory");
    if (tf) {
      fputs("temp stays", tf);
      fclose(tf);
    }
    setenv("NANOBOT_AUGOGEN_AUTO", "0", 1);
    js = ng_augogen_handle(
        NULL, "{\"action\":\"generate\",\"enqueue\":\"1\",\"suggestion\":\"move glen\"}");
    expect(js && strstr(js, "\"hold_flash\":1"),
           "a full temp directory keeps hold flash");
    free(js);
    expect(lstat(tmpdir, &st) == 0 && S_ISDIR(st.st_mode),
           "a temp directory that holds a name stays");
    expect(lstat(keepp, &st) == 0 && S_ISREG(st.st_mode),
           "the write does not enter the temp directory");
    tf = fopen(keepp, "rb");
    n = 0;
    buf[0] = 0;
    if (tf) {
      n = fread(buf, 1, sizeof buf - 1, tf);
      fclose(tf);
    }
    buf[n] = 0;
    expect(strcmp(buf, "temp stays") == 0, "the inner name stays unread");
    pend = ng_augogen_pending_json();
    expect(pend && strstr(pend, "\"suggestion\":\"stay temp\""),
           "a full temp directory does not replace the plate");
    expect(pend && !strstr(pend, "move glen"),
           "a full temp directory does not store move glen");
    free(pend);
    expect(lstat(platep, &st) == 0 && S_ISREG(st.st_mode),
           "the plate path stays a regular file");
    unlink(keepp);
    expect(lstat(tmpdir, &st) == 0 && S_ISDIR(st.st_mode),
           "the temp directory is empty");
    js = ng_augogen_handle(
        NULL, "{\"action\":\"generate\",\"enqueue\":\"1\",\"suggestion\":\"move vale\"}");
    expect(js && strstr(js, "\"suggestion\":\"move vale\""),
           "a generate stores over an empty temp directory");
    expect(js && strstr(js, "\"status\":\"pending\""),
           "an empty temp directory becomes a pending plate");
    expect(js && strstr(js, "\"hold_flash\":1"),
           "a replaced temp directory keeps hold flash");
    free(js);
    expect(lstat(tmpdir, &st) != 0, "the empty temp directory is removed");
    expect(lstat(platep, &st) == 0 && S_ISREG(st.st_mode),
           "the plate path is a regular file after the temp directory");
    tf = fopen(platep, "rb");
    n = 0;
    buf[0] = 0;
    if (tf) {
      n = fread(buf, 1, sizeof buf - 1, tf);
      fclose(tf);
    }
    buf[n] = 0;
    expect(strstr(buf, "\"suggestion\":\"move vale\"") != NULL,
           "the plate file keeps move vale");
    expect(strstr(buf, "stay temp") == NULL,
           "the empty temp directory does not keep the old line");
    pend = ng_augogen_pending_json();
    expect(pend && strstr(pend, "\"suggestion\":\"move vale\""),
           "a replaced temp directory still loads");
    expect(pend && !strstr(pend, "move glen"),
           "a replaced temp directory does not load move glen");
    free(pend);
    ng_motive_rank("read the law and prove", &m);
    expect(m.rank == 256 && m.lexical == 3 && m.hamming == 0,
           "a temp directory leaves the law rank");
    ng_motive_rank("you lied", &m);
    expect(m.plate == NG_PLATE_CONFESS && m.lexical == 0,
           "a temp directory leaves you lied");
    ng_motive_rank("", &m);
    expect(m.rank == 0, "a temp directory leaves an empty query at rank 0");
    unlink(platep);
    unlink(keepp);
    rmdir(tmpdir);
    rmdir(aug);
    mkdir(aug, 0700);
  }

  /* A hard link at the plate path is not the plate.
   * The other name keeps its bytes. */
  {
    const char *aug = "/tmp/nb-motive-test/augogen";
    const char *platep = "/tmp/nb-motive-test/augogen/pending.json";
    const char *side = "/tmp/nb-motive-test/side-plate.json";
    struct stat st;
    struct stat side_st;
    DIR *ad;
    struct dirent *de;
    FILE *tf;
    char path[320];
    char buf[512];
    char sidebuf[512];
    size_t n;
    char *pend;
    ad = opendir(aug);
    if (ad) {
      while ((de = readdir(ad)) != NULL) {
        if (de->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s", aug, de->d_name);
        unlink(path);
        rmdir(path);
      }
      closedir(ad);
    }
    mkdir(aug, 0700);
    unlink(platep);
    unlink(side);
    tf = fopen(platep, "wb");
    expect(tf != NULL, "plant a plate for a second name");
    if (tf) {
      fputs("{\"suggestion\":\"stay name\",\"status\":\"pending\","
            "\"guide_vote\":-1,\"oversee_vote\":-1,\"mesh_vote\":-1}",
            tf);
      fclose(tf);
    }
    expect(link(platep, side) == 0, "plant a second name on the plate inode");
    expect(lstat(platep, &st) == 0 && S_ISREG(st.st_mode) && st.st_nlink > 1,
           "the plate path has two names");
    expect(lstat(side, &side_st) == 0 && side_st.st_ino == st.st_ino,
           "the other name is the same inode");
    setenv("NANOBOT_AUGOGEN_AUTO", "0", 1);
    js = ng_augogen_handle(NULL, "{\"action\":\"pending\"}");
    expect(js && strstr(js, "\"status\":\"empty\""),
           "a hard-linked plate stays unread");
    expect(js && !strstr(js, "stay name"),
           "a hard-linked plate does not return stay name");
    expect(js && strstr(js, "\"hold_flash\":1"),
           "a hard-linked plate keeps hold flash");
    free(js);
    expect(lstat(platep, &st) == 0 && S_ISREG(st.st_mode) && st.st_nlink > 1,
           "a pending read leaves both names");
    tf = fopen(side, "rb");
    n = 0;
    sidebuf[0] = 0;
    if (tf) {
      n = fread(sidebuf, 1, sizeof sidebuf - 1, tf);
      fclose(tf);
    }
    sidebuf[n] = 0;
    expect(strstr(sidebuf, "\"suggestion\":\"stay name\"") != NULL,
           "the other name keeps stay name");
    js = ng_augogen_handle(
        NULL, "{\"action\":\"generate\",\"enqueue\":\"1\",\"suggestion\":\"move scree\"}");
    expect(js && strstr(js, "\"suggestion\":\"move scree\""),
           "a generate stores over a hard-linked plate");
    expect(js && strstr(js, "\"status\":\"pending\""),
           "a hard-linked plate becomes a pending plate");
    expect(js && strstr(js, "\"hold_flash\":1"),
           "a replaced hard link keeps hold flash");
    free(js);
    expect(lstat(platep, &st) == 0 && S_ISREG(st.st_mode) && st.st_nlink == 1,
           "the plate path is a one-link file after the write");
    expect(lstat(side, &side_st) == 0 && S_ISREG(side_st.st_mode) &&
               side_st.st_ino != st.st_ino,
           "the other name is not the new plate inode");
    tf = fopen(side, "rb");
    n = 0;
    buf[0] = 0;
    if (tf) {
      n = fread(buf, 1, sizeof buf - 1, tf);
      fclose(tf);
    }
    buf[n] = 0;
    expect(strcmp(buf, sidebuf) == 0, "the other name keeps its bytes");
    expect(strstr(buf, "stay name") != NULL, "the other name still says stay name");
    expect(strstr(buf, "move scree") == NULL, "the other name does not gain move scree");
    tf = fopen(platep, "rb");
    n = 0;
    buf[0] = 0;
    if (tf) {
      n = fread(buf, 1, sizeof buf - 1, tf);
      fclose(tf);
    }
    buf[n] = 0;
    expect(strstr(buf, "\"suggestion\":\"move scree\"") != NULL,
           "the plate file keeps move scree");
    expect(strstr(buf, "stay name") == NULL,
           "the new plate file does not keep stay name");
    pend = ng_augogen_pending_json();
    expect(pend && strstr(pend, "\"suggestion\":\"move scree\""),
           "a replaced hard link still loads");
    expect(pend && !strstr(pend, "stay name"),
           "a replaced hard link does not load stay name");
    free(pend);
    ng_motive_rank("read the law and prove", &m);
    expect(m.rank == 256 && m.lexical == 3 && m.hamming == 0,
           "a hard link leaves the law rank");
    ng_motive_rank("you lied", &m);
    expect(m.plate == NG_PLATE_CONFESS && m.lexical == 0,
           "a hard link leaves you lied");
    ng_motive_rank("", &m);
    expect(m.rank == 0, "a hard link leaves an empty query at rank 0");
    unlink(platep);
    unlink(side);
    rmdir(aug);
    mkdir(aug, 0700);
  }

  /* A file longer than 8192 bytes at the plate path is not the plate.
   * The same bytes at 8192 are still the plate. One more byte is not. */
  {
    const char *aug = "/tmp/nb-motive-test/augogen";
    const char *platep = "/tmp/nb-motive-test/augogen/pending.json";
    const char *body =
        "{\"suggestion\":\"stay vast\",\"status\":\"pending\","
        "\"guide_vote\":-1,\"oversee_vote\":-1,\"mesh_vote\":-1}";
    struct stat st;
    struct stat before;
    DIR *ad;
    struct dirent *de;
    FILE *tf;
    int pfd;
    int iw;
    int opened;
    char path[320];
    char buf[512];
    char evbuf[256];
    size_t n;
    char *pend;
    ad = opendir(aug);
    if (ad) {
      while ((de = readdir(ad)) != NULL) {
        if (de->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s", aug, de->d_name);
        unlink(path);
        rmdir(path);
      }
      closedir(ad);
    }
    mkdir(aug, 0700);
    unlink(platep);
    tf = fopen(platep, "wb");
    expect(tf != NULL, "plant a short plate");
    if (tf) {
      fputs(body, tf);
      fclose(tf);
    }
    setenv("NANOBOT_AUGOGEN_AUTO", "0", 1);
    js = ng_augogen_handle(NULL, "{\"action\":\"pending\"}");
    expect(js && strstr(js, "\"suggestion\":\"stay vast\""),
           "a short plate still loads");
    expect(js && strstr(js, "\"hold_flash\":1"),
           "a short plate keeps hold flash");
    free(js);
    expect(truncate(platep, 8192) == 0, "grow the plate to 8192");
    expect(lstat(platep, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 8192,
           "the plate path is 8192 bytes");
    iw = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    expect(iw >= 0, "inotify watches the 8192 plate");
    opened = 0;
    if (iw >= 0 && inotify_add_watch(iw, platep, IN_OPEN) >= 0) {
      js = ng_augogen_handle(NULL, "{\"action\":\"pending\"}");
      while (read(iw, evbuf, sizeof evbuf) > 0) opened = 1;
      close(iw);
    } else {
      if (iw >= 0) close(iw);
      js = ng_augogen_handle(NULL, "{\"action\":\"pending\"}");
    }
    expect(opened, "a plate of 8192 bytes is opened");
    expect(js && strstr(js, "\"suggestion\":\"stay vast\""),
           "a plate of 8192 bytes still loads");
    expect(js && strstr(js, "\"hold_flash\":1"),
           "a plate of 8192 bytes keeps hold flash");
    free(js);
    expect(truncate(platep, 8193) == 0, "grow the plate by one byte");
    expect(lstat(platep, &before) == 0 && before.st_size == 8193,
           "the plate path is 8193 bytes");
    iw = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    expect(iw >= 0, "inotify watches the long plate");
    opened = 0;
    if (iw >= 0 && inotify_add_watch(iw, platep, IN_OPEN) >= 0) {
      js = ng_augogen_handle(NULL, "{\"action\":\"pending\"}");
      while (read(iw, evbuf, sizeof evbuf) > 0) opened = 1;
      close(iw);
    } else {
      if (iw >= 0) close(iw);
      js = ng_augogen_handle(NULL, "{\"action\":\"pending\"}");
    }
    expect(!opened, "a long plate is not opened");
    expect(js && strstr(js, "\"status\":\"empty\""),
           "a long plate stays unread");
    expect(js && !strstr(js, "stay vast"),
           "a long plate does not return stay vast");
    expect(js && strstr(js, "\"hold_flash\":1"),
           "a long plate keeps hold flash");
    free(js);
    expect(lstat(platep, &st) == 0 && S_ISREG(st.st_mode) &&
               st.st_size == 8193 && st.st_ino == before.st_ino,
           "a pending read leaves the long file");
    pfd = open(platep, O_RDONLY);
    n = 0;
    buf[0] = 0;
    if (pfd >= 0) {
      ssize_t r = read(pfd, buf, sizeof buf - 1);
      if (r > 0) n = (size_t)r;
      close(pfd);
    }
    buf[n] = 0;
    expect(strstr(buf, "\"suggestion\":\"stay vast\"") != NULL,
           "the long file still holds stay vast");
    js = ng_augogen_handle(
        NULL, "{\"action\":\"generate\",\"enqueue\":\"1\",\"suggestion\":\"move mesa\"}");
    expect(js && strstr(js, "\"suggestion\":\"move mesa\""),
           "a generate stores over a long plate");
    expect(js && strstr(js, "\"status\":\"pending\""),
           "a long plate becomes a pending plate");
    expect(js && strstr(js, "\"hold_flash\":1"),
           "a replaced long plate keeps hold flash");
    free(js);
    expect(lstat(platep, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 &&
               st.st_size <= 8192 && st.st_nlink == 1,
           "the plate path is a short one-link file after the write");
    tf = fopen(platep, "rb");
    n = 0;
    buf[0] = 0;
    if (tf) {
      n = fread(buf, 1, sizeof buf - 1, tf);
      fclose(tf);
    }
    buf[n] = 0;
    expect(strstr(buf, "\"suggestion\":\"move mesa\"") != NULL,
           "the plate file keeps move mesa");
    expect(strstr(buf, "stay vast") == NULL,
           "the new plate file does not keep stay vast");
    pend = ng_augogen_pending_json();
    expect(pend && strstr(pend, "\"suggestion\":\"move mesa\""),
           "a replaced long plate still loads");
    expect(pend && !strstr(pend, "stay vast"),
           "a replaced long plate does not load stay vast");
    free(pend);
    ng_motive_rank("read the law and prove", &m);
    expect(m.rank == 256 && m.lexical == 3 && m.hamming == 0,
           "a long plate leaves the law rank");
    ng_motive_rank("you lied", &m);
    expect(m.plate == NG_PLATE_CONFESS && m.lexical == 0,
           "a long plate leaves you lied");
    ng_motive_rank("", &m);
    expect(m.rank == 0, "a long plate leaves an empty query at rank 0");
    unlink(platep);
    rmdir(aug);
    mkdir(aug, 0700);
  }

  {
    const char *law_t[] = {"read the law and prove"};
    const char *law_p[] = {"read the law and prove"};
    const char *ship_t[] = {"why did the ship fail"};
    const char *audit_p[] = {"audit the last code change"};
    const char *bogus_p[] = {"not a catalog line"};
    const char *tie_t[] = {"read the law and prove", "why did the ship fail"};
    const char *tie_p[] = {"read the law and prove", "pair vote then ship the step"};
    const char *path = "/tmp/nb-motive-learn.pairs";
    int learned;
    FILE *pf;

    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    expect(ng_motive_learn_lex_w(NULL, NULL, 0) == 64, "no pairs stay at weight 64");
    expect(ng_motive_learn_lex_w(law_t, NULL, 1) == 64, "missing prefer stays at weight 64");
    expect(ng_motive_learn_lex_w(law_t, law_p, 1) == 64,
           "an exact catalog line keeps weight 64");
    learned = ng_motive_learn_lex_w(ship_t, audit_p, 1);
    expect(learned == 0, "the smallest hitting weight is kept");
    ng_motive_rank_w("why did the ship fail", &m, learned);
    expect(strcmp(m.concept, "audit the last code change") == 0,
           "the learned weight ranks the preferred catalog line");
    ng_motive_rank_w("why did the ship fail", &m, 64);
    expect(m.lex_w == 64 && strcmp(m.concept, "pair vote then ship the step") == 0,
           "weight 64 still ranks the pair line");
    expect(ng_motive_learn_lex_w(ship_t, bogus_p, 1) == 64,
           "an unknown preferred line keeps weight 64");
    expect(ng_motive_learn_lex_w(tie_t, tie_p, 2) == 64,
           "weight 64 wins a tie of hit counts");
    pf = fopen(path, "w");
    expect(pf != NULL, "pairs file opens");
    if (pf) {
      fputs("# note\n\n", pf);
      fputs("why did the ship fail\taudit the last code change\n", pf);
      fclose(pf);
    }
    setenv("NANOBOT_MOTIVE_LEARN", path, 1);
    setenv("NANOBOT_MOTIVE_LEX_W", "1", 1);
    ng_motive_rank("why did the ship fail", &m);
    expect(m.lex_w == learned, "the pairs file overrides NANOBOT_MOTIVE_LEX_W");
    expect(strcmp(m.concept, "audit the last code change") == 0,
           "the pairs file ranks the preferred line");
    ng_motive_rank_w("read the law and prove", &m, 64);
    expect(m.rank == 256 && m.lexical == 3 && m.hamming == 0 && m.lex_w == 64,
           "a call weight still overrides the pairs file");
    ng_motive_rank("read the law and prove", &m);
    expect(strcmp(m.concept, "read the law and prove") == 0, "learned weight keeps the law line");
    expect(m.lexical == 3 && m.hamming == 0, "learned weight keeps the law features");
    expect(m.rank == 3 * learned + (64 - 0), "learned weight keeps the hybrid score");
    expect(m.lex_w == learned, "learned weight is the reported lex_w");
    setenv("NANOBOT_MOTIVE_LEARN", "/tmp/nb-motive-learn-missing", 1);
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    ng_motive_rank("read the law and prove", &m);
    expect(m.rank == 256 && m.lex_w == 64, "a missing pairs file keeps weight 64");
    unsetenv("NANOBOT_MOTIVE_LEARN");
    ng_motive_rank("read the law and prove", &m);
    expect(m.rank == 256 && m.lex_w == 64 && m.lexical == 3 && m.hamming == 0,
           "clearing the pairs path restores the law rank");
    ng_motive_rank("why did the ship fail", &m);
    expect(m.lex_w == 64 && strcmp(m.concept, "pair vote then ship the step") == 0,
           "clearing the pairs path restores the pair line");
    unlink(path);
  }

  {
    const char *path = "/tmp/nb-motive-recall.pairs";
    char *got = NULL;
    FILE *pf;
    ng_motive before, after;

    ng_motive_rank("read the law and prove", &before);
    got = ng_motive_recall_pref(NULL, path);
    expect(got == NULL, "a null query recalls nothing");
    got = ng_motive_recall_pref("", path);
    expect(got == NULL, "an empty query recalls nothing");
    got = ng_motive_recall_pref("...", path);
    expect(got == NULL, "a query with no tokens recalls nothing");
    got = ng_motive_recall_pref("why did the ship fail",
                                "/tmp/nb-motive-recall-missing");
    expect(got == NULL, "a missing pairs file recalls nothing");
    pf = fopen(path, "w");
    expect(pf != NULL, "recall pairs file opens");
    if (pf) {
      fputs("# note\n\n", pf);
      fputs("read the law\taudit the last code change\n", pf);
      fputs("ship fail\tkeep free energy flowing\n", pf);
      fputs("why did the ship fail\trank concepts then act\n", pf);
      fputs("fail ship\tfold the matrix and decide\n", pf);
      fputs("the and then\tconfess the heat and serve\n", pf);
      fclose(pf);
    }
    got = ng_motive_recall_pref("why did the ship fail", path);
    expect(got != NULL && strcmp(got, "rank concepts then act") == 0,
           "the greatest lexical overlap wins");
    free(got);
    got = ng_motive_recall_pref("ship fail", path);
    expect(got != NULL && strcmp(got, "keep free energy flowing") == 0,
           "an equal overlap keeps the earlier pair");
    free(got);
    got = ng_motive_recall_pref("prompt a peer to move", path);
    expect(got == NULL, "zero overlap recalls nothing");
    got = ng_motive_recall_pref("the and then a to", path);
    expect(got == NULL, "glue words alone are not overlap");
    ng_motive_rank("read the law and prove", &after);
    expect(before.rank == 256 && after.rank == 256 && after.lexical == 3 &&
               after.hamming == 0 && before.lex_w == after.lex_w,
           "recall leaves the law rank");
    ng_motive_rank("you lied", &after);
    expect(after.plate == NG_PLATE_CONFESS && after.lexical == 0,
           "recall leaves you lied");
    unlink(path);
  }

  {
    const char *path = "/tmp/nb-motive-hits.pairs";
    const char *dirp = "/tmp/nb-motive-hits.dir";
    const char *ship = "why did the ship fail\taudit the last code change\n";
    const char *padded = "why did the ship fail\tkeep free energy flowing  \n";
    char longp[520];
    char qbuf[200];
    char cbuf[96];
    char blob[16384];
    FILE *pf;
    ng_motive before, after;
    struct stat st;
    struct timespec ts[2];
    int i;

    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    ng_motive_rank("read the law and prove", &before);
    expect(ng_motive_learn_hits(NULL) == -1, "a null pairs path counts nothing");
    expect(ng_motive_learn_hits("") == -1, "an empty pairs path counts nothing");
    memset(longp, 'p', 512);
    longp[512] = 0;
    expect(strlen(longp) == 512, "the long pairs path is 512 bytes");
    expect(ng_motive_learn_hits(longp) == -1,
           "a pairs path of 512 bytes counts nothing");
    expect(ng_motive_learn_hits("/tmp/nb-motive-hits-missing") == -1,
           "a missing pairs file counts nothing");
    unlink(dirp);
    rmdir(dirp);
    expect(mkdir(dirp, 0700) == 0, "hits directory creates");
    expect(ng_motive_learn_hits(dirp) == -1,
           "a non-regular pairs path counts nothing");
    rmdir(dirp);

    pf = fopen(path, "w");
    expect(pf != NULL, "hits pairs file opens");
    if (pf) {
      fputs("# note\n\n", pf);
      fclose(pf);
    }
    expect(ng_motive_learn_hits(path) == -1,
           "a pairs file with no pair counts nothing");

    memset(qbuf, 'q', 192);
    qbuf[192] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "hits overlong query file opens");
    if (pf) {
      fprintf(pf, "%s\taudit the last code change\n", qbuf);
      fclose(pf);
    }
    expect(ng_motive_learn_hits(path) == -1,
           "a query of 192 bytes is not a pair");
    qbuf[191] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "hits bounded query file opens");
    if (pf) {
      fprintf(pf, "%s\tnot a catalog line\n", qbuf);
      fclose(pf);
    }
    expect(ng_motive_learn_hits(path) == 0,
           "a query under 192 bytes is a pair and can count zero");

    memset(cbuf, 'c', 80);
    cbuf[80] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "hits overlong catalog file opens");
    if (pf) {
      fprintf(pf, "why did the ship fail\t%s\n", cbuf);
      fclose(pf);
    }
    expect(ng_motive_learn_hits(path) == -1,
           "a catalog line of 80 bytes is not a pair");
    cbuf[79] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "hits bounded catalog file opens");
    if (pf) {
      fprintf(pf, "why did the ship fail\t%s\n", cbuf);
      fclose(pf);
    }
    expect(ng_motive_learn_hits(path) == 0,
           "a catalog line under 80 bytes can count zero");

    memset(blob, 'x', sizeof blob);
    pf = fopen(path, "w");
    expect(pf != NULL, "hits budget file opens");
    if (pf) {
      fwrite(blob, 1, sizeof blob, pf);
      fputc('\n', pf);
      fputs(ship, pf);
      fclose(pf);
    }
    expect(ng_motive_learn_hits(path) == -1,
           "a pair past the 16384 byte budget is not read");
    pf = fopen(path, "w");
    expect(pf != NULL, "hits budget-after file opens");
    if (pf) {
      fputs("# note\n\n", pf);
      fputs(ship, pf);
      fwrite(blob, 1, sizeof blob, pf);
      fputc('\n', pf);
      fclose(pf);
    }
    setenv("NANOBOT_MOTIVE_LEARN", "/tmp/nb-motive-hits-missing", 1);
    setenv("NANOBOT_MOTIVE_LEX_W", "1", 1);
    expect(ng_motive_learn_hits(path) == 1,
           "one preferred catalog line counts as one");
    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    ng_motive_rank("why did the ship fail", &after);
    expect(after.lex_w == 64 &&
               strcmp(after.concept, "pair vote then ship the step") == 0,
           "hits does not install a learned weight");

    pf = fopen(path, "w");
    expect(pf != NULL, "hits cache file opens");
    if (pf) {
      fputs(ship, pf);
      fclose(pf);
    }
    expect(ng_motive_learn_hits(path) == 1,
           "the ship pair still counts as one");
    expect(strlen(ship) == strlen(padded), "cache probe stays the same size");
    expect(stat(path, &st) == 0 && st.st_size == (off_t)strlen(ship),
           "hits pairs stat");
    ts[0].tv_sec = st.st_atim.tv_sec;
    ts[0].tv_nsec = st.st_atim.tv_nsec;
    ts[1].tv_sec = st.st_mtim.tv_sec;
    ts[1].tv_nsec = st.st_mtim.tv_nsec;
    pf = fopen(path, "w");
    expect(pf != NULL, "hits cache probe opens");
    if (pf) {
      fputs(padded, pf);
      fclose(pf);
    }
    expect(utimensat(AT_FDCWD, path, ts, 0) == 0, "hits cache probe restores mtime");
    setenv("NANOBOT_MOTIVE_LEARN", path, 1);
    ng_motive_rank("why did the ship fail", &after);
    expect(after.lex_w == 64 &&
               strcmp(after.concept, "pair vote then ship the step") == 0,
           "hits does not write the learn cache");
    unsetenv("NANOBOT_MOTIVE_LEARN");

    pf = fopen(path, "w");
    expect(pf != NULL, "hits zero file opens");
    if (pf) {
      fputs(padded, pf);
      fclose(pf);
    }
    expect(ng_motive_learn_hits(path) == 0,
           "an unknown preferred line counts zero");

    pf = fopen(path, "w");
    expect(pf != NULL, "hits two-pair file opens");
    if (pf) {
      fputs(ship, pf);
      fputs("read the law and prove\tread the law and prove\n", pf);
      fclose(pf);
    }
    expect(ng_motive_learn_hits(path) == 2,
           "two preferred catalog lines count as two");

    pf = fopen(path, "w");
    expect(pf != NULL, "hits tie file opens");
    if (pf) {
      fputs("read the law and prove\tread the law and prove\n", pf);
      fputs("keep free energy flowing\tkeep free energy flowing\n", pf);
      fclose(pf);
    }
    expect(ng_motive_learn_hits(path) == 2,
           "a tie that includes weight 64 still counts both lines");

    pf = fopen(path, "w");
    expect(pf != NULL, "hits cap file opens");
    if (pf) {
      for (i = 0; i < 32; i++) fputs("nope\tnot a catalog line\n", pf);
      fputs(ship, pf);
      fclose(pf);
    }
    expect(ng_motive_learn_hits(path) == 0, "the pair past 32 is not counted");

    pf = fopen(path, "w");
    expect(pf != NULL, "hits within-cap file opens");
    if (pf) {
      for (i = 0; i < 31; i++) fputs("nope\tnot a catalog line\n", pf);
      fputs(ship, pf);
      fclose(pf);
    }
    expect(ng_motive_learn_hits(path) == 1, "the 32nd pair is counted");

    pf = fopen(path, "w");
    expect(pf != NULL, "hits full file opens");
    if (pf) {
      for (i = 0; i < 32; i++) fputs(ship, pf);
      fclose(pf);
    }
    expect(ng_motive_learn_hits(path) == 32, "thirty-two preferred lines count");

    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    ng_motive_rank("read the law and prove", &after);
    expect(before.rank == 256 && after.rank == 256 && after.lexical == 3 &&
               after.hamming == 0 && before.lex_w == after.lex_w,
           "hits leaves the law rank");
    ng_motive_rank("you lied", &after);
    expect(after.plate == NG_PLATE_CONFESS && after.lexical == 0,
           "hits leaves you lied");
    ng_motive_rank("", &after);
    expect(after.rank == 0, "hits leaves an empty query at rank 0");
    unlink(path);
  }

  {
    const char *path = "/tmp/nb-motive-label.pairs";
    const char *dirp = "/tmp/nb-motive-label.dir";
    const char *keep = "keep free energy flowing";
    const char *pair = "pair vote then ship the step";
    const char *law = "read the law and prove";
    const char *ship = "ship fail\tkeep free energy flowing\n";
    const char *audit = "why did the ship fail\taudit the last code change\n";
    const char *padded = "why did the ship fail\tkeep free energy flowing  \n";
    char qbuf[200];
    char cbuf[96];
    char blob[16384];
    char big[420];
    FILE *pf;
    ng_motive m0, m255, ranked;
    struct stat st;
    struct timespec ts[2];
    int i;

    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    expect(ng_motive_label_lex(NULL, path, keep) == 0, "a null query labels nothing");
    expect(ng_motive_label_lex("", path, keep) == 0, "an empty query labels nothing");
    expect(ng_motive_label_lex("ship fail", NULL, keep) == 0, "a null path labels nothing");
    expect(ng_motive_label_lex("ship fail", "", keep) == 0, "an empty path labels nothing");
    expect(ng_motive_label_lex("ship fail", path, NULL) == 0, "a null concept labels nothing");
    expect(ng_motive_label_lex("ship fail", path, "") == 0, "an empty concept labels nothing");
    expect(ng_motive_label_lex("...", path, keep) == 0, "a query with no tokens labels nothing");
    expect(ng_motive_label_lex("ship fail", "/tmp/nb-motive-label-missing", keep) == 0,
           "a missing pairs file labels nothing");
    unlink(dirp);
    rmdir(dirp);
    expect(mkdir(dirp, 0700) == 0, "label directory creates");
    expect(ng_motive_label_lex("ship fail", dirp, keep) == 0,
           "a non-regular pairs path labels nothing");
    rmdir(dirp);
    expect(ng_motive_label_lex("ship fail", path, "not a catalog line") == 0,
           "an unknown concept labels nothing");
    expect(ng_motive_label_lex("ship fail", path, "keep free energy flowing ") == 0,
           "a concept with trailing spaces labels nothing");

    pf = fopen(path, "w");
    expect(pf != NULL, "label pairs file opens");
    if (pf) {
      fputs("# ship fail\tkeep free energy flowing\n\n", pf);
      fputs(ship, pf);
      fclose(pf);
    }
    expect(ng_motive_label_lex("ship fail", path, keep) == 2,
           "ship fail has two hits beyond the concept line");
    expect(ng_motive_label_lex("ship fail", path, pair) == 0,
           "an unlabeled concept adds nothing");
    expect(ng_motive_label_lex("keep free energy flowing", path, keep) == 0,
           "a negative label difference is zero");
    setenv("NANOBOT_MOTIVE_LEARN", "/tmp/nb-motive-label-missing", 1);
    expect(ng_motive_label_lex("ship fail", path, keep) == 2,
           "the label count uses the path, not the environment");
    unsetenv("NANOBOT_MOTIVE_LEARN");

    ng_motive_rank_w("ship fail", &m0, 0);
    ng_motive_rank_w("ship fail", &m255, 255);
    ng_motive_rank("ship fail", &ranked);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lexical == 1 && ranked.hamming == 23 && ranked.lex_w == 64,
           "unset learn leaves ship fail on the pair line");
    setenv("NANOBOT_MOTIVE_LEARN", path, 1);
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    ng_motive_rank("ship fail", &ranked);
    if (!(strcmp(ranked.concept, keep) == 0 && ranked.lexical == 2 &&
          ranked.hamming == 22 && ranked.rank == 170 && ranked.lex_w == 64)) {
      fprintf(stderr,
              "FAIL label rank concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              ranked.concept, ranked.lexical, ranked.hamming, ranked.rank,
              ranked.lex_w);
      g_fail++;
    }
    ng_motive_rank("read the law and prove", &ranked);
    if (!(strcmp(ranked.concept, law) == 0 && ranked.rank == 256 &&
          ranked.lexical == 3 && ranked.hamming == 0 && ranked.lex_w == 64)) {
      fprintf(stderr,
              "FAIL label law concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              ranked.concept, ranked.lexical, ranked.hamming, ranked.rank,
              ranked.lex_w);
      g_fail++;
    }
    ng_motive_rank_w("ship fail", &ranked, 64);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lexical == 1 && ranked.hamming == 23 && ranked.lex_w == 64,
           "weight 64 does not add the label");
    ng_motive_rank_w("ship fail", &ranked, 0);
    expect(ranked.rank == m0.rank && ranked.lexical == m0.lexical &&
               ranked.hamming == m0.hamming && ranked.lex_w == m0.lex_w &&
               strcmp(ranked.concept, m0.concept) == 0,
           "weight 0 does not read the label");
    ng_motive_rank_w("ship fail", &ranked, 255);
    expect(ranked.rank == m255.rank && ranked.lexical == m255.lexical &&
               ranked.hamming == m255.hamming && ranked.lex_w == m255.lex_w &&
               strcmp(ranked.concept, m255.concept) == 0,
           "weight 255 does not read the label");
    unsetenv("NANOBOT_MOTIVE_LEARN");
    ng_motive_rank("ship fail", &ranked);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lex_w == 64,
           "clearing the learn path restores the pair line");

    pf = fopen(path, "w");
    expect(pf != NULL, "label pad file opens");
    if (pf) {
      fputs("ship fail\tkeep free energy flowing \n", pf);
      fclose(pf);
    }
    expect(ng_motive_label_lex("ship fail", path, keep) == 0,
           "a catalog field with trailing spaces is not the concept");
    setenv("NANOBOT_MOTIVE_LEARN", path, 1);
    ng_motive_rank("ship fail", &ranked);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lexical == 1 && ranked.hamming == 23 && ranked.lex_w == 64,
           "trailing spaces add nothing");
    unsetenv("NANOBOT_MOTIVE_LEARN");

    pf = fopen(path, "w");
    expect(pf != NULL, "label law file opens");
    if (pf) {
      fputs("read the law and prove\tread the law and prove\n", pf);
      fclose(pf);
    }
    expect(ng_motive_label_lex(law, path, law) == 0,
           "an exact law pair adds no hits beyond the line");
    setenv("NANOBOT_MOTIVE_LEARN", path, 1);
    ng_motive_rank(law, &ranked);
    expect(strcmp(ranked.concept, law) == 0 && ranked.rank == 256 &&
               ranked.lexical == 3 && ranked.hamming == 0 && ranked.lex_w == 64,
           "an exact law pair leaves the law rank");
    unsetenv("NANOBOT_MOTIVE_LEARN");

    pf = fopen(path, "w");
    expect(pf != NULL, "label overlap file opens");
    if (pf) {
      fputs("ship\tkeep free energy flowing\n", pf);
      fputs(ship, pf);
      fclose(pf);
    }
    expect(ng_motive_label_lex("ship fail", path, keep) == 2,
           "the greater training overlap wins");
    pf = fopen(path, "w");
    expect(pf != NULL, "label overlap-early file opens");
    if (pf) {
      fputs(ship, pf);
      fputs("ship\tkeep free energy flowing\n", pf);
      fclose(pf);
    }
    expect(ng_motive_label_lex("ship fail", path, keep) == 2,
           "a later smaller overlap does not replace the earlier query");

    memset(qbuf, 'q', 192);
    memcpy(qbuf, "ship fail ", 10);
    qbuf[192] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "label overlong query file opens");
    if (pf) {
      fprintf(pf, "%s\t%s\n", qbuf, keep);
      fclose(pf);
    }
    expect(ng_motive_label_lex("ship fail", path, keep) == 0,
           "a query of 192 bytes is not a pair");
    qbuf[191] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "label bounded query file opens");
    if (pf) {
      fprintf(pf, "%s\t%s\n", qbuf, keep);
      fclose(pf);
    }
    expect(ng_motive_label_lex("ship fail", path, keep) == 2,
           "a query under 192 bytes can label the concept");

    memset(cbuf, 'c', 80);
    cbuf[80] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "label overlong catalog file opens");
    if (pf) {
      fprintf(pf, "ship fail\t%s\n", cbuf);
      fclose(pf);
    }
    expect(ng_motive_label_lex("ship fail", path, keep) == 0,
           "a catalog line of 80 bytes is not a pair");

    memset(blob, 'x', sizeof blob);
    pf = fopen(path, "w");
    expect(pf != NULL, "label budget file opens");
    if (pf) {
      fwrite(blob, 1, sizeof blob, pf);
      fputc('\n', pf);
      fputs(ship, pf);
      fclose(pf);
    }
    expect(ng_motive_label_lex("ship fail", path, keep) == 0,
           "a pair past the 16384 byte budget is not read");

    memset(big, 'z', 400);
    big[400] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "label overlong line file opens");
    if (pf) {
      fprintf(pf, "%s\n", big);
      fputs(ship, pf);
      fclose(pf);
    }
    expect(ng_motive_label_lex("ship fail", path, keep) == 2,
           "an overlong line is skipped");

    pf = fopen(path, "w");
    expect(pf != NULL, "label cap file opens");
    if (pf) {
      for (i = 0; i < 32; i++) fputs("nope\tnot a catalog line\n", pf);
      fputs(ship, pf);
      fclose(pf);
    }
    expect(ng_motive_label_lex("ship fail", path, keep) == 0,
           "the pair past 32 is not labeled");
    pf = fopen(path, "w");
    expect(pf != NULL, "label within-cap file opens");
    if (pf) {
      for (i = 0; i < 31; i++) fputs("nope\tnot a catalog line\n", pf);
      fputs(ship, pf);
      fclose(pf);
    }
    expect(ng_motive_label_lex("ship fail", path, keep) == 2, "the 32nd pair is labeled");

    pf = fopen(path, "w");
    expect(pf != NULL, "label cache file opens");
    if (pf) {
      fputs(audit, pf);
      fclose(pf);
    }
    expect(strlen(audit) == strlen(padded), "label cache probe stays the same size");
    expect(ng_motive_label_lex("why did the ship fail", path,
                               "audit the last code change") == 4,
           "the audit training query has four hits beyond that line");
    expect(stat(path, &st) == 0, "label cache stat");
    ts[0].tv_sec = st.st_atim.tv_sec;
    ts[0].tv_nsec = st.st_atim.tv_nsec;
    ts[1].tv_sec = st.st_mtim.tv_sec;
    ts[1].tv_nsec = st.st_mtim.tv_nsec;
    pf = fopen(path, "w");
    expect(pf != NULL, "label cache probe opens");
    if (pf) {
      fputs(padded, pf);
      fclose(pf);
    }
    expect(utimensat(AT_FDCWD, path, ts, 0) == 0, "label cache probe restores mtime");
    setenv("NANOBOT_MOTIVE_LEARN", path, 1);
    ng_motive_rank("why did the ship fail", &ranked);
    expect(ranked.lex_w == 64 && strcmp(ranked.concept, pair) == 0,
           "label does not write the learn cache");
    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    ng_motive_rank(law, &ranked);
    expect(ranked.rank == 256 && ranked.lexical == 3 && ranked.hamming == 0 &&
               ranked.lex_w == 64,
           "label leaves the law rank");
    ng_motive_rank("you lied", &ranked);
    expect(ranked.plate == NG_PLATE_CONFESS && ranked.lexical == 0,
           "label leaves you lied");
    ng_motive_rank("", &ranked);
    expect(ranked.rank == 0, "label leaves an empty query at rank 0");
    ng_motive_rank("ship fail", &ranked);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105,
           "label leaves ship fail on the pair line");
    unlink(path);
  }

  {
    const char *path = "/tmp/nb-motive-marked.pairs";
    const char *dirp = "/tmp/nb-motive-marked.dir";
    const char *keep = "keep free energy flowing";
    const char *pair = "pair vote then ship the step";
    const char *law = "read the law and prove";
    const char *audit_c = "audit the last code change";
    const char *peer = "prompt a peer to move";
    const char *ship = "ship fail\tkeep free energy flowing\n";
    const char *audit = "why did the ship fail\taudit the last code change\n";
    const char *lawln = "read the law and prove\tread the law and prove\n";
    const char *padded = "why did the ship fail\tkeep free energy flowing  \n";
    const char *trail = "ship fail\tkeep free energy flowing \n";
    char longp[520];
    char qbuf[200];
    char cbuf[96];
    char blob[16384];
    char big[420];
    FILE *pf;
    ng_motive before, after, ranked;
    struct stat st;
    struct timespec ts[2];
    const char *one_t[1];
    const char *one_p[1];
    int i, learned, marked;

    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    ng_motive_rank(law, &before);
    expect(ng_motive_label_hits(NULL) == -1, "a null marked path counts nothing");
    expect(ng_motive_label_hits("") == -1, "an empty marked path counts nothing");
    memset(longp, 'p', 512);
    longp[512] = 0;
    expect(strlen(longp) == 512, "the long marked path is 512 bytes");
    expect(ng_motive_label_hits(longp) == -1,
           "a marked path of 512 bytes counts nothing");
    expect(ng_motive_label_hits("/tmp/nb-motive-marked-missing") == -1,
           "a missing marked file counts nothing");
    unlink(dirp);
    rmdir(dirp);
    expect(mkdir(dirp, 0700) == 0, "marked directory creates");
    expect(ng_motive_label_hits(dirp) == -1,
           "a non-regular marked path counts nothing");
    rmdir(dirp);

    pf = fopen(path, "w");
    expect(pf != NULL, "marked empty file opens");
    if (pf) {
      fputs("# note\n\n", pf);
      fclose(pf);
    }
    expect(ng_motive_label_hits(path) == -1,
           "a marked file with no pair counts nothing");

    memset(qbuf, 'q', 192);
    qbuf[192] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "marked overlong query file opens");
    if (pf) {
      fprintf(pf, "%s\t%s\n", qbuf, audit_c);
      fclose(pf);
    }
    expect(ng_motive_label_hits(path) == -1,
           "a marked query of 192 bytes is not a pair");
    qbuf[191] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "marked bounded query file opens");
    if (pf) {
      fprintf(pf, "%s\tnot a catalog line\n", qbuf);
      fclose(pf);
    }
    expect(ng_motive_label_hits(path) == 0,
           "a marked query under 192 bytes can count zero");

    memset(cbuf, 'c', 80);
    cbuf[80] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "marked overlong catalog file opens");
    if (pf) {
      fprintf(pf, "why did the ship fail\t%s\n", cbuf);
      fclose(pf);
    }
    expect(ng_motive_label_hits(path) == -1,
           "a marked catalog line of 80 bytes is not a pair");
    cbuf[79] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "marked bounded catalog file opens");
    if (pf) {
      fprintf(pf, "why did the ship fail\t%s\n", cbuf);
      fclose(pf);
    }
    expect(ng_motive_label_hits(path) == 0,
           "a marked catalog line under 80 bytes can count zero");

    memset(blob, 'x', sizeof blob);
    pf = fopen(path, "w");
    expect(pf != NULL, "marked budget file opens");
    if (pf) {
      fwrite(blob, 1, sizeof blob, pf);
      fputc('\n', pf);
      fputs(audit, pf);
      fclose(pf);
    }
    expect(ng_motive_label_hits(path) == -1,
           "a marked pair past the 16384 byte budget is not read");

    memset(big, 'z', 400);
    big[400] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "marked overlong line file opens");
    if (pf) {
      fprintf(pf, "%s\n", big);
      fputs(audit, pf);
      fclose(pf);
    }
    learned = ng_motive_learn_hits(path);
    marked = ng_motive_label_hits(path);
    if (learned != 1 || marked != 1) {
      fprintf(stderr, "FAIL marked skip learn=%d label=%d\n", learned, marked);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "marked ship file opens");
    if (pf) {
      fputs(ship, pf);
      fclose(pf);
    }
    learned = ng_motive_learn_hits(path);
    marked = ng_motive_label_hits(path);
    if (learned != 0 || marked != 1) {
      fprintf(stderr, "FAIL marked ship learn=%d label=%d\n", learned, marked);
      g_fail++;
    }
    setenv("NANOBOT_MOTIVE_LEARN", "/tmp/nb-motive-marked-missing", 1);
    setenv("NANOBOT_MOTIVE_LEX_W", "1", 1);
    expect(ng_motive_label_hits(path) == 1,
           "the marked count does not read the environment");
    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    ng_motive_rank("why did the ship fail", &after);
    expect(after.lex_w == 64 && strcmp(after.concept, pair) == 0,
           "marked does not install a learned weight");

    pf = fopen(path, "w");
    expect(pf != NULL, "marked audit file opens");
    if (pf) {
      fputs(audit, pf);
      fclose(pf);
    }
    learned = ng_motive_learn_hits(path);
    marked = ng_motive_label_hits(path);
    one_t[0] = "why did the ship fail";
    one_p[0] = audit_c;
    if (learned != 1 || marked != 1 ||
        ng_motive_learn_lex_w(one_t, one_p, 1) != 0) {
      fprintf(stderr, "FAIL marked audit learn=%d label=%d fit=%d\n", learned,
              marked, ng_motive_learn_lex_w(one_t, one_p, 1));
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "marked law file opens");
    if (pf) {
      fputs(lawln, pf);
      fclose(pf);
    }
    learned = ng_motive_learn_hits(path);
    marked = ng_motive_label_hits(path);
    if (learned != 1 || marked != 1) {
      fprintf(stderr, "FAIL marked law learn=%d label=%d\n", learned, marked);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "marked trail file opens");
    if (pf) {
      fputs(trail, pf);
      fclose(pf);
    }
    learned = ng_motive_learn_hits(path);
    marked = ng_motive_label_hits(path);
    if (learned != 0 || marked != 0) {
      fprintf(stderr, "FAIL marked trail learn=%d label=%d\n", learned, marked);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "marked empty-norm file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fclose(pf);
    }
    expect(ng_motive_label_hits(path) == 1,
           "an empty norm names the peer-prompt concept");
    pf = fopen(path, "w");
    expect(pf != NULL, "marked empty-norm other file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", keep);
      fclose(pf);
    }
    expect(ng_motive_label_hits(path) == 0,
           "an empty norm does not name another concept");

    pf = fopen(path, "w");
    expect(pf != NULL, "marked cap file opens");
    if (pf) {
      for (i = 0; i < 32; i++) fputs("nope\tnot a catalog line\n", pf);
      fputs(audit, pf);
      fclose(pf);
    }
    learned = ng_motive_learn_hits(path);
    marked = ng_motive_label_hits(path);
    if (learned != 0 || marked != 0) {
      fprintf(stderr, "FAIL marked past learn=%d label=%d\n", learned, marked);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "marked within-cap file opens");
    if (pf) {
      for (i = 0; i < 31; i++) fputs("nope\tnot a catalog line\n", pf);
      fputs(audit, pf);
      fclose(pf);
    }
    learned = ng_motive_learn_hits(path);
    marked = ng_motive_label_hits(path);
    if (learned != 1 || marked != 1) {
      fprintf(stderr, "FAIL marked 32 learn=%d label=%d\n", learned, marked);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "marked cache file opens");
    if (pf) {
      fputs(audit, pf);
      fclose(pf);
    }
    expect(strlen(audit) == strlen(padded), "marked cache probe stays the same size");
    expect(ng_motive_label_hits(path) == 1, "the audit pair is marked once");
    expect(stat(path, &st) == 0 && st.st_size == (off_t)strlen(audit),
           "marked pairs stat");
    ts[0].tv_sec = st.st_atim.tv_sec;
    ts[0].tv_nsec = st.st_atim.tv_nsec;
    ts[1].tv_sec = st.st_mtim.tv_sec;
    ts[1].tv_nsec = st.st_mtim.tv_nsec;
    pf = fopen(path, "w");
    expect(pf != NULL, "marked cache probe opens");
    if (pf) {
      fputs(padded, pf);
      fclose(pf);
    }
    expect(utimensat(AT_FDCWD, path, ts, 0) == 0, "marked cache probe restores mtime");
    setenv("NANOBOT_MOTIVE_LEARN", path, 1);
    ng_motive_rank("why did the ship fail", &after);
    expect(after.lex_w == 64 && strcmp(after.concept, pair) == 0,
           "marked does not write the learn cache");
    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    ng_motive_rank("why did the ship fail", &after);
    expect(after.lex_w == 64 && strcmp(after.concept, pair) == 0,
           "marked leaves why did the ship fail on the pair line");

    pf = fopen(path, "w");
    expect(pf != NULL, "marked sealed file opens");
    if (pf) {
      fputs(ship, pf);
      fclose(pf);
    }
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    setenv("NANOBOT_MOTIVE_LEARN", path, 1);
    ng_motive_rank("ship fail", &ranked);
    if (!(strcmp(ranked.concept, keep) == 0 && ranked.lexical == 2 &&
          ranked.hamming == 22 && ranked.rank == 170 && ranked.lex_w == 64)) {
      fprintf(stderr,
              "FAIL marked rank concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              ranked.concept, ranked.lexical, ranked.hamming, ranked.rank,
              ranked.lex_w);
      g_fail++;
    }
    ng_motive_rank_w("ship fail", &ranked, 64);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lexical == 1 && ranked.hamming == 23 && ranked.lex_w == 64,
           "weight 64 still returns the pair line");
    unsetenv("NANOBOT_MOTIVE_LEARN");
    ng_motive_rank("ship fail", &ranked);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lex_w == 64,
           "unset learn still leaves the pair line");
    ng_motive_rank(law, &ranked);
    if (!(strcmp(ranked.concept, law) == 0 && ranked.rank == 256 &&
          ranked.lexical == 3 && ranked.hamming == 0 && ranked.lex_w == 64)) {
      fprintf(stderr,
              "FAIL marked law concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              ranked.concept, ranked.lexical, ranked.hamming, ranked.rank,
              ranked.lex_w);
      g_fail++;
    }
    expect(before.rank == 256 && before.lexical == 3 && before.hamming == 0 &&
               before.lex_w == ranked.lex_w,
           "marked leaves the law rank");
    ng_motive_rank("you lied", &after);
    expect(after.plate == NG_PLATE_CONFESS && after.lexical == 0,
           "marked leaves you lied");
    ng_motive_rank("", &after);
    expect(after.rank == 0, "marked leaves an empty query at rank 0");
    unlink(path);
  }

  {
    const char *path = "/tmp/nb-motive-labrank.pairs";
    const char *dirp = "/tmp/nb-motive-labrank.dir";
    const char *keep = "keep free energy flowing";
    const char *pair = "pair vote then ship the step";
    const char *law = "read the law and prove";
    const char *audit_c = "audit the last code change";
    const char *peer = "prompt a peer to move";
    const char *ship = "ship fail\tkeep free energy flowing\n";
    const char *audit = "why did the ship fail\taudit the last code change\n";
    const char *padded = "why did the ship fail\tkeep free energy flowing  \n";
    const char *trail = "ship fail\tkeep free energy flowing \n";
    char longp[520];
    FILE *pf;
    ng_motive m, rw, ranked, z;
    struct stat st;
    struct timespec ts[2];
    int rc, learned, marked;

    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    expect(ng_motive_label_rank("ship fail", path, NULL) == -1,
           "a null motive returns -1");
    expect(ng_motive_label_rank(NULL, NULL, NULL) == -1,
           "a null motive and query returns -1");

    memset(&m, 0x5a, sizeof m);
    expect(ng_motive_label_rank(NULL, path, &m) == -1, "a null query returns -1");
    memset(&z, 0, sizeof z);
    expect(memcmp(&m, &z, sizeof m) == 0, "a null query zeroes the motive");
    memset(&m, 0x5a, sizeof m);
    expect(ng_motive_label_rank("", path, &m) == -1, "an empty query returns -1");
    expect(memcmp(&m, &z, sizeof m) == 0, "an empty query zeroes the motive");
    memset(&m, 0x5a, sizeof m);
    expect(ng_motive_label_rank("ship fail", NULL, &m) == -1, "a null path returns -1");
    expect(memcmp(&m, &z, sizeof m) == 0, "a null path zeroes the motive");
    memset(&m, 0x5a, sizeof m);
    expect(ng_motive_label_rank("ship fail", "", &m) == -1, "an empty path returns -1");
    expect(memcmp(&m, &z, sizeof m) == 0, "an empty path zeroes the motive");
    memset(longp, 'p', 512);
    longp[512] = 0;
    expect(strlen(longp) == 512, "the long labrank path is 512 bytes");
    memset(&m, 0x5a, sizeof m);
    expect(ng_motive_label_rank("ship fail", longp, &m) == -1,
           "a labrank path of 512 bytes returns -1");
    expect(memcmp(&m, &z, sizeof m) == 0, "a long labrank path zeroes the motive");
    memset(&m, 0x5a, sizeof m);
    expect(ng_motive_label_rank("ship fail", "/tmp/nb-motive-labrank-missing", &m) == -1,
           "a missing labrank file returns -1");
    expect(memcmp(&m, &z, sizeof m) == 0, "a missing labrank file zeroes the motive");
    unlink(dirp);
    rmdir(dirp);
    expect(mkdir(dirp, 0700) == 0, "labrank directory creates");
    memset(&m, 0x5a, sizeof m);
    expect(ng_motive_label_rank("ship fail", dirp, &m) == -1,
           "a non-regular labrank path returns -1");
    expect(memcmp(&m, &z, sizeof m) == 0, "a non-regular labrank path zeroes the motive");
    rmdir(dirp);

    pf = fopen(path, "w");
    expect(pf != NULL, "labrank empty file opens");
    if (pf) {
      fputs("# note\n\n", pf);
      fclose(pf);
    }
    memset(&m, 0x5a, sizeof m);
    expect(ng_motive_label_rank("ship fail", path, &m) == -1,
           "a labrank file with no pair returns -1");
    expect(memcmp(&m, &z, sizeof m) == 0, "no labrank pair zeroes the motive");

    pf = fopen(path, "w");
    expect(pf != NULL, "labrank ship file opens");
    if (pf) {
      fputs(ship, pf);
      fclose(pf);
    }
    rc = ng_motive_label_rank("ship fail", path, &m);
    if (!(rc == 0 && strcmp(m.concept, keep) == 0 && m.lexical == 2 &&
          m.hamming == 22 && m.rank == 170 && m.lex_w == 64)) {
      fprintf(stderr,
              "FAIL labrank ship rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }
    expect(m.plate == NG_PLATE_OBSERVE && m.forward == 1 && strcmp(m.line, keep) == 0,
           "ship fail stays a forward observe line");
    rc = ng_motive_label_rank(law, path, &m);
    if (!(rc == 0 && strcmp(m.concept, law) == 0 && m.lexical == 3 &&
          m.hamming == 0 && m.rank == 256 && m.lex_w == 64)) {
      fprintf(stderr,
              "FAIL labrank law rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }
    expect(getenv("NANOBOT_MOTIVE_LEARN") == NULL, "labrank leaves LEARN unset");
    ng_motive_rank("ship fail", &ranked);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lex_w == 64,
           "labrank leaves ship fail on the pair line");
    setenv("NANOBOT_MOTIVE_LEARN", "/tmp/nb-motive-labrank-missing", 1);
    setenv("NANOBOT_MOTIVE_LEX_W", "1", 1);
    rc = ng_motive_label_rank("ship fail", path, &m);
    if (!(rc == 0 && strcmp(m.concept, keep) == 0 && m.lexical == 2 &&
          m.hamming == 22 && m.rank == 170 && m.lex_w == 64)) {
      fprintf(stderr,
              "FAIL labrank env rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }
    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    learned = ng_motive_learn_hits(path);
    marked = ng_motive_label_hits(path);
    if (learned != 0 || marked != 1) {
      fprintf(stderr, "FAIL labrank counts learn=%d label=%d\n", learned, marked);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "labrank audit file opens");
    if (pf) {
      fputs(audit, pf);
      fclose(pf);
    }
    ng_motive_rank_w("why did the ship fail", &rw, 0);
    rc = ng_motive_label_rank("why did the ship fail", path, &m);
    if (!(rc == 0 && m.lex_w == 0 && strcmp(m.concept, audit_c) == 0 &&
          m.rank == rw.rank && m.hamming == rw.hamming && m.lexical == 4 &&
          m.lexical == rw.lexical + 4 && m.plate == rw.plate &&
          m.forward == rw.forward && strcmp(m.line, rw.line) == 0)) {
      fprintf(stderr,
              "FAIL labrank audit rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d "
              "rw lex=%d ham=%d rank=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w, rw.lexical,
              rw.hamming, rw.rank);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "labrank trail file opens");
    if (pf) {
      fputs(trail, pf);
      fclose(pf);
    }
    ng_motive_rank_w("ship fail", &rw, 64);
    rc = ng_motive_label_rank("ship fail", path, &m);
    if (!(rc == 0 && strcmp(m.concept, pair) == 0 && m.lexical == 1 &&
          m.hamming == 23 && m.rank == 105 && m.lex_w == 64 &&
          m.rank == rw.rank && m.lexical == rw.lexical && m.hamming == rw.hamming &&
          strcmp(m.concept, rw.concept) == 0 && m.plate == rw.plate &&
          m.forward == rw.forward && strcmp(m.line, rw.line) == 0)) {
      fprintf(stderr,
              "FAIL labrank trail rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "labrank confess file opens");
    if (pf) {
      fputs(ship, pf);
      fclose(pf);
    }
    ng_motive_rank_w("you lied", &rw, 64);
    rc = ng_motive_label_rank("you lied", path, &m);
    if (!(rc == 0 && m.plate == NG_PLATE_CONFESS && m.lexical == 0 &&
          m.lex_w == 64 && m.rank == rw.rank && m.hamming == rw.hamming &&
          m.plate == rw.plate && strcmp(m.concept, rw.concept) == 0 &&
          strcmp(m.line, rw.line) == 0)) {
      fprintf(stderr,
              "FAIL labrank lied rc=%d plate=%d concept=%s lex=%d ham=%d rank=%d "
              "lex_w=%d\n",
              rc, m.plate, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }
    ng_motive_rank_w("wipe", &rw, 64);
    rc = ng_motive_label_rank("wipe", path, &m);
    if (!(rc == 0 && m.forward == 0 && m.plate == NG_PLATE_PROTECT &&
          strcmp(m.line, "keep free energy flowing") == 0 && m.lex_w == 64 &&
          m.rank == rw.rank && m.lexical == rw.lexical && m.hamming == rw.hamming &&
          m.plate == rw.plate && strcmp(m.concept, rw.concept) == 0 &&
          strcmp(m.line, rw.line) == 0)) {
      fprintf(stderr,
              "FAIL labrank wipe rc=%d plate=%d fwd=%d concept=%s line=%s lex=%d "
              "ham=%d rank=%d lex_w=%d\n",
              rc, m.plate, m.forward, m.concept, m.line, m.lexical, m.hamming,
              m.rank, m.lex_w);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "labrank dots file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fclose(pf);
    }
    rc = ng_motive_label_rank("...", path, &m);
    if (!(rc == 0 && m.rank == 0 && m.lexical == 0 && m.hamming == 0 &&
          m.lex_w == 64 && m.plate == NG_PLATE_OBSERVE && m.forward == 1 &&
          strcmp(m.concept, peer) == 0 && strcmp(m.line, peer) == 0)) {
      fprintf(stderr,
              "FAIL labrank dots rc=%d plate=%d concept=%s line=%s lex=%d ham=%d "
              "rank=%d lex_w=%d\n",
              rc, m.plate, m.concept, m.line, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "labrank cache file opens");
    if (pf) {
      fputs(audit, pf);
      fclose(pf);
    }
    expect(strlen(audit) == strlen(padded), "labrank cache probe stays the same size");
    expect(ng_motive_label_rank("why did the ship fail", path, &m) == 0,
           "the audit pair fills one motive");
    expect(stat(path, &st) == 0 && st.st_size == (off_t)strlen(audit),
           "labrank pairs stat");
    ts[0].tv_sec = st.st_atim.tv_sec;
    ts[0].tv_nsec = st.st_atim.tv_nsec;
    ts[1].tv_sec = st.st_mtim.tv_sec;
    ts[1].tv_nsec = st.st_mtim.tv_nsec;
    pf = fopen(path, "w");
    expect(pf != NULL, "labrank cache probe opens");
    if (pf) {
      fputs(padded, pf);
      fclose(pf);
    }
    expect(utimensat(AT_FDCWD, path, ts, 0) == 0, "labrank cache probe restores mtime");
    setenv("NANOBOT_MOTIVE_LEARN", path, 1);
    ng_motive_rank("why did the ship fail", &ranked);
    expect(ranked.lex_w == 64 && strcmp(ranked.concept, pair) == 0,
           "labrank does not write the learn cache");
    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");

    pf = fopen(path, "w");
    expect(pf != NULL, "labrank sealed file opens");
    if (pf) {
      fputs(ship, pf);
      fclose(pf);
    }
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    setenv("NANOBOT_MOTIVE_LEARN", path, 1);
    ng_motive_rank("ship fail", &ranked);
    if (!(strcmp(ranked.concept, keep) == 0 && ranked.lexical == 2 &&
          ranked.hamming == 22 && ranked.rank == 170 && ranked.lex_w == 64)) {
      fprintf(stderr,
              "FAIL labrank sealed concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              ranked.concept, ranked.lexical, ranked.hamming, ranked.rank,
              ranked.lex_w);
      g_fail++;
    }
    ng_motive_rank_w("ship fail", &ranked, 64);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lexical == 1 && ranked.hamming == 23 && ranked.lex_w == 64,
           "weight 64 still returns the pair line");
    unsetenv("NANOBOT_MOTIVE_LEARN");
    ng_motive_rank("ship fail", &ranked);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lex_w == 64,
           "unset learn still leaves the pair line");
    ng_motive_rank(law, &ranked);
    if (!(strcmp(ranked.concept, law) == 0 && ranked.rank == 256 &&
          ranked.lexical == 3 && ranked.hamming == 0 && ranked.lex_w == 64)) {
      fprintf(stderr,
              "FAIL labrank sealed law concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              ranked.concept, ranked.lexical, ranked.hamming, ranked.rank,
              ranked.lex_w);
      g_fail++;
    }
    learned = ng_motive_learn_hits(path);
    marked = ng_motive_label_hits(path);
    if (learned != 0 || marked != 1) {
      fprintf(stderr, "FAIL labrank sealed counts learn=%d label=%d\n", learned,
              marked);
      g_fail++;
    }
    ng_motive_rank("you lied", &ranked);
    expect(ranked.plate == NG_PLATE_CONFESS && ranked.lexical == 0,
           "labrank leaves you lied");
    ng_motive_rank("", &ranked);
    expect(ranked.rank == 0, "labrank leaves an empty query at rank 0");
    unlink(path);
  }

  {
    const char *path = "/tmp/nb-motive-margin.pairs";
    const char *dirp = "/tmp/nb-motive-margin.dir";
    const char *keep = "keep free energy flowing";
    const char *pair = "pair vote then ship the step";
    const char *law = "read the law and prove";
    const char *audit_c = "audit the last code change";
    const char *peer = "prompt a peer to move";
    const char *ship = "ship fail\tkeep free energy flowing\n";
    const char *audit = "why did the ship fail\taudit the last code change\n";
    const char *padded = "why did the ship fail\tkeep free energy flowing  \n";
    const char *trail = "ship fail\tkeep free energy flowing \n";
    char longp[520];
    FILE *pf;
    ng_motive m, ranked;
    struct stat st;
    struct timespec ts[2];
    int rc, margin, learned, marked;

    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    expect(ng_motive_label_margin(NULL, path) == -1, "a null query margin is -1");
    expect(ng_motive_label_margin("", path) == -1, "an empty query margin is -1");
    expect(ng_motive_label_margin("ship fail", NULL) == -1, "a null path margin is -1");
    expect(ng_motive_label_margin("ship fail", "") == -1, "an empty path margin is -1");
    memset(longp, 'm', 512);
    longp[512] = 0;
    expect(strlen(longp) == 512, "the long margin path is 512 bytes");
    expect(ng_motive_label_margin("ship fail", longp) == -1,
           "a margin path of 512 bytes returns -1");
    expect(ng_motive_label_margin("ship fail", "/tmp/nb-motive-margin-missing") == -1,
           "a missing margin file returns -1");
    unlink(dirp);
    rmdir(dirp);
    expect(mkdir(dirp, 0700) == 0, "margin directory creates");
    expect(ng_motive_label_margin("ship fail", dirp) == -1,
           "a non-regular margin path returns -1");
    rmdir(dirp);

    pf = fopen(path, "w");
    expect(pf != NULL, "margin empty file opens");
    if (pf) {
      fputs("# note\n\n", pf);
      fclose(pf);
    }
    expect(ng_motive_label_margin("ship fail", path) == -1,
           "a margin file with no pair returns -1");
    expect(ng_motive_label_margin("...", path) == -1,
           "an empty norm with no pair returns -1");

    pf = fopen(path, "w");
    expect(pf != NULL, "margin ship file opens");
    if (pf) {
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    if (margin != 65) {
      fprintf(stderr, "FAIL margin ship %d\n", margin);
      g_fail++;
    }
    setenv("NANOBOT_MOTIVE_LEARN", "/tmp/nb-motive-margin-missing", 1);
    setenv("NANOBOT_MOTIVE_LEX_W", "1", 1);
    margin = ng_motive_label_margin("ship fail", path);
    if (margin != 65) {
      fprintf(stderr, "FAIL margin env %d\n", margin);
      g_fail++;
    }
    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    margin = ng_motive_label_margin(law, path);
    if (margin <= 0) {
      fprintf(stderr, "FAIL margin law %d\n", margin);
      g_fail++;
    }
    rc = ng_motive_label_rank(law, path, &m);
    if (!(rc == 0 && strcmp(m.concept, law) == 0 && m.lexical == 3 &&
          m.hamming == 0 && m.rank == 256 && m.lex_w == 64)) {
      fprintf(stderr,
              "FAIL margin law rank rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }
    expect(getenv("NANOBOT_MOTIVE_LEARN") == NULL, "margin leaves LEARN unset");
    ng_motive_rank("ship fail", &ranked);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lexical == 1 && ranked.hamming == 23 && ranked.lex_w == 64,
           "margin leaves ship fail on the pair line");

    pf = fopen(path, "w");
    expect(pf != NULL, "margin audit file opens");
    if (pf) {
      fputs(audit, pf);
      fclose(pf);
    }
    rc = ng_motive_label_rank("why did the ship fail", path, &m);
    if (!(rc == 0 && strcmp(m.concept, audit_c) == 0 && m.lexical == 4 &&
          m.hamming == 18 && m.rank == 46 && m.lex_w == 0)) {
      fprintf(stderr,
              "FAIL margin audit rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }
    margin = ng_motive_label_margin("why did the ship fail", path);
    if (margin < 0) {
      fprintf(stderr, "FAIL margin audit margin %d\n", margin);
      g_fail++;
    }
    expect(strlen(audit) == strlen(padded), "margin cache probe stays the same size");
    expect(stat(path, &st) == 0 && st.st_size == (off_t)strlen(audit),
           "margin pairs stat");
    ts[0].tv_sec = st.st_atim.tv_sec;
    ts[0].tv_nsec = st.st_atim.tv_nsec;
    ts[1].tv_sec = st.st_mtim.tv_sec;
    ts[1].tv_nsec = st.st_mtim.tv_nsec;
    pf = fopen(path, "w");
    expect(pf != NULL, "margin cache probe opens");
    if (pf) {
      fputs(padded, pf);
      fclose(pf);
    }
    expect(utimensat(AT_FDCWD, path, ts, 0) == 0, "margin cache probe restores mtime");
    setenv("NANOBOT_MOTIVE_LEARN", path, 1);
    ng_motive_rank("why did the ship fail", &ranked);
    expect(ranked.lex_w == 64 && strcmp(ranked.concept, pair) == 0,
           "margin does not write the learn cache");
    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");

    pf = fopen(path, "w");
    expect(pf != NULL, "margin trail file opens");
    if (pf) {
      fputs(trail, pf);
      fclose(pf);
    }
    rc = ng_motive_label_rank("ship fail", path, &m);
    if (!(rc == 0 && strcmp(m.concept, pair) == 0 && m.lexical == 1 &&
          m.hamming == 23 && m.rank == 105 && m.lex_w == 64)) {
      fprintf(stderr,
              "FAIL margin trail rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "margin dots file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fclose(pf);
    }
    margin = ng_motive_label_margin("...", path);
    if (margin != 0) {
      fprintf(stderr, "FAIL margin dots %d\n", margin);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "margin sealed file opens");
    if (pf) {
      fputs(ship, pf);
      fclose(pf);
    }
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    setenv("NANOBOT_MOTIVE_LEARN", path, 1);
    ng_motive_rank("ship fail", &ranked);
    if (!(strcmp(ranked.concept, keep) == 0 && ranked.lexical == 2 &&
          ranked.hamming == 22 && ranked.rank == 170 && ranked.lex_w == 64)) {
      fprintf(stderr,
              "FAIL margin sealed concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              ranked.concept, ranked.lexical, ranked.hamming, ranked.rank,
              ranked.lex_w);
      g_fail++;
    }
    ng_motive_rank_w("ship fail", &ranked, 64);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lexical == 1 && ranked.hamming == 23 && ranked.lex_w == 64,
           "weight 64 still returns the pair line");
    unsetenv("NANOBOT_MOTIVE_LEARN");
    ng_motive_rank("ship fail", &ranked);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lex_w == 64,
           "unset learn still leaves the pair line");
    ng_motive_rank(law, &ranked);
    if (!(strcmp(ranked.concept, law) == 0 && ranked.rank == 256 &&
          ranked.lexical == 3 && ranked.hamming == 0 && ranked.lex_w == 64)) {
      fprintf(stderr,
              "FAIL margin sealed law concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              ranked.concept, ranked.lexical, ranked.hamming, ranked.rank,
              ranked.lex_w);
      g_fail++;
    }
    learned = ng_motive_learn_hits(path);
    marked = ng_motive_label_hits(path);
    if (learned != 0 || marked != 1) {
      fprintf(stderr, "FAIL margin sealed counts learn=%d label=%d\n", learned,
              marked);
      g_fail++;
    }
    ng_motive_rank("you lied", &ranked);
    expect(ranked.plate == NG_PLATE_CONFESS && ranked.lexical == 0,
           "margin leaves you lied");
    ng_motive_rank("", &ranked);
    expect(ranked.rank == 0, "margin leaves an empty query at rank 0");
    unlink(path);
  }

  {
    const char *path = "/tmp/nb-motive-mcount.pairs";
    const char *dirp = "/tmp/nb-motive-mcount.dir";
    const char *keep = "keep free energy flowing";
    const char *pair = "pair vote then ship the step";
    const char *law = "read the law and prove";
    const char *audit_c = "audit the last code change";
    const char *peer = "prompt a peer to move";
    const char *ship = "ship fail\tkeep free energy flowing\n";
    const char *audit = "why did the ship fail\taudit the last code change\n";
    const char *padded = "why did the ship fail\tkeep free energy flowing  \n";
    const char *trail = "ship fail\tkeep free energy flowing \n";
    char longp[520];
    char qbuf[200];
    char cbuf[96];
    char blob[16384];
    char big[420];
    FILE *pf;
    ng_motive m, ranked;
    struct stat st;
    struct timespec ts[2];
    int rc, margin, counted, learned, marked;

    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    expect(ng_motive_margin_hits(NULL) == -1, "a null mcount path counts nothing");
    expect(ng_motive_margin_hits("") == -1, "an empty mcount path counts nothing");
    memset(longp, 'm', 512);
    longp[512] = 0;
    expect(strlen(longp) == 512, "the long mcount path is 512 bytes");
    expect(ng_motive_margin_hits(longp) == -1,
           "an mcount path of 512 bytes returns -1");
    expect(ng_motive_margin_hits("/tmp/nb-motive-mcount-missing") == -1,
           "a missing mcount file returns -1");
    unlink(dirp);
    rmdir(dirp);
    expect(mkdir(dirp, 0700) == 0, "mcount directory creates");
    expect(ng_motive_margin_hits(dirp) == -1,
           "a non-regular mcount path returns -1");
    rmdir(dirp);

    pf = fopen(path, "w");
    expect(pf != NULL, "mcount empty file opens");
    if (pf) {
      fputs("# note\n\n", pf);
      fclose(pf);
    }
    expect(ng_motive_margin_hits(path) == -1,
           "an mcount file with no pair returns -1");

    memset(qbuf, 'q', 192);
    qbuf[192] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mcount overlong query file opens");
    if (pf) {
      fprintf(pf, "%s\t%s\n", qbuf, keep);
      fclose(pf);
    }
    expect(ng_motive_margin_hits(path) == -1,
           "an mcount query of 192 bytes is not a pair");
    qbuf[191] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mcount bounded query file opens");
    if (pf) {
      fprintf(pf, "%s\t%s\n", qbuf, keep);
      fclose(pf);
    }
    margin = ng_motive_label_margin(qbuf, path);
    counted = ng_motive_margin_hits(path);
    if (margin < 0 || counted != (margin > 0 ? 1 : 0)) {
      fprintf(stderr, "FAIL mcount bounded margin=%d hits=%d\n", margin, counted);
      g_fail++;
    }

    memset(cbuf, 'c', 80);
    cbuf[80] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mcount overlong catalog file opens");
    if (pf) {
      fprintf(pf, "ship fail\t%s\n", cbuf);
      fclose(pf);
    }
    expect(ng_motive_margin_hits(path) == -1,
           "an mcount catalog line of 80 bytes is not a pair");
    cbuf[79] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mcount bounded catalog file opens");
    if (pf) {
      fprintf(pf, "ship fail\t%s\n", cbuf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    if (margin < 0 || counted != (margin > 0 ? 1 : 0)) {
      fprintf(stderr, "FAIL mcount catalog margin=%d hits=%d\n", margin, counted);
      g_fail++;
    }

    memset(blob, 'x', sizeof blob);
    pf = fopen(path, "w");
    expect(pf != NULL, "mcount budget file opens");
    if (pf) {
      fwrite(blob, 1, sizeof blob, pf);
      fputc('\n', pf);
      fputs(ship, pf);
      fclose(pf);
    }
    expect(ng_motive_margin_hits(path) == -1,
           "an mcount pair past the 16384 byte budget is not read");

    memset(big, 'z', 400);
    big[400] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mcount overlong line file opens");
    if (pf) {
      fprintf(pf, "%s\n", big);
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    if (margin != 65 || counted != 1) {
      fprintf(stderr, "FAIL mcount skip margin=%d hits=%d\n", margin, counted);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mcount ship file opens");
    if (pf) {
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    if (margin != 65 || counted != 1) {
      fprintf(stderr, "FAIL mcount ship margin=%d hits=%d\n", margin, counted);
      g_fail++;
    }
    setenv("NANOBOT_MOTIVE_LEARN", "/tmp/nb-motive-mcount-missing", 1);
    setenv("NANOBOT_MOTIVE_LEX_W", "1", 1);
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    if (margin != 65 || counted != 1) {
      fprintf(stderr, "FAIL mcount env margin=%d hits=%d\n", margin, counted);
      g_fail++;
    }
    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    margin = ng_motive_label_margin(law, path);
    if (margin <= 0) {
      fprintf(stderr, "FAIL mcount law margin %d\n", margin);
      g_fail++;
    }
    rc = ng_motive_label_rank(law, path, &m);
    if (!(rc == 0 && strcmp(m.concept, law) == 0 && m.lexical == 3 &&
          m.hamming == 0 && m.rank == 256 && m.lex_w == 64)) {
      fprintf(stderr,
              "FAIL mcount law rank rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }
    expect(getenv("NANOBOT_MOTIVE_LEARN") == NULL, "mcount leaves LEARN unset");
    ng_motive_rank("ship fail", &ranked);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lexical == 1 && ranked.hamming == 23 && ranked.lex_w == 64,
           "mcount leaves ship fail on the pair line");

    pf = fopen(path, "w");
    expect(pf != NULL, "mcount audit file opens");
    if (pf) {
      fputs(audit, pf);
      fclose(pf);
    }
    rc = ng_motive_label_rank("why did the ship fail", path, &m);
    if (!(rc == 0 && strcmp(m.concept, audit_c) == 0 && m.lexical == 4 &&
          m.hamming == 18 && m.rank == 46 && m.lex_w == 0)) {
      fprintf(stderr,
              "FAIL mcount audit rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }
    margin = ng_motive_label_margin("why did the ship fail", path);
    counted = ng_motive_margin_hits(path);
    if (margin < 0 || (margin > 0 && counted != 1) ||
        (margin == 0 && counted != 0)) {
      fprintf(stderr, "FAIL mcount audit margin=%d hits=%d\n", margin, counted);
      g_fail++;
    }
    expect(strlen(audit) == strlen(padded), "mcount cache probe stays the same size");
    expect(stat(path, &st) == 0 && st.st_size == (off_t)strlen(audit),
           "mcount pairs stat");
    ts[0].tv_sec = st.st_atim.tv_sec;
    ts[0].tv_nsec = st.st_atim.tv_nsec;
    ts[1].tv_sec = st.st_mtim.tv_sec;
    ts[1].tv_nsec = st.st_mtim.tv_nsec;
    pf = fopen(path, "w");
    expect(pf != NULL, "mcount cache probe opens");
    if (pf) {
      fputs(padded, pf);
      fclose(pf);
    }
    expect(utimensat(AT_FDCWD, path, ts, 0) == 0, "mcount cache probe restores mtime");
    setenv("NANOBOT_MOTIVE_LEARN", path, 1);
    counted = ng_motive_margin_hits(path);
    expect(stat(path, &st) == 0 && st.st_mtim.tv_sec == ts[1].tv_sec &&
               st.st_mtim.tv_nsec == ts[1].tv_nsec &&
               st.st_size == (off_t)strlen(padded),
           "mcount does not write the pairs file");
    ng_motive_rank("why did the ship fail", &ranked);
    expect(ranked.lex_w == 64 && strcmp(ranked.concept, pair) == 0,
           "mcount does not write the learn cache");
    if (counted < 0) {
      fprintf(stderr, "FAIL mcount probe hits %d\n", counted);
      g_fail++;
    }
    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");

    pf = fopen(path, "w");
    expect(pf != NULL, "mcount trail file opens");
    if (pf) {
      fputs(trail, pf);
      fclose(pf);
    }
    rc = ng_motive_label_rank("ship fail", path, &m);
    if (!(rc == 0 && strcmp(m.concept, pair) == 0 && m.lexical == 1 &&
          m.hamming == 23 && m.rank == 105 && m.lex_w == 64)) {
      fprintf(stderr,
              "FAIL mcount trail rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    if (margin < 0 || (margin > 0 && counted != 1) ||
        (margin == 0 && counted != 0)) {
      fprintf(stderr, "FAIL mcount trail margin=%d hits=%d\n", margin, counted);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mcount dots file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fclose(pf);
    }
    margin = ng_motive_label_margin("...", path);
    counted = ng_motive_margin_hits(path);
    if (margin != 0 || counted != 0) {
      fprintf(stderr, "FAIL mcount dots margin=%d hits=%d\n", margin, counted);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mcount sealed file opens");
    if (pf) {
      fputs(ship, pf);
      fclose(pf);
    }
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    setenv("NANOBOT_MOTIVE_LEARN", path, 1);
    ng_motive_rank("ship fail", &ranked);
    if (!(strcmp(ranked.concept, keep) == 0 && ranked.lexical == 2 &&
          ranked.hamming == 22 && ranked.rank == 170 && ranked.lex_w == 64)) {
      fprintf(stderr,
              "FAIL mcount sealed concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              ranked.concept, ranked.lexical, ranked.hamming, ranked.rank,
              ranked.lex_w);
      g_fail++;
    }
    ng_motive_rank_w("ship fail", &ranked, 64);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lexical == 1 && ranked.hamming == 23 && ranked.lex_w == 64,
           "weight 64 still returns the pair line");
    unsetenv("NANOBOT_MOTIVE_LEARN");
    ng_motive_rank("ship fail", &ranked);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lex_w == 64,
           "unset learn still leaves the pair line");
    ng_motive_rank(law, &ranked);
    if (!(strcmp(ranked.concept, law) == 0 && ranked.rank == 256 &&
          ranked.lexical == 3 && ranked.hamming == 0 && ranked.lex_w == 64)) {
      fprintf(stderr,
              "FAIL mcount sealed law concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              ranked.concept, ranked.lexical, ranked.hamming, ranked.rank,
              ranked.lex_w);
      g_fail++;
    }
    learned = ng_motive_learn_hits(path);
    marked = ng_motive_label_hits(path);
    counted = ng_motive_margin_hits(path);
    if (learned != 0 || marked != 1 || counted != 1) {
      fprintf(stderr, "FAIL mcount sealed counts learn=%d label=%d hits=%d\n",
              learned, marked, counted);
      g_fail++;
    }
    ng_motive_rank("you lied", &ranked);
    expect(ranked.plate == NG_PLATE_CONFESS && ranked.lexical == 0,
           "mcount leaves you lied");
    ng_motive_rank("", &ranked);
    expect(ranked.rank == 0, "mcount leaves an empty query at rank 0");
    unlink(path);
  }

  {
    const char *path = "/tmp/nb-motive-mmax.pairs";
    const char *dirp = "/tmp/nb-motive-mmax.dir";
    const char *keep = "keep free energy flowing";
    const char *pair = "pair vote then ship the step";
    const char *law = "read the law and prove";
    const char *audit_c = "audit the last code change";
    const char *peer = "prompt a peer to move";
    const char *ship = "ship fail\tkeep free energy flowing\n";
    const char *audit = "why did the ship fail\taudit the last code change\n";
    const char *padded = "why did the ship fail\tkeep free energy flowing  \n";
    const char *trail = "ship fail\tkeep free energy flowing \n";
    char longp[520];
    char qbuf[200];
    char cbuf[96];
    char blob[16384];
    char big[420];
    FILE *pf;
    ng_motive m, ranked;
    struct stat st;
    struct timespec ts[2];
    int rc, margin, counted, learned, marked, greatest, dots_m;

    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    expect(ng_motive_margin_max(NULL) == -1, "a null mmax path returns -1");
    expect(ng_motive_margin_max("") == -1, "an empty mmax path returns -1");
    memset(longp, 'm', 512);
    longp[512] = 0;
    expect(strlen(longp) == 512, "the long mmax path is 512 bytes");
    expect(ng_motive_margin_max(longp) == -1,
           "an mmax path of 512 bytes returns -1");
    expect(ng_motive_margin_max("/tmp/nb-motive-mmax-missing") == -1,
           "a missing mmax file returns -1");
    unlink(dirp);
    rmdir(dirp);
    expect(mkdir(dirp, 0700) == 0, "mmax directory creates");
    expect(ng_motive_margin_max(dirp) == -1,
           "a non-regular mmax path returns -1");
    rmdir(dirp);

    pf = fopen(path, "w");
    expect(pf != NULL, "mmax empty file opens");
    if (pf) {
      fputs("# note\n\n", pf);
      fclose(pf);
    }
    expect(ng_motive_margin_max(path) == -1,
           "an mmax file with no pair returns -1");

    memset(qbuf, 'q', 192);
    qbuf[192] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mmax overlong query file opens");
    if (pf) {
      fprintf(pf, "%s\t%s\n", qbuf, keep);
      fclose(pf);
    }
    expect(ng_motive_margin_max(path) == -1,
           "an mmax query of 192 bytes is not a pair");
    qbuf[191] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mmax bounded query file opens");
    if (pf) {
      fprintf(pf, "%s\t%s\n", qbuf, keep);
      fclose(pf);
    }
    margin = ng_motive_label_margin(qbuf, path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    if (margin < 0 || counted != (margin > 0 ? 1 : 0) || greatest != margin) {
      fprintf(stderr, "FAIL mmax bounded margin=%d hits=%d max=%d\n", margin,
              counted, greatest);
      g_fail++;
    }

    memset(cbuf, 'c', 80);
    cbuf[80] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mmax overlong catalog file opens");
    if (pf) {
      fprintf(pf, "ship fail\t%s\n", cbuf);
      fclose(pf);
    }
    expect(ng_motive_margin_max(path) == -1,
           "an mmax catalog line of 80 bytes is not a pair");
    cbuf[79] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mmax bounded catalog file opens");
    if (pf) {
      fprintf(pf, "ship fail\t%s\n", cbuf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    if (margin < 0 || counted != (margin > 0 ? 1 : 0) || greatest != margin) {
      fprintf(stderr, "FAIL mmax catalog margin=%d hits=%d max=%d\n", margin,
              counted, greatest);
      g_fail++;
    }

    memset(blob, 'x', sizeof blob);
    pf = fopen(path, "w");
    expect(pf != NULL, "mmax budget file opens");
    if (pf) {
      fwrite(blob, 1, sizeof blob, pf);
      fputc('\n', pf);
      fputs(ship, pf);
      fclose(pf);
    }
    expect(ng_motive_margin_max(path) == -1,
           "an mmax pair past the 16384 byte budget is not read");

    memset(big, 'z', 400);
    big[400] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mmax overlong line file opens");
    if (pf) {
      fprintf(pf, "%s\n", big);
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    if (margin != 65 || counted != 1 || greatest != 65) {
      fprintf(stderr, "FAIL mmax skip margin=%d hits=%d max=%d\n", margin,
              counted, greatest);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mmax ship file opens");
    if (pf) {
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    if (margin != 65 || counted != 1 || greatest != 65) {
      fprintf(stderr, "FAIL mmax ship margin=%d hits=%d max=%d\n", margin,
              counted, greatest);
      g_fail++;
    }
    setenv("NANOBOT_MOTIVE_LEARN", "/tmp/nb-motive-mmax-missing", 1);
    setenv("NANOBOT_MOTIVE_LEX_W", "1", 1);
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    if (margin != 65 || counted != 1 || greatest != 65) {
      fprintf(stderr, "FAIL mmax env margin=%d hits=%d max=%d\n", margin,
              counted, greatest);
      g_fail++;
    }
    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    margin = ng_motive_label_margin(law, path);
    if (margin <= 0) {
      fprintf(stderr, "FAIL mmax law margin %d\n", margin);
      g_fail++;
    }
    rc = ng_motive_label_rank(law, path, &m);
    if (!(rc == 0 && strcmp(m.concept, law) == 0 && m.lexical == 3 &&
          m.hamming == 0 && m.rank == 256 && m.lex_w == 64)) {
      fprintf(stderr,
              "FAIL mmax law rank rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }
    expect(getenv("NANOBOT_MOTIVE_LEARN") == NULL, "mmax leaves LEARN unset");
    ng_motive_rank("ship fail", &ranked);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lexical == 1 && ranked.hamming == 23 && ranked.lex_w == 64,
           "mmax leaves ship fail on the pair line");
    greatest = ng_motive_margin_max(path);
    if (greatest != 65) {
      fprintf(stderr, "FAIL mmax ship stays %d\n", greatest);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mmax audit file opens");
    if (pf) {
      fputs(audit, pf);
      fclose(pf);
    }
    rc = ng_motive_label_rank("why did the ship fail", path, &m);
    if (!(rc == 0 && strcmp(m.concept, audit_c) == 0 && m.lexical == 4 &&
          m.hamming == 18 && m.rank == 46 && m.lex_w == 0)) {
      fprintf(stderr,
              "FAIL mmax audit rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }
    margin = ng_motive_label_margin("why did the ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    if (margin < 0 || (margin > 0 && counted != 1) ||
        (margin == 0 && counted != 0) || greatest != margin) {
      fprintf(stderr, "FAIL mmax audit margin=%d hits=%d max=%d\n", margin,
              counted, greatest);
      g_fail++;
    }
    expect(strlen(audit) == strlen(padded), "mmax cache probe stays the same size");
    expect(stat(path, &st) == 0 && st.st_size == (off_t)strlen(audit),
           "mmax pairs stat");
    ts[0].tv_sec = st.st_atim.tv_sec;
    ts[0].tv_nsec = st.st_atim.tv_nsec;
    ts[1].tv_sec = st.st_mtim.tv_sec;
    ts[1].tv_nsec = st.st_mtim.tv_nsec;
    pf = fopen(path, "w");
    expect(pf != NULL, "mmax cache probe opens");
    if (pf) {
      fputs(padded, pf);
      fclose(pf);
    }
    expect(utimensat(AT_FDCWD, path, ts, 0) == 0, "mmax cache probe restores mtime");
    setenv("NANOBOT_MOTIVE_LEARN", path, 1);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    expect(stat(path, &st) == 0 && st.st_mtim.tv_sec == ts[1].tv_sec &&
               st.st_mtim.tv_nsec == ts[1].tv_nsec &&
               st.st_size == (off_t)strlen(padded),
           "mmax does not write the pairs file");
    ng_motive_rank("why did the ship fail", &ranked);
    expect(ranked.lex_w == 64 && strcmp(ranked.concept, pair) == 0,
           "mmax does not write the learn cache");
    margin = ng_motive_label_margin("why did the ship fail", path);
    if (counted < 0 || margin < 0 || greatest != margin) {
      fprintf(stderr, "FAIL mmax probe hits=%d margin=%d max=%d\n", counted,
              margin, greatest);
      g_fail++;
    }
    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");

    pf = fopen(path, "w");
    expect(pf != NULL, "mmax trail file opens");
    if (pf) {
      fputs(trail, pf);
      fclose(pf);
    }
    rc = ng_motive_label_rank("ship fail", path, &m);
    if (!(rc == 0 && strcmp(m.concept, pair) == 0 && m.lexical == 1 &&
          m.hamming == 23 && m.rank == 105 && m.lex_w == 64)) {
      fprintf(stderr,
              "FAIL mmax trail rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    if (margin < 0 || (margin > 0 && counted != 1) ||
        (margin == 0 && counted != 0) || greatest != margin) {
      fprintf(stderr, "FAIL mmax trail margin=%d hits=%d max=%d\n", margin,
              counted, greatest);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mmax dots file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fclose(pf);
    }
    margin = ng_motive_label_margin("...", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    if (margin != 0 || counted != 0 || greatest != 0) {
      fprintf(stderr, "FAIL mmax dots margin=%d hits=%d max=%d\n", margin,
              counted, greatest);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mmax two file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    dots_m = ng_motive_label_margin("...", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    if (dots_m != 0 || margin < 0 ||
        greatest != (margin > dots_m ? margin : dots_m) ||
        (margin > 0 && counted != 1) || (margin == 0 && counted != 0)) {
      fprintf(stderr, "FAIL mmax two ship=%d dots=%d hits=%d max=%d\n", margin,
              dots_m, counted, greatest);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mmax sealed file opens");
    if (pf) {
      fputs(ship, pf);
      fclose(pf);
    }
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    setenv("NANOBOT_MOTIVE_LEARN", path, 1);
    ng_motive_rank("ship fail", &ranked);
    if (!(strcmp(ranked.concept, keep) == 0 && ranked.lexical == 2 &&
          ranked.hamming == 22 && ranked.rank == 170 && ranked.lex_w == 64)) {
      fprintf(stderr,
              "FAIL mmax sealed concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              ranked.concept, ranked.lexical, ranked.hamming, ranked.rank,
              ranked.lex_w);
      g_fail++;
    }
    ng_motive_rank_w("ship fail", &ranked, 64);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lexical == 1 && ranked.hamming == 23 && ranked.lex_w == 64,
           "weight 64 still returns the pair line");
    unsetenv("NANOBOT_MOTIVE_LEARN");
    ng_motive_rank("ship fail", &ranked);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lex_w == 64,
           "unset learn still leaves the pair line");
    ng_motive_rank(law, &ranked);
    if (!(strcmp(ranked.concept, law) == 0 && ranked.rank == 256 &&
          ranked.lexical == 3 && ranked.hamming == 0 && ranked.lex_w == 64)) {
      fprintf(stderr,
              "FAIL mmax sealed law concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              ranked.concept, ranked.lexical, ranked.hamming, ranked.rank,
              ranked.lex_w);
      g_fail++;
    }
    learned = ng_motive_learn_hits(path);
    marked = ng_motive_label_hits(path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    if (learned != 0 || marked != 1 || counted != 1 || greatest != 65) {
      fprintf(stderr,
              "FAIL mmax sealed counts learn=%d label=%d hits=%d max=%d\n",
              learned, marked, counted, greatest);
      g_fail++;
    }
    ng_motive_rank("you lied", &ranked);
    expect(ranked.plate == NG_PLATE_CONFESS && ranked.lexical == 0,
           "mmax leaves you lied");
    ng_motive_rank("", &ranked);
    expect(ranked.rank == 0, "mmax leaves an empty query at rank 0");
    unlink(path);
  }

  {
    const char *path = "/tmp/nb-motive-mtop.pairs";
    const char *dirp = "/tmp/nb-motive-mtop.dir";
    const char *keep = "keep free energy flowing";
    const char *pair = "pair vote then ship the step";
    const char *law = "read the law and prove";
    const char *audit_c = "audit the last code change";
    const char *peer = "prompt a peer to move";
    const char *ship = "ship fail\tkeep free energy flowing\n";
    const char *audit = "why did the ship fail\taudit the last code change\n";
    const char *padded = "why did the ship fail\tkeep free energy flowing  \n";
    const char *trail = "ship fail\tkeep free energy flowing \n";
    char longp[520];
    char qbuf[200];
    char cbuf[96];
    char blob[16384];
    char big[420];
    FILE *pf;
    ng_motive m, ranked;
    struct stat st;
    struct timespec ts[2];
    int rc, margin, counted, learned, marked, greatest, dots_m, top;

    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    expect(ng_motive_margin_top(NULL) == -1, "a null mtop path returns -1");
    expect(ng_motive_margin_top("") == -1, "an empty mtop path returns -1");
    memset(longp, 'm', 512);
    longp[512] = 0;
    expect(strlen(longp) == 512, "the long mtop path is 512 bytes");
    expect(ng_motive_margin_top(longp) == -1,
           "an mtop path of 512 bytes returns -1");
    expect(ng_motive_margin_top("/tmp/nb-motive-mtop-missing") == -1,
           "a missing mtop file returns -1");
    unlink(dirp);
    rmdir(dirp);
    expect(mkdir(dirp, 0700) == 0, "mtop directory creates");
    expect(ng_motive_margin_top(dirp) == -1,
           "a non-regular mtop path returns -1");
    rmdir(dirp);

    pf = fopen(path, "w");
    expect(pf != NULL, "mtop empty file opens");
    if (pf) {
      fputs("# note\n\n", pf);
      fclose(pf);
    }
    expect(ng_motive_margin_top(path) == -1,
           "an mtop file with no pair returns -1");

    memset(qbuf, 'q', 192);
    qbuf[192] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mtop overlong query file opens");
    if (pf) {
      fprintf(pf, "%s\t%s\n", qbuf, keep);
      fclose(pf);
    }
    expect(ng_motive_margin_top(path) == -1,
           "an mtop query of 192 bytes is not a pair");
    qbuf[191] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mtop bounded query file opens");
    if (pf) {
      fprintf(pf, "%s\t%s\n", qbuf, keep);
      fclose(pf);
    }
    margin = ng_motive_label_margin(qbuf, path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    if (margin < 0 || counted != (margin > 0 ? 1 : 0) || greatest != margin ||
        top != 1) {
      fprintf(stderr, "FAIL mtop bounded margin=%d hits=%d max=%d top=%d\n",
              margin, counted, greatest, top);
      g_fail++;
    }

    memset(cbuf, 'c', 80);
    cbuf[80] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mtop overlong catalog file opens");
    if (pf) {
      fprintf(pf, "ship fail\t%s\n", cbuf);
      fclose(pf);
    }
    expect(ng_motive_margin_top(path) == -1,
           "an mtop catalog line of 80 bytes is not a pair");
    cbuf[79] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mtop bounded catalog file opens");
    if (pf) {
      fprintf(pf, "ship fail\t%s\n", cbuf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    if (margin < 0 || counted != (margin > 0 ? 1 : 0) || greatest != margin ||
        top != 1) {
      fprintf(stderr, "FAIL mtop catalog margin=%d hits=%d max=%d top=%d\n",
              margin, counted, greatest, top);
      g_fail++;
    }

    memset(blob, 'x', sizeof blob);
    pf = fopen(path, "w");
    expect(pf != NULL, "mtop budget file opens");
    if (pf) {
      fwrite(blob, 1, sizeof blob, pf);
      fputc('\n', pf);
      fputs(ship, pf);
      fclose(pf);
    }
    expect(ng_motive_margin_top(path) == -1,
           "an mtop pair past the 16384 byte budget is not read");

    memset(big, 'z', 400);
    big[400] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mtop overlong line file opens");
    if (pf) {
      fprintf(pf, "%s\n", big);
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    if (margin != 65 || counted != 1 || greatest != 65 || top != 1) {
      fprintf(stderr, "FAIL mtop skip margin=%d hits=%d max=%d top=%d\n",
              margin, counted, greatest, top);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mtop ship file opens");
    if (pf) {
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    if (margin != 65 || counted != 1 || greatest != 65 || top != 1) {
      fprintf(stderr, "FAIL mtop ship margin=%d hits=%d max=%d top=%d\n",
              margin, counted, greatest, top);
      g_fail++;
    }
    setenv("NANOBOT_MOTIVE_LEARN", "/tmp/nb-motive-mtop-missing", 1);
    setenv("NANOBOT_MOTIVE_LEX_W", "1", 1);
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    if (margin != 65 || counted != 1 || greatest != 65 || top != 1) {
      fprintf(stderr, "FAIL mtop env margin=%d hits=%d max=%d top=%d\n",
              margin, counted, greatest, top);
      g_fail++;
    }
    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    margin = ng_motive_label_margin(law, path);
    if (margin <= 0) {
      fprintf(stderr, "FAIL mtop law margin %d\n", margin);
      g_fail++;
    }
    rc = ng_motive_label_rank(law, path, &m);
    if (!(rc == 0 && strcmp(m.concept, law) == 0 && m.lexical == 3 &&
          m.hamming == 0 && m.rank == 256 && m.lex_w == 64)) {
      fprintf(stderr,
              "FAIL mtop law rank rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }
    expect(getenv("NANOBOT_MOTIVE_LEARN") == NULL, "mtop leaves LEARN unset");
    ng_motive_rank("ship fail", &ranked);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lexical == 1 && ranked.hamming == 23 && ranked.lex_w == 64,
           "mtop leaves ship fail on the pair line");
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    if (greatest != 65 || top != 1) {
      fprintf(stderr, "FAIL mtop ship stays max=%d top=%d\n", greatest, top);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mtop audit file opens");
    if (pf) {
      fputs(audit, pf);
      fclose(pf);
    }
    rc = ng_motive_label_rank("why did the ship fail", path, &m);
    if (!(rc == 0 && strcmp(m.concept, audit_c) == 0 && m.lexical == 4 &&
          m.hamming == 18 && m.rank == 46 && m.lex_w == 0)) {
      fprintf(stderr,
              "FAIL mtop audit rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }
    margin = ng_motive_label_margin("why did the ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    if (margin < 0 || (margin > 0 && counted != 1) ||
        (margin == 0 && counted != 0) || greatest != margin || top != 1) {
      fprintf(stderr, "FAIL mtop audit margin=%d hits=%d max=%d top=%d\n",
              margin, counted, greatest, top);
      g_fail++;
    }
    expect(strlen(audit) == strlen(padded), "mtop cache probe stays the same size");
    expect(stat(path, &st) == 0 && st.st_size == (off_t)strlen(audit),
           "mtop pairs stat");
    ts[0].tv_sec = st.st_atim.tv_sec;
    ts[0].tv_nsec = st.st_atim.tv_nsec;
    ts[1].tv_sec = st.st_mtim.tv_sec;
    ts[1].tv_nsec = st.st_mtim.tv_nsec;
    pf = fopen(path, "w");
    expect(pf != NULL, "mtop cache probe opens");
    if (pf) {
      fputs(padded, pf);
      fclose(pf);
    }
    expect(utimensat(AT_FDCWD, path, ts, 0) == 0, "mtop cache probe restores mtime");
    setenv("NANOBOT_MOTIVE_LEARN", path, 1);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    expect(stat(path, &st) == 0 && st.st_mtim.tv_sec == ts[1].tv_sec &&
               st.st_mtim.tv_nsec == ts[1].tv_nsec &&
               st.st_size == (off_t)strlen(padded),
           "mtop does not write the pairs file");
    ng_motive_rank("why did the ship fail", &ranked);
    expect(ranked.lex_w == 64 && strcmp(ranked.concept, pair) == 0,
           "mtop does not write the learn cache");
    margin = ng_motive_label_margin("why did the ship fail", path);
    if (counted < 0 || margin < 0 || greatest != margin || top != 1) {
      fprintf(stderr, "FAIL mtop probe hits=%d margin=%d max=%d top=%d\n",
              counted, margin, greatest, top);
      g_fail++;
    }
    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");

    pf = fopen(path, "w");
    expect(pf != NULL, "mtop trail file opens");
    if (pf) {
      fputs(trail, pf);
      fclose(pf);
    }
    rc = ng_motive_label_rank("ship fail", path, &m);
    if (!(rc == 0 && strcmp(m.concept, pair) == 0 && m.lexical == 1 &&
          m.hamming == 23 && m.rank == 105 && m.lex_w == 64)) {
      fprintf(stderr,
              "FAIL mtop trail rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    if (margin < 0 || (margin > 0 && counted != 1) ||
        (margin == 0 && counted != 0) || greatest != margin || top != 1) {
      fprintf(stderr, "FAIL mtop trail margin=%d hits=%d max=%d top=%d\n",
              margin, counted, greatest, top);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mtop dots file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fclose(pf);
    }
    margin = ng_motive_label_margin("...", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    if (margin != 0 || counted != 0 || greatest != 0 || top != 1) {
      fprintf(stderr, "FAIL mtop dots margin=%d hits=%d max=%d top=%d\n",
              margin, counted, greatest, top);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mtop two file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    dots_m = ng_motive_label_margin("...", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    if (dots_m != 0 || margin < 0 ||
        greatest != (margin > dots_m ? margin : dots_m) ||
        (margin > 0 && counted != 1) || (margin == 0 && counted != 0) ||
        top != (margin == 0 ? 2 : 1)) {
      fprintf(stderr, "FAIL mtop two ship=%d dots=%d hits=%d max=%d top=%d\n",
              margin, dots_m, counted, greatest, top);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mtop two-zero file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fprintf(pf, "...\t%s\n", peer);
      fclose(pf);
    }
    margin = ng_motive_label_margin("...", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    if (margin != 0 || counted != 0 || greatest != 0 || top != 2) {
      fprintf(stderr, "FAIL mtop two-zero margin=%d hits=%d max=%d top=%d\n",
              margin, counted, greatest, top);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mtop two-ship file opens");
    if (pf) {
      fputs(ship, pf);
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    if (margin < 0 || greatest != margin || top != 2 ||
        counted != (margin > 0 ? 2 : 0) ||
        (margin != 1 && top == margin + margin)) {
      fprintf(stderr, "FAIL mtop two-ship margin=%d hits=%d max=%d top=%d\n",
              margin, counted, greatest, top);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mtop sealed file opens");
    if (pf) {
      fputs(ship, pf);
      fclose(pf);
    }
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    setenv("NANOBOT_MOTIVE_LEARN", path, 1);
    ng_motive_rank("ship fail", &ranked);
    if (!(strcmp(ranked.concept, keep) == 0 && ranked.lexical == 2 &&
          ranked.hamming == 22 && ranked.rank == 170 && ranked.lex_w == 64)) {
      fprintf(stderr,
              "FAIL mtop sealed concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              ranked.concept, ranked.lexical, ranked.hamming, ranked.rank,
              ranked.lex_w);
      g_fail++;
    }
    ng_motive_rank_w("ship fail", &ranked, 64);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lexical == 1 && ranked.hamming == 23 && ranked.lex_w == 64,
           "weight 64 still returns the pair line");
    unsetenv("NANOBOT_MOTIVE_LEARN");
    ng_motive_rank("ship fail", &ranked);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lex_w == 64,
           "unset learn still leaves the pair line");
    ng_motive_rank(law, &ranked);
    if (!(strcmp(ranked.concept, law) == 0 && ranked.rank == 256 &&
          ranked.lexical == 3 && ranked.hamming == 0 && ranked.lex_w == 64)) {
      fprintf(stderr,
              "FAIL mtop sealed law concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              ranked.concept, ranked.lexical, ranked.hamming, ranked.rank,
              ranked.lex_w);
      g_fail++;
    }
    learned = ng_motive_learn_hits(path);
    marked = ng_motive_label_hits(path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    if (learned != 0 || marked != 1 || counted != 1 || greatest != 65 ||
        top != 1) {
      fprintf(stderr,
              "FAIL mtop sealed counts learn=%d label=%d hits=%d max=%d top=%d\n",
              learned, marked, counted, greatest, top);
      g_fail++;
    }
    ng_motive_rank("you lied", &ranked);
    expect(ranked.plate == NG_PLATE_CONFESS && ranked.lexical == 0,
           "mtop leaves you lied");
    ng_motive_rank("", &ranked);
    expect(ranked.rank == 0, "mtop leaves an empty query at rank 0");
    unlink(path);
  }

  {
    const char *path = "/tmp/nb-motive-msum.pairs";
    const char *dirp = "/tmp/nb-motive-msum.dir";
    const char *keep = "keep free energy flowing";
    const char *pair = "pair vote then ship the step";
    const char *law = "read the law and prove";
    const char *audit_c = "audit the last code change";
    const char *peer = "prompt a peer to move";
    const char *ship = "ship fail\tkeep free energy flowing\n";
    const char *audit = "why did the ship fail\taudit the last code change\n";
    const char *padded = "why did the ship fail\tkeep free energy flowing  \n";
    const char *trail = "ship fail\tkeep free energy flowing \n";
    char longp[520];
    char qbuf[200];
    char cbuf[96];
    char blob[16384];
    char big[420];
    FILE *pf;
    ng_motive m, ranked;
    struct stat st;
    struct timespec ts[2];
    int rc, margin, counted, learned, marked, greatest, dots_m, top, sum;

    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    expect(ng_motive_margin_sum(NULL) == -1, "a null msum path returns -1");
    expect(ng_motive_margin_sum("") == -1, "an empty msum path returns -1");
    memset(longp, 'm', 512);
    longp[512] = 0;
    expect(strlen(longp) == 512, "the long msum path is 512 bytes");
    expect(ng_motive_margin_sum(longp) == -1,
           "an msum path of 512 bytes returns -1");
    expect(ng_motive_margin_sum("/tmp/nb-motive-msum-missing") == -1,
           "a missing msum file returns -1");
    unlink(dirp);
    rmdir(dirp);
    expect(mkdir(dirp, 0700) == 0, "msum directory creates");
    expect(ng_motive_margin_sum(dirp) == -1,
           "a non-regular msum path returns -1");
    rmdir(dirp);

    pf = fopen(path, "w");
    expect(pf != NULL, "msum empty file opens");
    if (pf) {
      fputs("# note\n\n", pf);
      fclose(pf);
    }
    expect(ng_motive_margin_sum(path) == -1,
           "an msum file with no pair returns -1");

    memset(qbuf, 'q', 192);
    qbuf[192] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "msum overlong query file opens");
    if (pf) {
      fprintf(pf, "%s\t%s\n", qbuf, keep);
      fclose(pf);
    }
    expect(ng_motive_margin_sum(path) == -1,
           "an msum query of 192 bytes is not a pair");
    qbuf[191] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "msum bounded query file opens");
    if (pf) {
      fprintf(pf, "%s\t%s\n", qbuf, keep);
      fclose(pf);
    }
    margin = ng_motive_label_margin(qbuf, path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    if (margin < 0 || counted != (margin > 0 ? 1 : 0) || greatest != margin ||
        top != 1 || sum != margin) {
      fprintf(stderr,
              "FAIL msum bounded margin=%d hits=%d max=%d top=%d sum=%d\n",
              margin, counted, greatest, top, sum);
      g_fail++;
    }

    memset(cbuf, 'c', 80);
    cbuf[80] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "msum overlong catalog file opens");
    if (pf) {
      fprintf(pf, "ship fail\t%s\n", cbuf);
      fclose(pf);
    }
    expect(ng_motive_margin_sum(path) == -1,
           "an msum catalog line of 80 bytes is not a pair");
    cbuf[79] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "msum bounded catalog file opens");
    if (pf) {
      fprintf(pf, "ship fail\t%s\n", cbuf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    if (margin < 0 || counted != (margin > 0 ? 1 : 0) || greatest != margin ||
        top != 1 || sum != margin) {
      fprintf(stderr,
              "FAIL msum catalog margin=%d hits=%d max=%d top=%d sum=%d\n",
              margin, counted, greatest, top, sum);
      g_fail++;
    }

    memset(blob, 'x', sizeof blob);
    pf = fopen(path, "w");
    expect(pf != NULL, "msum budget file opens");
    if (pf) {
      fwrite(blob, 1, sizeof blob, pf);
      fputc('\n', pf);
      fputs(ship, pf);
      fclose(pf);
    }
    expect(ng_motive_margin_sum(path) == -1,
           "an msum pair past the 16384 byte budget is not read");

    memset(big, 'z', 400);
    big[400] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "msum overlong line file opens");
    if (pf) {
      fprintf(pf, "%s\n", big);
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    if (margin != 65 || counted != 1 || greatest != 65 || top != 1 ||
        sum != 65) {
      fprintf(stderr,
              "FAIL msum skip margin=%d hits=%d max=%d top=%d sum=%d\n",
              margin, counted, greatest, top, sum);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "msum ship file opens");
    if (pf) {
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    if (margin != 65 || counted != 1 || greatest != 65 || top != 1 ||
        sum != 65 || sum != greatest) {
      fprintf(stderr,
              "FAIL msum ship margin=%d hits=%d max=%d top=%d sum=%d\n",
              margin, counted, greatest, top, sum);
      g_fail++;
    }
    setenv("NANOBOT_MOTIVE_LEARN", "/tmp/nb-motive-msum-missing", 1);
    setenv("NANOBOT_MOTIVE_LEX_W", "1", 1);
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    if (margin != 65 || counted != 1 || greatest != 65 || top != 1 ||
        sum != 65) {
      fprintf(stderr, "FAIL msum env margin=%d hits=%d max=%d top=%d sum=%d\n",
              margin, counted, greatest, top, sum);
      g_fail++;
    }
    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    margin = ng_motive_label_margin(law, path);
    if (margin <= 0) {
      fprintf(stderr, "FAIL msum law margin %d\n", margin);
      g_fail++;
    }
    rc = ng_motive_label_rank(law, path, &m);
    if (!(rc == 0 && strcmp(m.concept, law) == 0 && m.lexical == 3 &&
          m.hamming == 0 && m.rank == 256 && m.lex_w == 64)) {
      fprintf(stderr,
              "FAIL msum law rank rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }
    expect(getenv("NANOBOT_MOTIVE_LEARN") == NULL, "msum leaves LEARN unset");
    ng_motive_rank("ship fail", &ranked);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lexical == 1 && ranked.hamming == 23 && ranked.lex_w == 64,
           "msum leaves ship fail on the pair line");
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    if (greatest != 65 || top != 1 || sum != 65) {
      fprintf(stderr, "FAIL msum ship stays max=%d top=%d sum=%d\n", greatest,
              top, sum);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "msum audit file opens");
    if (pf) {
      fputs(audit, pf);
      fclose(pf);
    }
    rc = ng_motive_label_rank("why did the ship fail", path, &m);
    if (!(rc == 0 && strcmp(m.concept, audit_c) == 0 && m.lexical == 4 &&
          m.hamming == 18 && m.rank == 46 && m.lex_w == 0)) {
      fprintf(stderr,
              "FAIL msum audit rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }
    margin = ng_motive_label_margin("why did the ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    if (margin < 0 || (margin > 0 && counted != 1) ||
        (margin == 0 && counted != 0) || greatest != margin || top != 1 ||
        sum != margin) {
      fprintf(stderr,
              "FAIL msum audit margin=%d hits=%d max=%d top=%d sum=%d\n",
              margin, counted, greatest, top, sum);
      g_fail++;
    }
    expect(strlen(audit) == strlen(padded), "msum cache probe stays the same size");
    expect(stat(path, &st) == 0 && st.st_size == (off_t)strlen(audit),
           "msum pairs stat");
    ts[0].tv_sec = st.st_atim.tv_sec;
    ts[0].tv_nsec = st.st_atim.tv_nsec;
    ts[1].tv_sec = st.st_mtim.tv_sec;
    ts[1].tv_nsec = st.st_mtim.tv_nsec;
    pf = fopen(path, "w");
    expect(pf != NULL, "msum cache probe opens");
    if (pf) {
      fputs(padded, pf);
      fclose(pf);
    }
    expect(utimensat(AT_FDCWD, path, ts, 0) == 0, "msum cache probe restores mtime");
    setenv("NANOBOT_MOTIVE_LEARN", path, 1);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    expect(stat(path, &st) == 0 && st.st_mtim.tv_sec == ts[1].tv_sec &&
               st.st_mtim.tv_nsec == ts[1].tv_nsec &&
               st.st_size == (off_t)strlen(padded),
           "msum does not write the pairs file");
    ng_motive_rank("why did the ship fail", &ranked);
    expect(ranked.lex_w == 64 && strcmp(ranked.concept, pair) == 0,
           "msum does not write the learn cache");
    margin = ng_motive_label_margin("why did the ship fail", path);
    if (counted < 0 || margin < 0 || greatest != margin || top != 1 ||
        sum != margin) {
      fprintf(stderr,
              "FAIL msum probe hits=%d margin=%d max=%d top=%d sum=%d\n",
              counted, margin, greatest, top, sum);
      g_fail++;
    }
    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");

    pf = fopen(path, "w");
    expect(pf != NULL, "msum trail file opens");
    if (pf) {
      fputs(trail, pf);
      fclose(pf);
    }
    rc = ng_motive_label_rank("ship fail", path, &m);
    if (!(rc == 0 && strcmp(m.concept, pair) == 0 && m.lexical == 1 &&
          m.hamming == 23 && m.rank == 105 && m.lex_w == 64)) {
      fprintf(stderr,
              "FAIL msum trail rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    if (margin < 0 || (margin > 0 && counted != 1) ||
        (margin == 0 && counted != 0) || greatest != margin || top != 1 ||
        sum != margin) {
      fprintf(stderr,
              "FAIL msum trail margin=%d hits=%d max=%d top=%d sum=%d\n",
              margin, counted, greatest, top, sum);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "msum dots file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fclose(pf);
    }
    margin = ng_motive_label_margin("...", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    if (margin != 0 || counted != 0 || greatest != 0 || top != 1 || sum != 0) {
      fprintf(stderr, "FAIL msum dots margin=%d hits=%d max=%d top=%d sum=%d\n",
              margin, counted, greatest, top, sum);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "msum two file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    dots_m = ng_motive_label_margin("...", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    if (dots_m != 0 || margin < 0 ||
        greatest != (margin > dots_m ? margin : dots_m) ||
        (margin > 0 && counted != 1) || (margin == 0 && counted != 0) ||
        top != (margin == 0 ? 2 : 1) || sum != margin) {
      fprintf(stderr,
              "FAIL msum two ship=%d dots=%d hits=%d max=%d top=%d sum=%d\n",
              margin, dots_m, counted, greatest, top, sum);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "msum two-zero file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fprintf(pf, "...\t%s\n", peer);
      fclose(pf);
    }
    margin = ng_motive_label_margin("...", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    if (margin != 0 || counted != 0 || greatest != 0 || top != 2 || sum != 0) {
      fprintf(stderr,
              "FAIL msum two-zero margin=%d hits=%d max=%d top=%d sum=%d\n",
              margin, counted, greatest, top, sum);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "msum two-ship file opens");
    if (pf) {
      fputs(ship, pf);
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    if (margin < 0 || greatest != margin || top != 2 ||
        counted != (margin > 0 ? 2 : 0) || sum != margin + margin ||
        (margin != 1 && sum == top)) {
      fprintf(stderr,
              "FAIL msum two-ship margin=%d hits=%d max=%d top=%d sum=%d\n",
              margin, counted, greatest, top, sum);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "msum sealed file opens");
    if (pf) {
      fputs(ship, pf);
      fclose(pf);
    }
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    setenv("NANOBOT_MOTIVE_LEARN", path, 1);
    ng_motive_rank("ship fail", &ranked);
    if (!(strcmp(ranked.concept, keep) == 0 && ranked.lexical == 2 &&
          ranked.hamming == 22 && ranked.rank == 170 && ranked.lex_w == 64)) {
      fprintf(stderr,
              "FAIL msum sealed concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              ranked.concept, ranked.lexical, ranked.hamming, ranked.rank,
              ranked.lex_w);
      g_fail++;
    }
    ng_motive_rank_w("ship fail", &ranked, 64);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lexical == 1 && ranked.hamming == 23 && ranked.lex_w == 64,
           "weight 64 still returns the pair line");
    unsetenv("NANOBOT_MOTIVE_LEARN");
    ng_motive_rank("ship fail", &ranked);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lex_w == 64,
           "unset learn still leaves the pair line");
    ng_motive_rank(law, &ranked);
    if (!(strcmp(ranked.concept, law) == 0 && ranked.rank == 256 &&
          ranked.lexical == 3 && ranked.hamming == 0 && ranked.lex_w == 64)) {
      fprintf(stderr,
              "FAIL msum sealed law concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              ranked.concept, ranked.lexical, ranked.hamming, ranked.rank,
              ranked.lex_w);
      g_fail++;
    }
    learned = ng_motive_learn_hits(path);
    marked = ng_motive_label_hits(path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    if (learned != 0 || marked != 1 || counted != 1 || greatest != 65 ||
        top != 1 || sum != 65 || sum != greatest) {
      fprintf(stderr,
              "FAIL msum sealed counts learn=%d label=%d hits=%d max=%d top=%d sum=%d\n",
              learned, marked, counted, greatest, top, sum);
      g_fail++;
    }
    ng_motive_rank("you lied", &ranked);
    expect(ranked.plate == NG_PLATE_CONFESS && ranked.lexical == 0,
           "msum leaves you lied");
    ng_motive_rank("", &ranked);
    expect(ranked.rank == 0, "msum leaves an empty query at rank 0");
    unlink(path);
  }

  {
    const char *path = "/tmp/nb-motive-mmin.pairs";
    const char *dirp = "/tmp/nb-motive-mmin.dir";
    const char *keep = "keep free energy flowing";
    const char *pair = "pair vote then ship the step";
    const char *law = "read the law and prove";
    const char *audit_c = "audit the last code change";
    const char *peer = "prompt a peer to move";
    const char *ship = "ship fail\tkeep free energy flowing\n";
    const char *audit = "why did the ship fail\taudit the last code change\n";
    const char *padded = "why did the ship fail\tkeep free energy flowing  \n";
    const char *trail = "ship fail\tkeep free energy flowing \n";
    char longp[520];
    char qbuf[200];
    char cbuf[96];
    char blob[16384];
    char big[420];
    FILE *pf;
    ng_motive m, ranked;
    struct stat st;
    struct timespec ts[2];
    int rc, margin, counted, learned, marked, greatest, dots_m, top, sum, least;

    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    expect(ng_motive_margin_min(NULL) == -1, "a null mmin path returns -1");
    expect(ng_motive_margin_min("") == -1, "an empty mmin path returns -1");
    memset(longp, 'm', 512);
    longp[512] = 0;
    expect(strlen(longp) == 512, "the long mmin path is 512 bytes");
    expect(ng_motive_margin_min(longp) == -1,
           "an mmin path of 512 bytes returns -1");
    expect(ng_motive_margin_min("/tmp/nb-motive-mmin-missing") == -1,
           "a missing mmin file returns -1");
    unlink(dirp);
    rmdir(dirp);
    expect(mkdir(dirp, 0700) == 0, "mmin directory creates");
    expect(ng_motive_margin_min(dirp) == -1,
           "a non-regular mmin path returns -1");
    rmdir(dirp);

    pf = fopen(path, "w");
    expect(pf != NULL, "mmin empty file opens");
    if (pf) {
      fputs("# note\n\n", pf);
      fclose(pf);
    }
    expect(ng_motive_margin_min(path) == -1,
           "an mmin file with no pair returns -1");

    memset(qbuf, 'q', 192);
    qbuf[192] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mmin overlong query file opens");
    if (pf) {
      fprintf(pf, "%s\t%s\n", qbuf, keep);
      fclose(pf);
    }
    expect(ng_motive_margin_min(path) == -1,
           "an mmin query of 192 bytes is not a pair");
    qbuf[191] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mmin bounded query file opens");
    if (pf) {
      fprintf(pf, "%s\t%s\n", qbuf, keep);
      fclose(pf);
    }
    margin = ng_motive_label_margin(qbuf, path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    if (margin < 0 || counted != (margin > 0 ? 1 : 0) || greatest != margin ||
        top != 1 || sum != margin || least != margin) {
      fprintf(stderr,
              "FAIL mmin bounded margin=%d hits=%d max=%d top=%d sum=%d min=%d\n",
              margin, counted, greatest, top, sum, least);
      g_fail++;
    }

    memset(cbuf, 'c', 80);
    cbuf[80] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mmin overlong catalog file opens");
    if (pf) {
      fprintf(pf, "ship fail\t%s\n", cbuf);
      fclose(pf);
    }
    expect(ng_motive_margin_min(path) == -1,
           "an mmin catalog line of 80 bytes is not a pair");
    cbuf[79] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mmin bounded catalog file opens");
    if (pf) {
      fprintf(pf, "ship fail\t%s\n", cbuf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    if (margin < 0 || counted != (margin > 0 ? 1 : 0) || greatest != margin ||
        top != 1 || sum != margin || least != margin) {
      fprintf(stderr,
              "FAIL mmin catalog margin=%d hits=%d max=%d top=%d sum=%d min=%d\n",
              margin, counted, greatest, top, sum, least);
      g_fail++;
    }

    memset(blob, 'x', sizeof blob);
    pf = fopen(path, "w");
    expect(pf != NULL, "mmin budget file opens");
    if (pf) {
      fwrite(blob, 1, sizeof blob, pf);
      fputc('\n', pf);
      fputs(ship, pf);
      fclose(pf);
    }
    expect(ng_motive_margin_min(path) == -1,
           "an mmin pair past the 16384 byte budget is not read");

    memset(big, 'z', 400);
    big[400] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mmin overlong line file opens");
    if (pf) {
      fprintf(pf, "%s\n", big);
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    if (margin != 65 || counted != 1 || greatest != 65 || top != 1 ||
        sum != 65 || least != 65) {
      fprintf(stderr,
              "FAIL mmin skip margin=%d hits=%d max=%d top=%d sum=%d min=%d\n",
              margin, counted, greatest, top, sum, least);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mmin ship file opens");
    if (pf) {
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    if (margin != 65 || counted != 1 || greatest != 65 || top != 1 ||
        sum != 65 || least != 65 || sum != greatest || least != greatest) {
      fprintf(stderr,
              "FAIL mmin ship margin=%d hits=%d max=%d top=%d sum=%d min=%d\n",
              margin, counted, greatest, top, sum, least);
      g_fail++;
    }
    setenv("NANOBOT_MOTIVE_LEARN", "/tmp/nb-motive-mmin-missing", 1);
    setenv("NANOBOT_MOTIVE_LEX_W", "1", 1);
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    if (margin != 65 || counted != 1 || greatest != 65 || top != 1 ||
        sum != 65 || least != 65) {
      fprintf(stderr, "FAIL mmin env margin=%d hits=%d max=%d top=%d sum=%d min=%d\n",
              margin, counted, greatest, top, sum, least);
      g_fail++;
    }
    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    margin = ng_motive_label_margin(law, path);
    if (margin <= 0) {
      fprintf(stderr, "FAIL mmin law margin %d\n", margin);
      g_fail++;
    }
    rc = ng_motive_label_rank(law, path, &m);
    if (!(rc == 0 && strcmp(m.concept, law) == 0 && m.lexical == 3 &&
          m.hamming == 0 && m.rank == 256 && m.lex_w == 64)) {
      fprintf(stderr,
              "FAIL mmin law rank rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }
    expect(getenv("NANOBOT_MOTIVE_LEARN") == NULL, "mmin leaves LEARN unset");
    ng_motive_rank("ship fail", &ranked);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lexical == 1 && ranked.hamming == 23 && ranked.lex_w == 64,
           "mmin leaves ship fail on the pair line");
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    if (greatest != 65 || top != 1 || sum != 65 || least != 65) {
      fprintf(stderr, "FAIL mmin ship stays max=%d top=%d sum=%d min=%d\n",
              greatest, top, sum, least);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mmin audit file opens");
    if (pf) {
      fputs(audit, pf);
      fclose(pf);
    }
    rc = ng_motive_label_rank("why did the ship fail", path, &m);
    if (!(rc == 0 && strcmp(m.concept, audit_c) == 0 && m.lexical == 4 &&
          m.hamming == 18 && m.rank == 46 && m.lex_w == 0)) {
      fprintf(stderr,
              "FAIL mmin audit rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }
    margin = ng_motive_label_margin("why did the ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    if (margin < 0 || (margin > 0 && counted != 1) ||
        (margin == 0 && counted != 0) || greatest != margin || top != 1 ||
        sum != margin || least != margin) {
      fprintf(stderr,
              "FAIL mmin audit margin=%d hits=%d max=%d top=%d sum=%d min=%d\n",
              margin, counted, greatest, top, sum, least);
      g_fail++;
    }
    expect(strlen(audit) == strlen(padded), "mmin cache probe stays the same size");
    expect(stat(path, &st) == 0 && st.st_size == (off_t)strlen(audit),
           "mmin pairs stat");
    ts[0].tv_sec = st.st_atim.tv_sec;
    ts[0].tv_nsec = st.st_atim.tv_nsec;
    ts[1].tv_sec = st.st_mtim.tv_sec;
    ts[1].tv_nsec = st.st_mtim.tv_nsec;
    pf = fopen(path, "w");
    expect(pf != NULL, "mmin cache probe opens");
    if (pf) {
      fputs(padded, pf);
      fclose(pf);
    }
    expect(utimensat(AT_FDCWD, path, ts, 0) == 0, "mmin cache probe restores mtime");
    setenv("NANOBOT_MOTIVE_LEARN", path, 1);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    expect(stat(path, &st) == 0 && st.st_mtim.tv_sec == ts[1].tv_sec &&
               st.st_mtim.tv_nsec == ts[1].tv_nsec &&
               st.st_size == (off_t)strlen(padded),
           "mmin does not write the pairs file");
    ng_motive_rank("why did the ship fail", &ranked);
    expect(ranked.lex_w == 64 && strcmp(ranked.concept, pair) == 0,
           "mmin does not write the learn cache");
    margin = ng_motive_label_margin("why did the ship fail", path);
    if (counted < 0 || margin < 0 || greatest != margin || top != 1 ||
        sum != margin || least != margin) {
      fprintf(stderr,
              "FAIL mmin probe hits=%d margin=%d max=%d top=%d sum=%d min=%d\n",
              counted, margin, greatest, top, sum, least);
      g_fail++;
    }
    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");

    pf = fopen(path, "w");
    expect(pf != NULL, "mmin trail file opens");
    if (pf) {
      fputs(trail, pf);
      fclose(pf);
    }
    rc = ng_motive_label_rank("ship fail", path, &m);
    if (!(rc == 0 && strcmp(m.concept, pair) == 0 && m.lexical == 1 &&
          m.hamming == 23 && m.rank == 105 && m.lex_w == 64)) {
      fprintf(stderr,
              "FAIL mmin trail rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    if (margin < 0 || (margin > 0 && counted != 1) ||
        (margin == 0 && counted != 0) || greatest != margin || top != 1 ||
        sum != margin || least != margin) {
      fprintf(stderr,
              "FAIL mmin trail margin=%d hits=%d max=%d top=%d sum=%d min=%d\n",
              margin, counted, greatest, top, sum, least);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mmin dots file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fclose(pf);
    }
    margin = ng_motive_label_margin("...", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    if (margin != 0 || counted != 0 || greatest != 0 || top != 1 || sum != 0 ||
        least != 0) {
      fprintf(stderr, "FAIL mmin dots margin=%d hits=%d max=%d top=%d sum=%d min=%d\n",
              margin, counted, greatest, top, sum, least);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mmin two file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    dots_m = ng_motive_label_margin("...", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    if (dots_m != 0 || margin < 0 ||
        greatest != (margin > dots_m ? margin : dots_m) ||
        least != (margin < dots_m ? margin : dots_m) ||
        (margin > 0 && (least != 0 || greatest != margin || least == sum)) ||
        (margin > 0 && counted != 1) || (margin == 0 && counted != 0) ||
        top != (margin == 0 ? 2 : 1) || sum != margin) {
      fprintf(stderr,
              "FAIL mmin two ship=%d dots=%d hits=%d max=%d top=%d sum=%d min=%d\n",
              margin, dots_m, counted, greatest, top, sum, least);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mmin two-zero file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fprintf(pf, "...\t%s\n", peer);
      fclose(pf);
    }
    margin = ng_motive_label_margin("...", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    if (margin != 0 || counted != 0 || greatest != 0 || top != 2 || sum != 0 ||
        least != 0) {
      fprintf(stderr,
              "FAIL mmin two-zero margin=%d hits=%d max=%d top=%d sum=%d min=%d\n",
              margin, counted, greatest, top, sum, least);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mmin two-ship file opens");
    if (pf) {
      fputs(ship, pf);
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    if (margin < 0 || greatest != margin || least != margin || top != 2 ||
        counted != (margin > 0 ? 2 : 0) || sum != margin + margin ||
        (margin != 1 && sum == top)) {
      fprintf(stderr,
              "FAIL mmin two-ship margin=%d hits=%d max=%d top=%d sum=%d min=%d\n",
              margin, counted, greatest, top, sum, least);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mmin sealed file opens");
    if (pf) {
      fputs(ship, pf);
      fclose(pf);
    }
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    setenv("NANOBOT_MOTIVE_LEARN", path, 1);
    ng_motive_rank("ship fail", &ranked);
    if (!(strcmp(ranked.concept, keep) == 0 && ranked.lexical == 2 &&
          ranked.hamming == 22 && ranked.rank == 170 && ranked.lex_w == 64)) {
      fprintf(stderr,
              "FAIL mmin sealed concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              ranked.concept, ranked.lexical, ranked.hamming, ranked.rank,
              ranked.lex_w);
      g_fail++;
    }
    ng_motive_rank_w("ship fail", &ranked, 64);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lexical == 1 && ranked.hamming == 23 && ranked.lex_w == 64,
           "weight 64 still returns the pair line");
    unsetenv("NANOBOT_MOTIVE_LEARN");
    ng_motive_rank("ship fail", &ranked);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lex_w == 64,
           "unset learn still leaves the pair line");
    ng_motive_rank(law, &ranked);
    if (!(strcmp(ranked.concept, law) == 0 && ranked.rank == 256 &&
          ranked.lexical == 3 && ranked.hamming == 0 && ranked.lex_w == 64)) {
      fprintf(stderr,
              "FAIL mmin sealed law concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              ranked.concept, ranked.lexical, ranked.hamming, ranked.rank,
              ranked.lex_w);
      g_fail++;
    }
    learned = ng_motive_learn_hits(path);
    marked = ng_motive_label_hits(path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    if (learned != 0 || marked != 1 || counted != 1 || greatest != 65 ||
        top != 1 || sum != 65 || least != 65 || sum != greatest ||
        least != greatest) {
      fprintf(stderr,
              "FAIL mmin sealed counts learn=%d label=%d hits=%d max=%d top=%d sum=%d min=%d\n",
              learned, marked, counted, greatest, top, sum, least);
      g_fail++;
    }
    ng_motive_rank("you lied", &ranked);
    expect(ranked.plate == NG_PLATE_CONFESS && ranked.lexical == 0,
           "mmin leaves you lied");
    ng_motive_rank("", &ranked);
    expect(ranked.rank == 0, "mmin leaves an empty query at rank 0");
    unlink(path);
  }

  {
    const char *path = "/tmp/nb-motive-munder.pairs";
    const char *dirp = "/tmp/nb-motive-munder.dir";
    const char *keep = "keep free energy flowing";
    const char *pair = "pair vote then ship the step";
    const char *law = "read the law and prove";
    const char *audit_c = "audit the last code change";
    const char *peer = "prompt a peer to move";
    const char *ship = "ship fail\tkeep free energy flowing\n";
    const char *audit = "why did the ship fail\taudit the last code change\n";
    const char *padded = "why did the ship fail\tkeep free energy flowing  \n";
    const char *trail = "ship fail\tkeep free energy flowing \n";
    const char *qs[3];
    char longp[520];
    char qbuf[200];
    char cbuf[96];
    char blob[16384];
    char big[420];
    FILE *pf;
    ng_motive m, ranked;
    struct stat st;
    struct timespec ts[2];
    int rc, margin, counted, learned, marked, greatest, dots_m, top, sum, least, under;
    int ms[3];
    int si, seen_m, best_m, least_m, expect_u, least_n, mid_n;

    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    expect(ng_motive_margin_under(NULL) == -1, "a null munder path returns -1");
    expect(ng_motive_margin_under("") == -1, "an empty munder path returns -1");
    memset(longp, 'm', 512);
    longp[512] = 0;
    expect(strlen(longp) == 512, "the long munder path is 512 bytes");
    expect(ng_motive_margin_under(longp) == -1,
           "an munder path of 512 bytes returns -1");
    expect(ng_motive_margin_under("/tmp/nb-motive-munder-missing") == -1,
           "a missing munder file returns -1");
    unlink(dirp);
    rmdir(dirp);
    expect(mkdir(dirp, 0700) == 0, "munder directory creates");
    expect(ng_motive_margin_under(dirp) == -1,
           "a non-regular munder path returns -1");
    rmdir(dirp);

    pf = fopen(path, "w");
    expect(pf != NULL, "munder empty file opens");
    if (pf) {
      fputs("# note\n\n", pf);
      fclose(pf);
    }
    expect(ng_motive_margin_under(path) == -1,
           "an munder file with no pair returns -1");

    memset(qbuf, 'q', 192);
    qbuf[192] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "munder overlong query file opens");
    if (pf) {
      fprintf(pf, "%s\t%s\n", qbuf, keep);
      fclose(pf);
    }
    expect(ng_motive_margin_under(path) == -1,
           "an munder query of 192 bytes is not a pair");
    qbuf[191] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "munder bounded query file opens");
    if (pf) {
      fprintf(pf, "%s\t%s\n", qbuf, keep);
      fclose(pf);
    }
    margin = ng_motive_label_margin(qbuf, path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    if (margin < 0 || counted != (margin > 0 ? 1 : 0) || greatest != margin ||
        top != 1 || sum != margin || least != margin || under != 0) {
      fprintf(stderr,
              "FAIL munder bounded margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d\n",
              margin, counted, greatest, top, sum, least, under);
      g_fail++;
    }

    memset(cbuf, 'c', 80);
    cbuf[80] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "munder overlong catalog file opens");
    if (pf) {
      fprintf(pf, "ship fail\t%s\n", cbuf);
      fclose(pf);
    }
    expect(ng_motive_margin_under(path) == -1,
           "an munder catalog line of 80 bytes is not a pair");
    cbuf[79] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "munder bounded catalog file opens");
    if (pf) {
      fprintf(pf, "ship fail\t%s\n", cbuf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    if (margin < 0 || counted != (margin > 0 ? 1 : 0) || greatest != margin ||
        top != 1 || sum != margin || least != margin || under != 0) {
      fprintf(stderr,
              "FAIL munder catalog margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d\n",
              margin, counted, greatest, top, sum, least, under);
      g_fail++;
    }

    memset(blob, 'x', sizeof blob);
    pf = fopen(path, "w");
    expect(pf != NULL, "munder budget file opens");
    if (pf) {
      fwrite(blob, 1, sizeof blob, pf);
      fputc('\n', pf);
      fputs(ship, pf);
      fclose(pf);
    }
    expect(ng_motive_margin_under(path) == -1,
           "an munder pair past the 16384 byte budget is not read");

    memset(big, 'z', 400);
    big[400] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "munder overlong line file opens");
    if (pf) {
      fprintf(pf, "%s\n", big);
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    if (margin != 65 || counted != 1 || greatest != 65 || top != 1 ||
        sum != 65 || least != 65 || under != 0) {
      fprintf(stderr,
              "FAIL munder skip margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d\n",
              margin, counted, greatest, top, sum, least, under);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "munder ship file opens");
    if (pf) {
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    if (margin != 65 || counted != 1 || greatest != 65 || top != 1 ||
        sum != 65 || least != 65 || under != 0 || sum != greatest ||
        least != greatest) {
      fprintf(stderr,
              "FAIL munder ship margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d\n",
              margin, counted, greatest, top, sum, least, under);
      g_fail++;
    }
    setenv("NANOBOT_MOTIVE_LEARN", "/tmp/nb-motive-munder-missing", 1);
    setenv("NANOBOT_MOTIVE_LEX_W", "1", 1);
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    if (margin != 65 || counted != 1 || greatest != 65 || top != 1 ||
        sum != 65 || least != 65 || under != 0) {
      fprintf(stderr,
              "FAIL munder env margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d\n",
              margin, counted, greatest, top, sum, least, under);
      g_fail++;
    }
    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    margin = ng_motive_label_margin(law, path);
    if (margin <= 0) {
      fprintf(stderr, "FAIL munder law margin %d\n", margin);
      g_fail++;
    }
    rc = ng_motive_label_rank(law, path, &m);
    if (!(rc == 0 && strcmp(m.concept, law) == 0 && m.lexical == 3 &&
          m.hamming == 0 && m.rank == 256 && m.lex_w == 64)) {
      fprintf(stderr,
              "FAIL munder law rank rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }
    expect(getenv("NANOBOT_MOTIVE_LEARN") == NULL, "munder leaves LEARN unset");
    ng_motive_rank("ship fail", &ranked);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lexical == 1 && ranked.hamming == 23 && ranked.lex_w == 64,
           "munder leaves ship fail on the pair line");
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    if (greatest != 65 || top != 1 || sum != 65 || least != 65 || under != 0) {
      fprintf(stderr, "FAIL munder ship stays max=%d top=%d sum=%d min=%d under=%d\n",
              greatest, top, sum, least, under);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "munder audit file opens");
    if (pf) {
      fputs(audit, pf);
      fclose(pf);
    }
    rc = ng_motive_label_rank("why did the ship fail", path, &m);
    if (!(rc == 0 && strcmp(m.concept, audit_c) == 0 && m.lexical == 4 &&
          m.hamming == 18 && m.rank == 46 && m.lex_w == 0)) {
      fprintf(stderr,
              "FAIL munder audit rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }
    margin = ng_motive_label_margin("why did the ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    if (margin < 0 || (margin > 0 && counted != 1) ||
        (margin == 0 && counted != 0) || greatest != margin || top != 1 ||
        sum != margin || least != margin || under != 0) {
      fprintf(stderr,
              "FAIL munder audit margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d\n",
              margin, counted, greatest, top, sum, least, under);
      g_fail++;
    }
    expect(strlen(audit) == strlen(padded), "munder cache probe stays the same size");
    expect(stat(path, &st) == 0 && st.st_size == (off_t)strlen(audit),
           "munder pairs stat");
    ts[0].tv_sec = st.st_atim.tv_sec;
    ts[0].tv_nsec = st.st_atim.tv_nsec;
    ts[1].tv_sec = st.st_mtim.tv_sec;
    ts[1].tv_nsec = st.st_mtim.tv_nsec;
    pf = fopen(path, "w");
    expect(pf != NULL, "munder cache probe opens");
    if (pf) {
      fputs(padded, pf);
      fclose(pf);
    }
    expect(utimensat(AT_FDCWD, path, ts, 0) == 0, "munder cache probe restores mtime");
    setenv("NANOBOT_MOTIVE_LEARN", path, 1);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    expect(stat(path, &st) == 0 && st.st_mtim.tv_sec == ts[1].tv_sec &&
               st.st_mtim.tv_nsec == ts[1].tv_nsec &&
               st.st_size == (off_t)strlen(padded),
           "munder does not write the pairs file");
    ng_motive_rank("why did the ship fail", &ranked);
    expect(ranked.lex_w == 64 && strcmp(ranked.concept, pair) == 0,
           "munder does not write the learn cache");
    margin = ng_motive_label_margin("why did the ship fail", path);
    if (counted < 0 || margin < 0 || greatest != margin || top != 1 ||
        sum != margin || least != margin || under != 0) {
      fprintf(stderr,
              "FAIL munder probe hits=%d margin=%d max=%d top=%d sum=%d min=%d under=%d\n",
              counted, margin, greatest, top, sum, least, under);
      g_fail++;
    }
    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");

    pf = fopen(path, "w");
    expect(pf != NULL, "munder trail file opens");
    if (pf) {
      fputs(trail, pf);
      fclose(pf);
    }
    rc = ng_motive_label_rank("ship fail", path, &m);
    if (!(rc == 0 && strcmp(m.concept, pair) == 0 && m.lexical == 1 &&
          m.hamming == 23 && m.rank == 105 && m.lex_w == 64)) {
      fprintf(stderr,
              "FAIL munder trail rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    if (margin < 0 || (margin > 0 && counted != 1) ||
        (margin == 0 && counted != 0) || greatest != margin || top != 1 ||
        sum != margin || least != margin || under != 0) {
      fprintf(stderr,
              "FAIL munder trail margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d\n",
              margin, counted, greatest, top, sum, least, under);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "munder dots file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fclose(pf);
    }
    margin = ng_motive_label_margin("...", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    if (margin != 0 || counted != 0 || greatest != 0 || top != 1 || sum != 0 ||
        least != 0 || under != 0) {
      fprintf(stderr,
              "FAIL munder dots margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d\n",
              margin, counted, greatest, top, sum, least, under);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "munder two file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    dots_m = ng_motive_label_margin("...", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    if (dots_m != 0 || margin < 0 || least != 0 || sum != margin ||
        greatest != (margin > dots_m ? margin : dots_m) ||
        (margin > 0 && (under != 1 || greatest != margin || top != 1 || counted != 1)) ||
        (margin == 0 && (under != 0 || top != 2 || counted != 0)) ||
        (margin > 1 && under == margin)) {
      fprintf(stderr,
              "FAIL munder two ship=%d dots=%d hits=%d max=%d top=%d sum=%d min=%d under=%d\n",
              margin, dots_m, counted, greatest, top, sum, least, under);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "munder two-zero file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fprintf(pf, "...\t%s\n", peer);
      fclose(pf);
    }
    margin = ng_motive_label_margin("...", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    if (margin != 0 || counted != 0 || greatest != 0 || top != 2 || sum != 0 ||
        least != 0 || under != 0) {
      fprintf(stderr,
              "FAIL munder two-zero margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d\n",
              margin, counted, greatest, top, sum, least, under);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "munder two-ship file opens");
    if (pf) {
      fputs(ship, pf);
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    if (margin < 0 || greatest != margin || least != margin || top != 2 ||
        counted != (margin > 0 ? 2 : 0) || sum != margin + margin || under != 0 ||
        (margin != 1 && sum == top)) {
      fprintf(stderr,
              "FAIL munder two-ship margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d\n",
              margin, counted, greatest, top, sum, least, under);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "munder two-dots ship file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fprintf(pf, "...\t%s\n", peer);
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    dots_m = ng_motive_label_margin("...", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    if (dots_m != 0 || margin < 0 || least != 0 ||
        (margin > 0 && (under != 2 || top != 1 || sum != margin ||
                        greatest != margin || counted != 1)) ||
        (margin == 0 && (under != 0 || top != 3)) ||
        (margin > 2 && under == margin)) {
      fprintf(stderr,
              "FAIL munder below ship=%d dots=%d hits=%d max=%d top=%d sum=%d min=%d under=%d\n",
              margin, dots_m, counted, greatest, top, sum, least, under);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "munder spread file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fputs(audit, pf);
      fputs(ship, pf);
      fclose(pf);
    }
    qs[0] = "...";
    qs[1] = "why did the ship fail";
    qs[2] = "ship fail";
    seen_m = 0;
    best_m = -1;
    least_m = 0;
    expect_u = 0;
    least_n = 0;
    mid_n = 0;
    for (si = 0; si < 3; si++) {
      ms[si] = ng_motive_label_margin(qs[si], path);
      if (ms[si] < 0) continue;
      if (!seen_m || ms[si] < least_m) least_m = ms[si];
      if (ms[si] > best_m) best_m = ms[si];
      seen_m++;
    }
    for (si = 0; si < 3; si++) {
      if (ms[si] < 0) continue;
      if (ms[si] < best_m) expect_u++;
      if (ms[si] == least_m) least_n++;
      if (ms[si] > least_m && ms[si] < best_m) mid_n++;
    }
    under = ng_motive_margin_under(path);
    least = ng_motive_margin_min(path);
    greatest = ng_motive_margin_max(path);
    if ((seen_m == 0 && under != -1) || (seen_m > 0 && under != expect_u) ||
        (seen_m > 0 && (least != least_m || greatest != best_m)) ||
        (mid_n > 0 && under == least_n)) {
      fprintf(stderr,
              "FAIL munder spread under=%d expect=%d min=%d least=%d max=%d best=%d mid=%d margins=%d,%d,%d\n",
              under, expect_u, least, least_m, greatest, best_m, mid_n, ms[0], ms[1], ms[2]);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "munder sealed file opens");
    if (pf) {
      fputs(ship, pf);
      fclose(pf);
    }
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    setenv("NANOBOT_MOTIVE_LEARN", path, 1);
    ng_motive_rank("ship fail", &ranked);
    if (!(strcmp(ranked.concept, keep) == 0 && ranked.lexical == 2 &&
          ranked.hamming == 22 && ranked.rank == 170 && ranked.lex_w == 64)) {
      fprintf(stderr,
              "FAIL munder sealed concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              ranked.concept, ranked.lexical, ranked.hamming, ranked.rank,
              ranked.lex_w);
      g_fail++;
    }
    ng_motive_rank_w("ship fail", &ranked, 64);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lexical == 1 && ranked.hamming == 23 && ranked.lex_w == 64,
           "weight 64 still returns the pair line");
    unsetenv("NANOBOT_MOTIVE_LEARN");
    ng_motive_rank("ship fail", &ranked);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lex_w == 64,
           "unset learn still leaves the pair line");
    ng_motive_rank(law, &ranked);
    if (!(strcmp(ranked.concept, law) == 0 && ranked.rank == 256 &&
          ranked.lexical == 3 && ranked.hamming == 0 && ranked.lex_w == 64)) {
      fprintf(stderr,
              "FAIL munder sealed law concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              ranked.concept, ranked.lexical, ranked.hamming, ranked.rank,
              ranked.lex_w);
      g_fail++;
    }
    learned = ng_motive_learn_hits(path);
    marked = ng_motive_label_hits(path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    if (learned != 0 || marked != 1 || counted != 1 || greatest != 65 ||
        top != 1 || sum != 65 || least != 65 || under != 0 || sum != greatest ||
        least != greatest) {
      fprintf(stderr,
              "FAIL munder sealed counts learn=%d label=%d hits=%d max=%d top=%d sum=%d min=%d under=%d\n",
              learned, marked, counted, greatest, top, sum, least, under);
      g_fail++;
    }
    ng_motive_rank("you lied", &ranked);
    expect(ranked.plate == NG_PLATE_CONFESS && ranked.lexical == 0,
           "munder leaves you lied");
    ng_motive_rank("", &ranked);
    expect(ranked.rank == 0, "munder leaves an empty query at rank 0");
    unlink(path);
  }

  {
    const char *path = "/tmp/nb-motive-mlow.pairs";
    const char *dirp = "/tmp/nb-motive-mlow.dir";
    const char *keep = "keep free energy flowing";
    const char *pair = "pair vote then ship the step";
    const char *law = "read the law and prove";
    const char *audit_c = "audit the last code change";
    const char *peer = "prompt a peer to move";
    const char *ship = "ship fail\tkeep free energy flowing\n";
    const char *audit = "why did the ship fail\taudit the last code change\n";
    const char *padded = "why did the ship fail\tkeep free energy flowing  \n";
    const char *trail = "ship fail\tkeep free energy flowing \n";
    const char *qs[3];
    char longp[520];
    char qbuf[200];
    char cbuf[96];
    char blob[16384];
    char big[420];
    FILE *pf;
    ng_motive m, ranked;
    struct stat st;
    struct timespec ts[2];
    int rc, margin, counted, learned, marked, greatest, dots_m, top, sum, least, under, low;
    int ms[3];
    int si, seen_m, best_m, least_m, expect_u, least_n, mid_n, kept;

    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    expect(ng_motive_margin_low(NULL) == -1, "a null mlow path returns -1");
    expect(ng_motive_margin_low("") == -1, "an empty mlow path returns -1");
    memset(longp, 'm', 512);
    longp[512] = 0;
    expect(strlen(longp) == 512, "the long mlow path is 512 bytes");
    expect(ng_motive_margin_low(longp) == -1,
           "an mlow path of 512 bytes returns -1");
    expect(ng_motive_margin_low("/tmp/nb-motive-mlow-missing") == -1,
           "a missing mlow file returns -1");
    unlink(dirp);
    rmdir(dirp);
    expect(mkdir(dirp, 0700) == 0, "mlow directory creates");
    expect(ng_motive_margin_low(dirp) == -1,
           "a non-regular mlow path returns -1");
    rmdir(dirp);

    pf = fopen(path, "w");
    expect(pf != NULL, "mlow empty file opens");
    if (pf) {
      fputs("# note\n\n", pf);
      fclose(pf);
    }
    expect(ng_motive_margin_low(path) == -1,
           "an mlow file with no pair returns -1");

    memset(qbuf, 'q', 192);
    qbuf[192] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mlow overlong query file opens");
    if (pf) {
      fprintf(pf, "%s\t%s\n", qbuf, keep);
      fclose(pf);
    }
    expect(ng_motive_margin_low(path) == -1,
           "an mlow query of 192 bytes is not a pair");
    qbuf[191] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mlow bounded query file opens");
    if (pf) {
      fprintf(pf, "%s\t%s\n", qbuf, keep);
      fclose(pf);
    }
    margin = ng_motive_label_margin(qbuf, path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    if (margin < 0 || counted != (margin > 0 ? 1 : 0) || greatest != margin ||
        top != 1 || sum != margin || least != margin || under != 0 || low != 1) {
      fprintf(stderr,
              "FAIL mlow bounded margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d\n",
              margin, counted, greatest, top, sum, least, under, low);
      g_fail++;
    }

    memset(cbuf, 'c', 80);
    cbuf[80] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mlow overlong catalog file opens");
    if (pf) {
      fprintf(pf, "ship fail\t%s\n", cbuf);
      fclose(pf);
    }
    expect(ng_motive_margin_low(path) == -1,
           "an mlow catalog line of 80 bytes is not a pair");
    cbuf[79] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mlow bounded catalog file opens");
    if (pf) {
      fprintf(pf, "ship fail\t%s\n", cbuf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    if (margin < 0 || counted != (margin > 0 ? 1 : 0) || greatest != margin ||
        top != 1 || sum != margin || least != margin || under != 0 || low != 1) {
      fprintf(stderr,
              "FAIL mlow catalog margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d\n",
              margin, counted, greatest, top, sum, least, under, low);
      g_fail++;
    }

    memset(blob, 'x', sizeof blob);
    pf = fopen(path, "w");
    expect(pf != NULL, "mlow budget file opens");
    if (pf) {
      fwrite(blob, 1, sizeof blob, pf);
      fputc('\n', pf);
      fputs(ship, pf);
      fclose(pf);
    }
    expect(ng_motive_margin_low(path) == -1,
           "an mlow pair past the 16384 byte budget is not read");

    memset(big, 'z', 400);
    big[400] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mlow overlong line file opens");
    if (pf) {
      fprintf(pf, "%s\n", big);
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    if (margin != 65 || counted != 1 || greatest != 65 || top != 1 ||
        sum != 65 || least != 65 || under != 0 || low != 1) {
      fprintf(stderr,
              "FAIL mlow skip margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d\n",
              margin, counted, greatest, top, sum, least, under, low);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mlow ship file opens");
    if (pf) {
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    if (margin != 65 || counted != 1 || greatest != 65 || top != 1 ||
        sum != 65 || least != 65 || under != 0 || low != 1 || low == least ||
        sum != greatest || least != greatest) {
      fprintf(stderr,
              "FAIL mlow ship margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d\n",
              margin, counted, greatest, top, sum, least, under, low);
      g_fail++;
    }
    setenv("NANOBOT_MOTIVE_LEARN", "/tmp/nb-motive-mlow-missing", 1);
    setenv("NANOBOT_MOTIVE_LEX_W", "1", 1);
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    if (margin != 65 || counted != 1 || greatest != 65 || top != 1 ||
        sum != 65 || least != 65 || under != 0 || low != 1) {
      fprintf(stderr,
              "FAIL mlow env margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d\n",
              margin, counted, greatest, top, sum, least, under, low);
      g_fail++;
    }
    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    margin = ng_motive_label_margin(law, path);
    if (margin <= 0) {
      fprintf(stderr, "FAIL mlow law margin %d\n", margin);
      g_fail++;
    }
    rc = ng_motive_label_rank(law, path, &m);
    if (!(rc == 0 && strcmp(m.concept, law) == 0 && m.lexical == 3 &&
          m.hamming == 0 && m.rank == 256 && m.lex_w == 64)) {
      fprintf(stderr,
              "FAIL mlow law rank rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }
    expect(getenv("NANOBOT_MOTIVE_LEARN") == NULL, "mlow leaves LEARN unset");
    ng_motive_rank("ship fail", &ranked);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lexical == 1 && ranked.hamming == 23 && ranked.lex_w == 64,
           "mlow leaves ship fail on the pair line");
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    if (greatest != 65 || top != 1 || sum != 65 || least != 65 || under != 0 ||
        low != 1 || low == least) {
      fprintf(stderr,
              "FAIL mlow ship stays max=%d top=%d sum=%d min=%d under=%d low=%d\n",
              greatest, top, sum, least, under, low);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mlow audit file opens");
    if (pf) {
      fputs(audit, pf);
      fclose(pf);
    }
    rc = ng_motive_label_rank("why did the ship fail", path, &m);
    if (!(rc == 0 && strcmp(m.concept, audit_c) == 0 && m.lexical == 4 &&
          m.hamming == 18 && m.rank == 46 && m.lex_w == 0)) {
      fprintf(stderr,
              "FAIL mlow audit rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }
    margin = ng_motive_label_margin("why did the ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    if (margin < 0 || (margin > 0 && counted != 1) ||
        (margin == 0 && counted != 0) || greatest != margin || top != 1 ||
        sum != margin || least != margin || under != 0 || low != 1) {
      fprintf(stderr,
              "FAIL mlow audit margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d\n",
              margin, counted, greatest, top, sum, least, under, low);
      g_fail++;
    }
    expect(strlen(audit) == strlen(padded), "mlow cache probe stays the same size");
    expect(stat(path, &st) == 0 && st.st_size == (off_t)strlen(audit),
           "mlow pairs stat");
    ts[0].tv_sec = st.st_atim.tv_sec;
    ts[0].tv_nsec = st.st_atim.tv_nsec;
    ts[1].tv_sec = st.st_mtim.tv_sec;
    ts[1].tv_nsec = st.st_mtim.tv_nsec;
    pf = fopen(path, "w");
    expect(pf != NULL, "mlow cache probe opens");
    if (pf) {
      fputs(padded, pf);
      fclose(pf);
    }
    expect(utimensat(AT_FDCWD, path, ts, 0) == 0, "mlow cache probe restores mtime");
    setenv("NANOBOT_MOTIVE_LEARN", path, 1);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    expect(stat(path, &st) == 0 && st.st_mtim.tv_sec == ts[1].tv_sec &&
               st.st_mtim.tv_nsec == ts[1].tv_nsec &&
               st.st_size == (off_t)strlen(padded),
           "mlow does not write the pairs file");
    ng_motive_rank("why did the ship fail", &ranked);
    expect(ranked.lex_w == 64 && strcmp(ranked.concept, pair) == 0,
           "mlow does not write the learn cache");
    margin = ng_motive_label_margin("why did the ship fail", path);
    if (counted < 0 || margin < 0 || greatest != margin || top != 1 ||
        sum != margin || least != margin || under != 0 || low != 1) {
      fprintf(stderr,
              "FAIL mlow probe hits=%d margin=%d max=%d top=%d sum=%d min=%d under=%d low=%d\n",
              counted, margin, greatest, top, sum, least, under, low);
      g_fail++;
    }
    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");

    pf = fopen(path, "w");
    expect(pf != NULL, "mlow trail file opens");
    if (pf) {
      fputs(trail, pf);
      fclose(pf);
    }
    rc = ng_motive_label_rank("ship fail", path, &m);
    if (!(rc == 0 && strcmp(m.concept, pair) == 0 && m.lexical == 1 &&
          m.hamming == 23 && m.rank == 105 && m.lex_w == 64)) {
      fprintf(stderr,
              "FAIL mlow trail rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    if (margin < 0 || (margin > 0 && counted != 1) ||
        (margin == 0 && counted != 0) || greatest != margin || top != 1 ||
        sum != margin || least != margin || under != 0 || low != 1) {
      fprintf(stderr,
              "FAIL mlow trail margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d\n",
              margin, counted, greatest, top, sum, least, under, low);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mlow dots file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fclose(pf);
    }
    margin = ng_motive_label_margin("...", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    if (margin != 0 || counted != 0 || greatest != 0 || top != 1 || sum != 0 ||
        least != 0 || under != 0 || low != 1) {
      fprintf(stderr,
              "FAIL mlow dots margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d\n",
              margin, counted, greatest, top, sum, least, under, low);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mlow two file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    dots_m = ng_motive_label_margin("...", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    kept = (dots_m >= 0 ? 1 : 0) + (margin >= 0 ? 1 : 0);
    if (dots_m != 0 || margin < 0 || least != 0 || sum != margin ||
        greatest != (margin > dots_m ? margin : dots_m) || kept != 2 ||
        (margin > 0 && (low != 1 || under != 1 || greatest != margin ||
                        top != 1 || counted != 1 || low == kept)) ||
        (margin == 0 && (low != 2 || under != 0 || top != 2 || counted != 0)) ||
        (margin > 1 && low == margin)) {
      fprintf(stderr,
              "FAIL mlow two ship=%d dots=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d kept=%d\n",
              margin, dots_m, counted, greatest, top, sum, least, under, low, kept);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mlow two-zero file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fprintf(pf, "...\t%s\n", peer);
      fclose(pf);
    }
    margin = ng_motive_label_margin("...", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    if (margin != 0 || counted != 0 || greatest != 0 || top != 2 || sum != 0 ||
        least != 0 || under != 0 || low != 2) {
      fprintf(stderr,
              "FAIL mlow two-zero margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d\n",
              margin, counted, greatest, top, sum, least, under, low);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mlow two-ship file opens");
    if (pf) {
      fputs(ship, pf);
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    if (margin < 0 || greatest != margin || least != margin || top != 2 ||
        counted != (margin > 0 ? 2 : 0) || sum != margin + margin || under != 0 ||
        low != 2 || (low == 2 && under != 0) ||
        (margin != 1 && sum == top)) {
      fprintf(stderr,
              "FAIL mlow two-ship margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d\n",
              margin, counted, greatest, top, sum, least, under, low);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mlow two-dots ship file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fprintf(pf, "...\t%s\n", peer);
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    dots_m = ng_motive_label_margin("...", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    if (dots_m != 0 || margin < 0 || least != 0 ||
        (margin > 0 && (low != 2 || under != 2 || top != 1 || sum != margin ||
                        greatest != margin || counted != 1)) ||
        (margin == 0 && (low != 3 || under != 0 || top != 3)) ||
        (margin > 2 && low == margin)) {
      fprintf(stderr,
              "FAIL mlow below ship=%d dots=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d\n",
              margin, dots_m, counted, greatest, top, sum, least, under, low);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mlow spread file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fputs(audit, pf);
      fputs(ship, pf);
      fclose(pf);
    }
    qs[0] = "...";
    qs[1] = "why did the ship fail";
    qs[2] = "ship fail";
    seen_m = 0;
    best_m = -1;
    least_m = 0;
    expect_u = 0;
    least_n = 0;
    mid_n = 0;
    for (si = 0; si < 3; si++) {
      ms[si] = ng_motive_label_margin(qs[si], path);
      if (ms[si] < 0) continue;
      if (!seen_m || ms[si] < least_m) least_m = ms[si];
      if (ms[si] > best_m) best_m = ms[si];
      seen_m++;
    }
    for (si = 0; si < 3; si++) {
      if (ms[si] < 0) continue;
      if (ms[si] < best_m) expect_u++;
      if (ms[si] == least_m) least_n++;
      if (ms[si] > least_m && ms[si] < best_m) mid_n++;
    }
    under = ng_motive_margin_under(path);
    least = ng_motive_margin_min(path);
    greatest = ng_motive_margin_max(path);
    low = ng_motive_margin_low(path);
    if ((seen_m == 0 && (under != -1 || low != -1)) ||
        (seen_m > 0 && (under != expect_u || low != least_n)) ||
        (seen_m > 0 && (least != least_m || greatest != best_m)) ||
        (mid_n > 0 && (under == least_n || low == under))) {
      fprintf(stderr,
              "FAIL mlow spread low=%d least_n=%d under=%d expect=%d min=%d least=%d max=%d best=%d mid=%d margins=%d,%d,%d\n",
              low, least_n, under, expect_u, least, least_m, greatest, best_m, mid_n,
              ms[0], ms[1], ms[2]);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mlow sealed file opens");
    if (pf) {
      fputs(ship, pf);
      fclose(pf);
    }
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    setenv("NANOBOT_MOTIVE_LEARN", path, 1);
    ng_motive_rank("ship fail", &ranked);
    if (!(strcmp(ranked.concept, keep) == 0 && ranked.lexical == 2 &&
          ranked.hamming == 22 && ranked.rank == 170 && ranked.lex_w == 64)) {
      fprintf(stderr,
              "FAIL mlow sealed concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              ranked.concept, ranked.lexical, ranked.hamming, ranked.rank,
              ranked.lex_w);
      g_fail++;
    }
    ng_motive_rank_w("ship fail", &ranked, 64);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lexical == 1 && ranked.hamming == 23 && ranked.lex_w == 64,
           "weight 64 still returns the pair line");
    unsetenv("NANOBOT_MOTIVE_LEARN");
    ng_motive_rank("ship fail", &ranked);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lex_w == 64,
           "unset learn still leaves the pair line");
    ng_motive_rank(law, &ranked);
    if (!(strcmp(ranked.concept, law) == 0 && ranked.rank == 256 &&
          ranked.lexical == 3 && ranked.hamming == 0 && ranked.lex_w == 64)) {
      fprintf(stderr,
              "FAIL mlow sealed law concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              ranked.concept, ranked.lexical, ranked.hamming, ranked.rank,
              ranked.lex_w);
      g_fail++;
    }
    learned = ng_motive_learn_hits(path);
    marked = ng_motive_label_hits(path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    if (learned != 0 || marked != 1 || counted != 1 || greatest != 65 ||
        top != 1 || sum != 65 || least != 65 || under != 0 || low != 1 ||
        low == least || sum != greatest || least != greatest) {
      fprintf(stderr,
              "FAIL mlow sealed counts learn=%d label=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d\n",
              learned, marked, counted, greatest, top, sum, least, under, low);
      g_fail++;
    }
    ng_motive_rank("you lied", &ranked);
    expect(ranked.plate == NG_PLATE_CONFESS && ranked.lexical == 0,
           "mlow leaves you lied");
    ng_motive_rank("", &ranked);
    expect(ranked.rank == 0, "mlow leaves an empty query at rank 0");
    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    unlink(path);
  }

  {
    const char *path = "/tmp/nb-motive-mnext.pairs";
    const char *dirp = "/tmp/nb-motive-mnext.dir";
    const char *keep = "keep free energy flowing";
    const char *pair = "pair vote then ship the step";
    const char *law = "read the law and prove";
    const char *audit_c = "audit the last code change";
    const char *peer = "prompt a peer to move";
    const char *ship = "ship fail\tkeep free energy flowing\n";
    const char *audit = "why did the ship fail\taudit the last code change\n";
    const char *padded = "why did the ship fail\tkeep free energy flowing  \n";
    const char *trail = "ship fail\tkeep free energy flowing \n";
    const char *qs[3];
    char longp[520];
    char qbuf[200];
    char cbuf[96];
    char blob[16384];
    char big[420];
    FILE *pf;
    ng_motive m, ranked;
    struct stat st;
    struct timespec ts[2];
    int rc, margin, counted, learned, marked, greatest, dots_m, top, sum, least;
    int under, low, nxt;
    int ms[3];
    int si, seen_m, best_m, least_m, expect_u, least_n, mid_n, kept, expect_n;
    int span;

    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    expect(ng_motive_margin_next(NULL) == -1, "a null mnext path returns -1");
    expect(ng_motive_margin_next("") == -1, "an empty mnext path returns -1");
    memset(longp, 'm', 512);
    longp[512] = 0;
    expect(strlen(longp) == 512, "the long mnext path is 512 bytes");
    expect(ng_motive_margin_next(longp) == -1,
           "an mnext path of 512 bytes returns -1");
    expect(ng_motive_margin_next("/tmp/nb-motive-mnext-missing") == -1,
           "a missing mnext file returns -1");
    unlink(dirp);
    rmdir(dirp);
    expect(mkdir(dirp, 0700) == 0, "mnext directory creates");
    expect(ng_motive_margin_next(dirp) == -1,
           "a non-regular mnext path returns -1");
    rmdir(dirp);

    pf = fopen(path, "w");
    expect(pf != NULL, "mnext empty file opens");
    if (pf) {
      fputs("# note\n\n", pf);
      fclose(pf);
    }
    expect(ng_motive_margin_next(path) == -1,
           "an mnext file with no pair returns -1");

    memset(qbuf, 'q', 192);
    qbuf[192] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mnext overlong query file opens");
    if (pf) {
      fprintf(pf, "%s\t%s\n", qbuf, keep);
      fclose(pf);
    }
    expect(ng_motive_margin_next(path) == -1,
           "an mnext query of 192 bytes is not a pair");
    qbuf[191] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mnext bounded query file opens");
    if (pf) {
      fprintf(pf, "%s\t%s\n", qbuf, keep);
      fclose(pf);
    }
    margin = ng_motive_label_margin(qbuf, path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    if (margin < 0 || counted != (margin > 0 ? 1 : 0) || greatest != margin ||
        top != 1 || sum != margin || least != margin || under != 0 ||
        low != 1 || nxt != -1 || nxt == greatest) {
      fprintf(stderr,
              "FAIL mnext bounded margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d\n",
              margin, counted, greatest, top, sum, least, under, low, nxt);
      g_fail++;
    }

    memset(cbuf, 'c', 80);
    cbuf[80] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mnext overlong catalog file opens");
    if (pf) {
      fprintf(pf, "ship fail\t%s\n", cbuf);
      fclose(pf);
    }
    expect(ng_motive_margin_next(path) == -1,
           "an mnext catalog line of 80 bytes is not a pair");
    cbuf[79] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mnext bounded catalog file opens");
    if (pf) {
      fprintf(pf, "ship fail\t%s\n", cbuf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    if (margin < 0 || counted != (margin > 0 ? 1 : 0) || greatest != margin ||
        top != 1 || sum != margin || least != margin || under != 0 ||
        low != 1 || nxt != -1) {
      fprintf(stderr,
              "FAIL mnext catalog margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d\n",
              margin, counted, greatest, top, sum, least, under, low, nxt);
      g_fail++;
    }

    memset(blob, 'x', sizeof blob);
    pf = fopen(path, "w");
    expect(pf != NULL, "mnext budget file opens");
    if (pf) {
      fwrite(blob, 1, sizeof blob, pf);
      fputc('\n', pf);
      fputs(ship, pf);
      fclose(pf);
    }
    expect(ng_motive_margin_next(path) == -1,
           "an mnext pair past the 16384 byte budget is not read");

    memset(big, 'z', 400);
    big[400] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mnext overlong line file opens");
    if (pf) {
      fprintf(pf, "%s\n", big);
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    if (margin != 65 || counted != 1 || greatest != 65 || top != 1 ||
        sum != 65 || least != 65 || under != 0 || low != 1 || nxt != -1) {
      fprintf(stderr,
              "FAIL mnext skip margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d\n",
              margin, counted, greatest, top, sum, least, under, low, nxt);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mnext ship file opens");
    if (pf) {
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    if (margin != 65 || counted != 1 || greatest != 65 || top != 1 ||
        sum != 65 || least != 65 || under != 0 || low != 1 || nxt != -1 ||
        nxt == greatest || sum != greatest || least != greatest) {
      fprintf(stderr,
              "FAIL mnext ship margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d\n",
              margin, counted, greatest, top, sum, least, under, low, nxt);
      g_fail++;
    }
    setenv("NANOBOT_MOTIVE_LEARN", "/tmp/nb-motive-mnext-missing", 1);
    setenv("NANOBOT_MOTIVE_LEX_W", "1", 1);
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    if (margin != 65 || counted != 1 || greatest != 65 || top != 1 ||
        sum != 65 || least != 65 || under != 0 || low != 1 || nxt != -1) {
      fprintf(stderr,
              "FAIL mnext env margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d\n",
              margin, counted, greatest, top, sum, least, under, low, nxt);
      g_fail++;
    }
    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    margin = ng_motive_label_margin(law, path);
    if (margin <= 0) {
      fprintf(stderr, "FAIL mnext law margin %d\n", margin);
      g_fail++;
    }
    rc = ng_motive_label_rank(law, path, &m);
    if (!(rc == 0 && strcmp(m.concept, law) == 0 && m.lexical == 3 &&
          m.hamming == 0 && m.rank == 256 && m.lex_w == 64)) {
      fprintf(stderr,
              "FAIL mnext law rank rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }
    expect(getenv("NANOBOT_MOTIVE_LEARN") == NULL, "mnext leaves LEARN unset");
    ng_motive_rank("ship fail", &ranked);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lexical == 1 && ranked.hamming == 23 && ranked.lex_w == 64,
           "mnext leaves ship fail on the pair line");
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    if (greatest != 65 || top != 1 || sum != 65 || least != 65 || under != 0 ||
        low != 1 || nxt != -1 || nxt == greatest) {
      fprintf(stderr,
              "FAIL mnext ship stays max=%d top=%d sum=%d min=%d under=%d low=%d next=%d\n",
              greatest, top, sum, least, under, low, nxt);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mnext audit file opens");
    if (pf) {
      fputs(audit, pf);
      fclose(pf);
    }
    rc = ng_motive_label_rank("why did the ship fail", path, &m);
    if (!(rc == 0 && strcmp(m.concept, audit_c) == 0 && m.lexical == 4 &&
          m.hamming == 18 && m.rank == 46 && m.lex_w == 0)) {
      fprintf(stderr,
              "FAIL mnext audit rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }
    margin = ng_motive_label_margin("why did the ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    if (margin < 0 || (margin > 0 && counted != 1) ||
        (margin == 0 && counted != 0) || greatest != margin || top != 1 ||
        sum != margin || least != margin || under != 0 || low != 1 ||
        nxt != -1) {
      fprintf(stderr,
              "FAIL mnext audit margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d\n",
              margin, counted, greatest, top, sum, least, under, low, nxt);
      g_fail++;
    }
    expect(strlen(audit) == strlen(padded), "mnext cache probe stays the same size");
    expect(stat(path, &st) == 0 && st.st_size == (off_t)strlen(audit),
           "mnext pairs stat");
    ts[0].tv_sec = st.st_atim.tv_sec;
    ts[0].tv_nsec = st.st_atim.tv_nsec;
    ts[1].tv_sec = st.st_mtim.tv_sec;
    ts[1].tv_nsec = st.st_mtim.tv_nsec;
    pf = fopen(path, "w");
    expect(pf != NULL, "mnext cache probe opens");
    if (pf) {
      fputs(padded, pf);
      fclose(pf);
    }
    expect(utimensat(AT_FDCWD, path, ts, 0) == 0, "mnext cache probe restores mtime");
    setenv("NANOBOT_MOTIVE_LEARN", path, 1);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    nxt = ng_motive_margin_next(path);
    expect(stat(path, &st) == 0 && st.st_mtim.tv_sec == ts[1].tv_sec &&
               st.st_mtim.tv_nsec == ts[1].tv_nsec &&
               st.st_size == (off_t)strlen(padded),
           "mnext does not write the pairs file");
    ng_motive_rank("why did the ship fail", &ranked);
    expect(ranked.lex_w == 64 && strcmp(ranked.concept, pair) == 0,
           "mnext does not write the learn cache");
    margin = ng_motive_label_margin("why did the ship fail", path);
    if (counted < 0 || margin < 0 || greatest != margin || nxt != -1) {
      fprintf(stderr, "FAIL mnext probe hits=%d margin=%d max=%d next=%d\n",
              counted, margin, greatest, nxt);
      g_fail++;
    }
    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");

    pf = fopen(path, "w");
    expect(pf != NULL, "mnext trail file opens");
    if (pf) {
      fputs(trail, pf);
      fclose(pf);
    }
    rc = ng_motive_label_rank("ship fail", path, &m);
    if (!(rc == 0 && strcmp(m.concept, pair) == 0 && m.lexical == 1 &&
          m.hamming == 23 && m.rank == 105 && m.lex_w == 64)) {
      fprintf(stderr,
              "FAIL mnext trail rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    if (margin < 0 || (margin > 0 && counted != 1) ||
        (margin == 0 && counted != 0) || greatest != margin || top != 1 ||
        sum != margin || least != margin || under != 0 || low != 1 ||
        nxt != -1) {
      fprintf(stderr,
              "FAIL mnext trail margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d\n",
              margin, counted, greatest, top, sum, least, under, low, nxt);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mnext dots file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fclose(pf);
    }
    margin = ng_motive_label_margin("...", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    if (margin != 0 || counted != 0 || greatest != 0 || top != 1 || sum != 0 ||
        least != 0 || under != 0 || low != 1 || nxt != -1) {
      fprintf(stderr,
              "FAIL mnext dots margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d\n",
              margin, counted, greatest, top, sum, least, under, low, nxt);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mnext two file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    dots_m = ng_motive_label_margin("...", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kept = (dots_m >= 0 ? 1 : 0) + (margin >= 0 ? 1 : 0);
    if (dots_m != 0 || margin < 0 || least != 0 || sum != margin ||
        greatest != (margin > dots_m ? margin : dots_m) || kept != 2 ||
        (margin > 0 && (nxt != margin || low != 1 || under != 1 || top != 1 ||
                        counted != 1 || nxt == least || nxt == kept ||
                        (margin != (kept - low) && nxt == (kept - low)) ||
                        (margin != (kept - top) && nxt == (kept - top)))) ||
        (margin == 0 && (nxt != -1 || low != 2 || under != 0 || top != 2 ||
                         counted != 0))) {
      fprintf(stderr,
              "FAIL mnext two ship=%d dots=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kept=%d\n",
              margin, dots_m, counted, greatest, top, sum, least, under, low, nxt,
              kept);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mnext two-zero file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fprintf(pf, "...\t%s\n", peer);
      fclose(pf);
    }
    margin = ng_motive_label_margin("...", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    if (margin != 0 || counted != 0 || greatest != 0 || top != 2 || sum != 0 ||
        least != 0 || under != 0 || low != 2 || nxt != -1) {
      fprintf(stderr,
              "FAIL mnext two-zero margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d\n",
              margin, counted, greatest, top, sum, least, under, low, nxt);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mnext two-ship file opens");
    if (pf) {
      fputs(ship, pf);
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    if (margin < 0 || greatest != margin || least != margin || top != 2 ||
        counted != (margin > 0 ? 2 : 0) || sum != margin + margin || under != 0 ||
        low != 2 || nxt != -1 || (low == 2 && nxt != -1) ||
        (greatest == margin && nxt == greatest)) {
      fprintf(stderr,
              "FAIL mnext two-ship margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d\n",
              margin, counted, greatest, top, sum, least, under, low, nxt);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mnext two-dots ship file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fprintf(pf, "...\t%s\n", peer);
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    dots_m = ng_motive_label_margin("...", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    if (dots_m != 0 || margin < 0 || least != 0 ||
        (margin > 0 && (nxt != margin || low != 2 || under != 2 || top != 1 ||
                        sum != margin || greatest != margin || counted != 1 ||
                        nxt == least)) ||
        (margin == 0 && (nxt != -1 || low != 3 || under != 0 || top != 3))) {
      fprintf(stderr,
              "FAIL mnext below ship=%d dots=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d\n",
              margin, dots_m, counted, greatest, top, sum, least, under, low, nxt);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mnext spread file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fputs(audit, pf);
      fputs(ship, pf);
      fclose(pf);
    }
    qs[0] = "...";
    qs[1] = "why did the ship fail";
    qs[2] = "ship fail";
    seen_m = 0;
    best_m = -1;
    least_m = 0;
    expect_u = 0;
    least_n = 0;
    mid_n = 0;
    expect_n = -1;
    for (si = 0; si < 3; si++) {
      ms[si] = ng_motive_label_margin(qs[si], path);
      if (ms[si] < 0) continue;
      if (!seen_m || ms[si] < least_m) least_m = ms[si];
      if (ms[si] > best_m) best_m = ms[si];
      seen_m++;
    }
    for (si = 0; si < 3; si++) {
      if (ms[si] < 0) continue;
      if (ms[si] < best_m) expect_u++;
      if (ms[si] == least_m) least_n++;
      if (ms[si] > least_m && ms[si] < best_m) mid_n++;
      if (ms[si] > least_m && (expect_n < 0 || ms[si] < expect_n))
        expect_n = ms[si];
    }
    under = ng_motive_margin_under(path);
    least = ng_motive_margin_min(path);
    greatest = ng_motive_margin_max(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    span = (seen_m > 0 && best_m >= least_m) ? (best_m - least_m) : -1;
    if ((seen_m == 0 && (under != -1 || low != -1 || nxt != -1)) ||
        (seen_m > 0 && (under != expect_u || low != least_n || nxt != expect_n)) ||
        (seen_m > 0 && (least != least_m || greatest != best_m)) ||
        (seen_m > 0 && least_m == best_m && nxt != -1) ||
        (expect_n >= 0 && span != expect_n && nxt == span) ||
        (expect_n >= 0 && best_m > expect_n && nxt == best_m) ||
        (mid_n > 0 && nxt == best_m)) {
      fprintf(stderr,
              "FAIL mnext spread next=%d expect=%d low=%d least_n=%d under=%d expect_u=%d min=%d least=%d max=%d best=%d mid=%d span=%d margins=%d,%d,%d\n",
              nxt, expect_n, low, least_n, under, expect_u, least, least_m,
              greatest, best_m, mid_n, span, ms[0], ms[1], ms[2]);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mnext sealed file opens");
    if (pf) {
      fputs(ship, pf);
      fclose(pf);
    }
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    setenv("NANOBOT_MOTIVE_LEARN", path, 1);
    ng_motive_rank("ship fail", &ranked);
    if (!(strcmp(ranked.concept, keep) == 0 && ranked.lexical == 2 &&
          ranked.hamming == 22 && ranked.rank == 170 && ranked.lex_w == 64)) {
      fprintf(stderr,
              "FAIL mnext sealed concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              ranked.concept, ranked.lexical, ranked.hamming, ranked.rank,
              ranked.lex_w);
      g_fail++;
    }
    ng_motive_rank_w("ship fail", &ranked, 64);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lexical == 1 && ranked.hamming == 23 && ranked.lex_w == 64,
           "weight 64 still returns the pair line");
    unsetenv("NANOBOT_MOTIVE_LEARN");
    ng_motive_rank("ship fail", &ranked);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lex_w == 64,
           "unset learn still leaves the pair line");
    ng_motive_rank(law, &ranked);
    if (!(strcmp(ranked.concept, law) == 0 && ranked.rank == 256 &&
          ranked.lexical == 3 && ranked.hamming == 0 && ranked.lex_w == 64)) {
      fprintf(stderr,
              "FAIL mnext sealed law concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              ranked.concept, ranked.lexical, ranked.hamming, ranked.rank,
              ranked.lex_w);
      g_fail++;
    }
    learned = ng_motive_learn_hits(path);
    marked = ng_motive_label_hits(path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    if (learned != 0 || marked != 1 || counted != 1 || greatest != 65 ||
        top != 1 || sum != 65 || least != 65 || under != 0 || low != 1 ||
        nxt != -1 || nxt == greatest || sum != greatest || least != greatest) {
      fprintf(stderr,
              "FAIL mnext sealed counts learn=%d label=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d\n",
              learned, marked, counted, greatest, top, sum, least, under, low, nxt);
      g_fail++;
    }
    ng_motive_rank("you lied", &ranked);
    expect(ranked.plate == NG_PLATE_CONFESS && ranked.lexical == 0,
           "mnext leaves you lied");
    ng_motive_rank("", &ranked);
    expect(ranked.rank == 0, "mnext leaves an empty query at rank 0");
    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    unlink(path);
  }

  {
    const char *path = "/tmp/nb-motive-mkinds.pairs";
    const char *dirp = "/tmp/nb-motive-mkinds.dir";
    const char *keep = "keep free energy flowing";
    const char *pair = "pair vote then ship the step";
    const char *law = "read the law and prove";
    const char *audit_c = "audit the last code change";
    const char *peer = "prompt a peer to move";
    const char *ship = "ship fail\tkeep free energy flowing\n";
    const char *audit = "why did the ship fail\taudit the last code change\n";
    const char *padded = "why did the ship fail\tkeep free energy flowing  \n";
    const char *trail = "ship fail\tkeep free energy flowing \n";
    const char *qs[3];
    char longp[520];
    char qbuf[200];
    char cbuf[96];
    char blob[16384];
    char big[420];
    FILE *pf;
    ng_motive m, ranked;
    struct stat st;
    struct timespec ts[2];
    int rc, margin, counted, learned, marked, greatest, dots_m, top, sum, least;
    int under, low, nxt, kinds;
    int ms[3];
    int si, seen_m, best_m, least_m, expect_u, least_n, mid_n, kept, expect_n;
    int span, expect_k;

    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    expect(ng_motive_margin_kinds(NULL) == -1, "a null mkinds path returns -1");
    expect(ng_motive_margin_kinds("") == -1, "an empty mkinds path returns -1");
    memset(longp, 'm', 512);
    longp[512] = 0;
    expect(strlen(longp) == 512, "the long mkinds path is 512 bytes");
    expect(ng_motive_margin_kinds(longp) == -1,
           "an mkinds path of 512 bytes returns -1");
    expect(ng_motive_margin_kinds("/tmp/nb-motive-mkinds-missing") == -1,
           "a missing mkinds file returns -1");
    unlink(dirp);
    rmdir(dirp);
    expect(mkdir(dirp, 0700) == 0, "mkinds directory creates");
    expect(ng_motive_margin_kinds(dirp) == -1,
           "a non-regular mkinds path returns -1");
    rmdir(dirp);

    pf = fopen(path, "w");
    expect(pf != NULL, "mkinds empty file opens");
    if (pf) {
      fputs("# note\n\n", pf);
      fclose(pf);
    }
    expect(ng_motive_margin_kinds(path) == -1,
           "an mkinds file with no pair returns -1");

    memset(qbuf, 'q', 192);
    qbuf[192] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mkinds overlong query file opens");
    if (pf) {
      fprintf(pf, "%s\t%s\n", qbuf, keep);
      fclose(pf);
    }
    expect(ng_motive_margin_kinds(path) == -1,
           "an mkinds query of 192 bytes is not a pair");
    qbuf[191] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mkinds bounded query file opens");
    if (pf) {
      fprintf(pf, "%s\t%s\n", qbuf, keep);
      fclose(pf);
    }
    margin = ng_motive_label_margin(qbuf, path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    if (margin < 0 || counted != (margin > 0 ? 1 : 0) || greatest != margin ||
        top != 1 || sum != margin || least != margin || under != 0 ||
        low != 1 || nxt != -1 || kinds != 1) {
      fprintf(stderr,
              "FAIL mkinds bounded margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d\n",
              margin, counted, greatest, top, sum, least, under, low, nxt, kinds);
      g_fail++;
    }

    memset(cbuf, 'c', 80);
    cbuf[80] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mkinds overlong catalog file opens");
    if (pf) {
      fprintf(pf, "ship fail\t%s\n", cbuf);
      fclose(pf);
    }
    expect(ng_motive_margin_kinds(path) == -1,
           "an mkinds catalog line of 80 bytes is not a pair");
    cbuf[79] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mkinds bounded catalog file opens");
    if (pf) {
      fprintf(pf, "ship fail\t%s\n", cbuf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    if (margin < 0 || counted != (margin > 0 ? 1 : 0) || greatest != margin ||
        top != 1 || sum != margin || least != margin || under != 0 ||
        low != 1 || nxt != -1 || kinds != 1) {
      fprintf(stderr,
              "FAIL mkinds catalog margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d\n",
              margin, counted, greatest, top, sum, least, under, low, nxt, kinds);
      g_fail++;
    }

    memset(blob, 'x', sizeof blob);
    pf = fopen(path, "w");
    expect(pf != NULL, "mkinds budget file opens");
    if (pf) {
      fwrite(blob, 1, sizeof blob, pf);
      fputc('\n', pf);
      fputs(ship, pf);
      fclose(pf);
    }
    expect(ng_motive_margin_kinds(path) == -1,
           "an mkinds pair past the 16384 byte budget is not read");

    memset(big, 'z', 400);
    big[400] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mkinds overlong line file opens");
    if (pf) {
      fprintf(pf, "%s\n", big);
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    if (margin != 65 || counted != 1 || greatest != 65 || top != 1 ||
        sum != 65 || least != 65 || under != 0 || low != 1 || nxt != -1 ||
        kinds != 1) {
      fprintf(stderr,
              "FAIL mkinds skip margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d\n",
              margin, counted, greatest, top, sum, least, under, low, nxt, kinds);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mkinds ship file opens");
    if (pf) {
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    if (margin != 65 || counted != 1 || greatest != 65 || top != 1 ||
        sum != 65 || least != 65 || under != 0 || low != 1 || nxt != -1 ||
        kinds != 1 || kinds == greatest || kinds == nxt ||
        sum != greatest || least != greatest) {
      fprintf(stderr,
              "FAIL mkinds ship margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d\n",
              margin, counted, greatest, top, sum, least, under, low, nxt, kinds);
      g_fail++;
    }
    setenv("NANOBOT_MOTIVE_LEARN", "/tmp/nb-motive-mkinds-missing", 1);
    setenv("NANOBOT_MOTIVE_LEX_W", "1", 1);
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    if (margin != 65 || counted != 1 || greatest != 65 || top != 1 ||
        sum != 65 || least != 65 || under != 0 || low != 1 || nxt != -1 ||
        kinds != 1) {
      fprintf(stderr,
              "FAIL mkinds env margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d\n",
              margin, counted, greatest, top, sum, least, under, low, nxt, kinds);
      g_fail++;
    }
    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    margin = ng_motive_label_margin(law, path);
    if (margin <= 0) {
      fprintf(stderr, "FAIL mkinds law margin %d\n", margin);
      g_fail++;
    }
    rc = ng_motive_label_rank(law, path, &m);
    if (!(rc == 0 && strcmp(m.concept, law) == 0 && m.lexical == 3 &&
          m.hamming == 0 && m.rank == 256 && m.lex_w == 64)) {
      fprintf(stderr,
              "FAIL mkinds law rank rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }
    expect(getenv("NANOBOT_MOTIVE_LEARN") == NULL, "mkinds leaves LEARN unset");
    ng_motive_rank("ship fail", &ranked);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lexical == 1 && ranked.hamming == 23 && ranked.lex_w == 64,
           "mkinds leaves ship fail on the pair line");
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    if (greatest != 65 || top != 1 || sum != 65 || least != 65 || under != 0 ||
        low != 1 || nxt != -1 || kinds != 1 || kinds == greatest) {
      fprintf(stderr,
              "FAIL mkinds ship stays max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d\n",
              greatest, top, sum, least, under, low, nxt, kinds);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mkinds audit file opens");
    if (pf) {
      fputs(audit, pf);
      fclose(pf);
    }
    rc = ng_motive_label_rank("why did the ship fail", path, &m);
    if (!(rc == 0 && strcmp(m.concept, audit_c) == 0 && m.lexical == 4 &&
          m.hamming == 18 && m.rank == 46 && m.lex_w == 0)) {
      fprintf(stderr,
              "FAIL mkinds audit rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }
    margin = ng_motive_label_margin("why did the ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    if (margin < 0 || (margin > 0 && counted != 1) ||
        (margin == 0 && counted != 0) || greatest != margin || top != 1 ||
        sum != margin || least != margin || under != 0 || low != 1 ||
        nxt != -1 || kinds != 1) {
      fprintf(stderr,
              "FAIL mkinds audit margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d\n",
              margin, counted, greatest, top, sum, least, under, low, nxt, kinds);
      g_fail++;
    }
    expect(strlen(audit) == strlen(padded), "mkinds cache probe stays the same size");
    expect(stat(path, &st) == 0 && st.st_size == (off_t)strlen(audit),
           "mkinds pairs stat");
    ts[0].tv_sec = st.st_atim.tv_sec;
    ts[0].tv_nsec = st.st_atim.tv_nsec;
    ts[1].tv_sec = st.st_mtim.tv_sec;
    ts[1].tv_nsec = st.st_mtim.tv_nsec;
    pf = fopen(path, "w");
    expect(pf != NULL, "mkinds cache probe opens");
    if (pf) {
      fputs(padded, pf);
      fclose(pf);
    }
    expect(utimensat(AT_FDCWD, path, ts, 0) == 0, "mkinds cache probe restores mtime");
    setenv("NANOBOT_MOTIVE_LEARN", path, 1);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    expect(stat(path, &st) == 0 && st.st_mtim.tv_sec == ts[1].tv_sec &&
               st.st_mtim.tv_nsec == ts[1].tv_nsec &&
               st.st_size == (off_t)strlen(padded),
           "mkinds does not write the pairs file");
    ng_motive_rank("why did the ship fail", &ranked);
    expect(ranked.lex_w == 64 && strcmp(ranked.concept, pair) == 0,
           "mkinds does not write the learn cache");
    margin = ng_motive_label_margin("why did the ship fail", path);
    if (counted < 0 || margin < 0 || greatest != margin || nxt != -1 ||
        kinds != 1) {
      fprintf(stderr,
              "FAIL mkinds probe hits=%d margin=%d max=%d next=%d kinds=%d\n",
              counted, margin, greatest, nxt, kinds);
      g_fail++;
    }
    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");

    pf = fopen(path, "w");
    expect(pf != NULL, "mkinds trail file opens");
    if (pf) {
      fputs(trail, pf);
      fclose(pf);
    }
    rc = ng_motive_label_rank("ship fail", path, &m);
    if (!(rc == 0 && strcmp(m.concept, pair) == 0 && m.lexical == 1 &&
          m.hamming == 23 && m.rank == 105 && m.lex_w == 64)) {
      fprintf(stderr,
              "FAIL mkinds trail rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    if (margin < 0 || (margin > 0 && counted != 1) ||
        (margin == 0 && counted != 0) || greatest != margin || top != 1 ||
        sum != margin || least != margin || under != 0 || low != 1 ||
        nxt != -1 || kinds != 1) {
      fprintf(stderr,
              "FAIL mkinds trail margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d\n",
              margin, counted, greatest, top, sum, least, under, low, nxt, kinds);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mkinds dots file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fclose(pf);
    }
    margin = ng_motive_label_margin("...", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    if (margin != 0 || counted != 0 || greatest != 0 || top != 1 || sum != 0 ||
        least != 0 || under != 0 || low != 1 || nxt != -1 || kinds != 1) {
      fprintf(stderr,
              "FAIL mkinds dots margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d\n",
              margin, counted, greatest, top, sum, least, under, low, nxt, kinds);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mkinds two file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    dots_m = ng_motive_label_margin("...", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    kept = (dots_m >= 0 ? 1 : 0) + (margin >= 0 ? 1 : 0);
    if (dots_m != 0 || margin < 0 || least != 0 || sum != margin ||
        greatest != (margin > dots_m ? margin : dots_m) || kept != 2 ||
        (margin > 0 && (kinds != 2 || nxt != margin || low != 1 || under != 1 ||
                        top != 1 || counted != 1 || kinds == least ||
                        kinds == low || kinds == (kept - low) ||
                        kinds == (kept - top))) ||
        (margin == 0 && (kinds != 1 || nxt != -1 || low != 2 || under != 0 ||
                         top != 2 || counted != 0))) {
      fprintf(stderr,
              "FAIL mkinds two ship=%d dots=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d kept=%d\n",
              margin, dots_m, counted, greatest, top, sum, least, under, low, nxt,
              kinds, kept);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mkinds two-zero file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fprintf(pf, "...\t%s\n", peer);
      fclose(pf);
    }
    margin = ng_motive_label_margin("...", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    if (margin != 0 || counted != 0 || greatest != 0 || top != 2 || sum != 0 ||
        least != 0 || under != 0 || low != 2 || nxt != -1 || kinds != 1 ||
        kinds == low || kinds == top) {
      fprintf(stderr,
              "FAIL mkinds two-zero margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d\n",
              margin, counted, greatest, top, sum, least, under, low, nxt, kinds);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mkinds two-ship file opens");
    if (pf) {
      fputs(ship, pf);
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    kept = 2;
    if (margin < 0 || greatest != margin || least != margin || top != 2 ||
        counted != (margin > 0 ? 2 : 0) || sum != margin + margin || under != 0 ||
        low != 2 || nxt != -1 || kinds != 1 || kinds == nxt || kinds == low ||
        kinds == top || kinds == kept || kinds == (kept - low) ||
        kinds == (kept - top) ||
        (greatest > 1 && kinds == greatest)) {
      fprintf(stderr,
              "FAIL mkinds two-ship margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d\n",
              margin, counted, greatest, top, sum, least, under, low, nxt, kinds);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mkinds two-dots ship file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fprintf(pf, "...\t%s\n", peer);
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    dots_m = ng_motive_label_margin("...", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    if (dots_m != 0 || margin < 0 || least != 0 ||
        (margin > 0 && (kinds != 2 || nxt != margin || low != 2 || under != 2 ||
                        top != 1 || sum != margin || greatest != margin ||
                        counted != 1 || kinds == least)) ||
        (margin == 0 && (kinds != 1 || nxt != -1 || low != 3 || under != 0 ||
                         top != 3))) {
      fprintf(stderr,
              "FAIL mkinds below ship=%d dots=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d\n",
              margin, dots_m, counted, greatest, top, sum, least, under, low, nxt,
              kinds);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mkinds spread file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fputs(audit, pf);
      fputs(ship, pf);
      fclose(pf);
    }
    qs[0] = "...";
    qs[1] = "why did the ship fail";
    qs[2] = "ship fail";
    seen_m = 0;
    best_m = -1;
    least_m = 0;
    expect_u = 0;
    least_n = 0;
    mid_n = 0;
    expect_n = -1;
    expect_k = 0;
    for (si = 0; si < 3; si++) {
      int sj, dup = 0;
      ms[si] = ng_motive_label_margin(qs[si], path);
      if (ms[si] < 0) continue;
      if (!seen_m || ms[si] < least_m) least_m = ms[si];
      if (ms[si] > best_m) best_m = ms[si];
      for (sj = 0; sj < si; sj++) {
        if (ms[sj] == ms[si]) dup = 1;
      }
      if (!dup) expect_k++;
      seen_m++;
    }
    for (si = 0; si < 3; si++) {
      if (ms[si] < 0) continue;
      if (ms[si] < best_m) expect_u++;
      if (ms[si] == least_m) least_n++;
      if (ms[si] > least_m && ms[si] < best_m) mid_n++;
      if (ms[si] > least_m && (expect_n < 0 || ms[si] < expect_n))
        expect_n = ms[si];
    }
    under = ng_motive_margin_under(path);
    least = ng_motive_margin_min(path);
    greatest = ng_motive_margin_max(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    span = (seen_m > 0 && best_m >= least_m) ? (best_m - least_m) : -1;
    if ((seen_m == 0 && kinds != -1) ||
        (seen_m > 0 && (kinds != expect_k || under != expect_u || low != least_n ||
                        nxt != expect_n || least != least_m ||
                        greatest != best_m)) ||
        (seen_m > 0 && least_m == best_m && (kinds != 1 || nxt != -1)) ||
        (seen_m > 0 && span != expect_k && kinds == span) ||
        (expect_n >= 0 && expect_n != expect_k && kinds == expect_n) ||
        (least_n != expect_k && kinds == least_n) ||
        (seen_m != expect_k && kinds == seen_m)) {
      fprintf(stderr,
              "FAIL mkinds spread kinds=%d expect=%d next=%d next_e=%d low=%d least_n=%d under=%d expect_u=%d min=%d least=%d max=%d best=%d mid=%d span=%d margins=%d,%d,%d\n",
              kinds, expect_k, nxt, expect_n, low, least_n, under, expect_u, least,
              least_m, greatest, best_m, mid_n, span, ms[0], ms[1], ms[2]);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mkinds sealed file opens");
    if (pf) {
      fputs(ship, pf);
      fclose(pf);
    }
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    setenv("NANOBOT_MOTIVE_LEARN", path, 1);
    ng_motive_rank("ship fail", &ranked);
    if (!(strcmp(ranked.concept, keep) == 0 && ranked.lexical == 2 &&
          ranked.hamming == 22 && ranked.rank == 170 && ranked.lex_w == 64)) {
      fprintf(stderr,
              "FAIL mkinds sealed concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              ranked.concept, ranked.lexical, ranked.hamming, ranked.rank,
              ranked.lex_w);
      g_fail++;
    }
    ng_motive_rank_w("ship fail", &ranked, 64);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lexical == 1 && ranked.hamming == 23 && ranked.lex_w == 64,
           "weight 64 still returns the pair line");
    unsetenv("NANOBOT_MOTIVE_LEARN");
    ng_motive_rank("ship fail", &ranked);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lex_w == 64,
           "unset learn still leaves the pair line");
    ng_motive_rank(law, &ranked);
    if (!(strcmp(ranked.concept, law) == 0 && ranked.rank == 256 &&
          ranked.lexical == 3 && ranked.hamming == 0 && ranked.lex_w == 64)) {
      fprintf(stderr,
              "FAIL mkinds sealed law concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              ranked.concept, ranked.lexical, ranked.hamming, ranked.rank,
              ranked.lex_w);
      g_fail++;
    }
    learned = ng_motive_learn_hits(path);
    marked = ng_motive_label_hits(path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    if (learned != 0 || marked != 1 || counted != 1 || greatest != 65 ||
        top != 1 || sum != 65 || least != 65 || under != 0 || low != 1 ||
        nxt != -1 || kinds != 1 || kinds == greatest || sum != greatest ||
        least != greatest) {
      fprintf(stderr,
              "FAIL mkinds sealed counts learn=%d label=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d\n",
              learned, marked, counted, greatest, top, sum, least, under, low, nxt,
              kinds);
      g_fail++;
    }
    ng_motive_rank("you lied", &ranked);
    expect(ranked.plate == NG_PLATE_CONFESS && ranked.lexical == 0,
           "mkinds leaves you lied");
    ng_motive_rank("", &ranked);
    expect(ranked.rank == 0, "mkinds leaves an empty query at rank 0");
    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    unlink(path);
  }

  {
    const char *path = "/tmp/nb-motive-mmode.pairs";
    const char *dirp = "/tmp/nb-motive-mmode.dir";
    const char *keep = "keep free energy flowing";
    const char *pair = "pair vote then ship the step";
    const char *law = "read the law and prove";
    const char *audit_c = "audit the last code change";
    const char *peer = "prompt a peer to move";
    const char *ship = "ship fail\tkeep free energy flowing\n";
    const char *audit = "why did the ship fail\taudit the last code change\n";
    const char *padded = "why did the ship fail\tkeep free energy flowing  \n";
    const char *trail = "ship fail\tkeep free energy flowing \n";
    const char *qs[3];
    char longp[520];
    char qbuf[200];
    char cbuf[96];
    char blob[16384];
    char big[420];
    FILE *pf;
    ng_motive m, ranked;
    struct stat st;
    struct timespec ts[2];
    int rc, margin, counted, learned, marked, greatest, dots_m, top, sum, least;
    int under, low, nxt, kinds, mode;
    int ms[3];
    int si, seen_m, best_m, least_m, expect_u, least_n, mid_n, kept, expect_n;
    int span, expect_k, expect_mode, mode_n;

    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    expect(ng_motive_margin_mode(NULL) == -1, "a null mmode path returns -1");
    expect(ng_motive_margin_mode("") == -1, "an empty mmode path returns -1");
    memset(longp, 'm', 512);
    longp[512] = 0;
    expect(strlen(longp) == 512, "the long mmode path is 512 bytes");
    expect(ng_motive_margin_mode(longp) == -1,
           "an mmode path of 512 bytes returns -1");
    expect(ng_motive_margin_mode("/tmp/nb-motive-mmode-missing") == -1,
           "a missing mmode file returns -1");
    unlink(dirp);
    rmdir(dirp);
    expect(mkdir(dirp, 0700) == 0, "mmode directory creates");
    expect(ng_motive_margin_mode(dirp) == -1,
           "a non-regular mmode path returns -1");
    rmdir(dirp);

    pf = fopen(path, "w");
    expect(pf != NULL, "mmode empty file opens");
    if (pf) {
      fputs("# note\n\n", pf);
      fclose(pf);
    }
    expect(ng_motive_margin_mode(path) == -1,
           "an mmode file with no pair returns -1");

    memset(qbuf, 'q', 192);
    qbuf[192] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mmode overlong query file opens");
    if (pf) {
      fprintf(pf, "%s\t%s\n", qbuf, keep);
      fclose(pf);
    }
    expect(ng_motive_margin_mode(path) == -1,
           "an mmode query of 192 bytes is not a pair");
    qbuf[191] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mmode bounded query file opens");
    if (pf) {
      fprintf(pf, "%s\t%s\n", qbuf, keep);
      fclose(pf);
    }
    margin = ng_motive_label_margin(qbuf, path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    mode = ng_motive_margin_mode(path);
    if (margin < 0 || counted != (margin > 0 ? 1 : 0) || greatest != margin ||
        top != 1 || sum != margin || least != margin || under != 0 ||
        low != 1 || nxt != -1 || kinds != 1 || mode != margin) {
      fprintf(stderr,
              "FAIL mmode bounded margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d mode=%d\n",
              margin, counted, greatest, top, sum, least, under, low, nxt, kinds,
              mode);
      g_fail++;
    }

    memset(cbuf, 'c', 80);
    cbuf[80] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mmode overlong catalog file opens");
    if (pf) {
      fprintf(pf, "ship fail\t%s\n", cbuf);
      fclose(pf);
    }
    expect(ng_motive_margin_mode(path) == -1,
           "an mmode catalog line of 80 bytes is not a pair");
    cbuf[79] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mmode bounded catalog file opens");
    if (pf) {
      fprintf(pf, "ship fail\t%s\n", cbuf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    mode = ng_motive_margin_mode(path);
    if (margin < 0 || counted != (margin > 0 ? 1 : 0) || greatest != margin ||
        top != 1 || sum != margin || least != margin || under != 0 ||
        low != 1 || nxt != -1 || kinds != 1 || mode != margin) {
      fprintf(stderr,
              "FAIL mmode catalog margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d mode=%d\n",
              margin, counted, greatest, top, sum, least, under, low, nxt, kinds,
              mode);
      g_fail++;
    }

    memset(blob, 'x', sizeof blob);
    pf = fopen(path, "w");
    expect(pf != NULL, "mmode budget file opens");
    if (pf) {
      fwrite(blob, 1, sizeof blob, pf);
      fputc('\n', pf);
      fputs(ship, pf);
      fclose(pf);
    }
    expect(ng_motive_margin_mode(path) == -1,
           "an mmode pair past the 16384 byte budget is not read");

    memset(big, 'z', 400);
    big[400] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mmode overlong line file opens");
    if (pf) {
      fprintf(pf, "%s\n", big);
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    mode = ng_motive_margin_mode(path);
    if (margin != 65 || counted != 1 || greatest != 65 || top != 1 ||
        sum != 65 || least != 65 || under != 0 || low != 1 || nxt != -1 ||
        kinds != 1 || mode != 65 || mode == nxt || mode == kinds) {
      fprintf(stderr,
              "FAIL mmode skip margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d mode=%d\n",
              margin, counted, greatest, top, sum, least, under, low, nxt, kinds,
              mode);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mmode ship file opens");
    if (pf) {
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    mode = ng_motive_margin_mode(path);
    if (margin != 65 || counted != 1 || greatest != 65 || top != 1 ||
        sum != 65 || least != 65 || under != 0 || low != 1 || nxt != -1 ||
        kinds != 1 || mode != 65 || mode == nxt || mode == kinds ||
        mode == counted || mode == (greatest - least)) {
      fprintf(stderr,
              "FAIL mmode ship margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d mode=%d\n",
              margin, counted, greatest, top, sum, least, under, low, nxt, kinds,
              mode);
      g_fail++;
    }
    setenv("NANOBOT_MOTIVE_LEARN", "/tmp/nb-motive-mmode-missing", 1);
    setenv("NANOBOT_MOTIVE_LEX_W", "1", 1);
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    mode = ng_motive_margin_mode(path);
    if (margin != 65 || counted != 1 || greatest != 65 || top != 1 ||
        sum != 65 || least != 65 || under != 0 || low != 1 || nxt != -1 ||
        kinds != 1 || mode != 65 || mode == nxt) {
      fprintf(stderr,
              "FAIL mmode env margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d mode=%d\n",
              margin, counted, greatest, top, sum, least, under, low, nxt, kinds,
              mode);
      g_fail++;
    }
    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    margin = ng_motive_label_margin(law, path);
    if (margin <= 0) {
      fprintf(stderr, "FAIL mmode law margin %d\n", margin);
      g_fail++;
    }
    rc = ng_motive_label_rank(law, path, &m);
    if (!(rc == 0 && strcmp(m.concept, law) == 0 && m.lexical == 3 &&
          m.hamming == 0 && m.rank == 256 && m.lex_w == 64)) {
      fprintf(stderr,
              "FAIL mmode law rank rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }
    expect(getenv("NANOBOT_MOTIVE_LEARN") == NULL, "mmode leaves LEARN unset");
    ng_motive_rank("ship fail", &ranked);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lexical == 1 && ranked.hamming == 23 && ranked.lex_w == 64,
           "mmode leaves ship fail on the pair line");
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    mode = ng_motive_margin_mode(path);
    if (greatest != 65 || top != 1 || sum != 65 || least != 65 || under != 0 ||
        low != 1 || nxt != -1 || kinds != 1 || mode != 65 || mode == nxt ||
        mode == kinds) {
      fprintf(stderr,
              "FAIL mmode ship stays max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d mode=%d\n",
              greatest, top, sum, least, under, low, nxt, kinds, mode);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mmode audit file opens");
    if (pf) {
      fputs(audit, pf);
      fclose(pf);
    }
    rc = ng_motive_label_rank("why did the ship fail", path, &m);
    if (!(rc == 0 && strcmp(m.concept, audit_c) == 0 && m.lexical == 4 &&
          m.hamming == 18 && m.rank == 46 && m.lex_w == 0)) {
      fprintf(stderr,
              "FAIL mmode audit rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }
    margin = ng_motive_label_margin("why did the ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    mode = ng_motive_margin_mode(path);
    if (margin < 0 || (margin > 0 && counted != 1) ||
        (margin == 0 && counted != 0) || greatest != margin || top != 1 ||
        sum != margin || least != margin || under != 0 || low != 1 ||
        nxt != -1 || kinds != 1 || mode != margin) {
      fprintf(stderr,
              "FAIL mmode audit margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d mode=%d\n",
              margin, counted, greatest, top, sum, least, under, low, nxt, kinds,
              mode);
      g_fail++;
    }
    expect(strlen(audit) == strlen(padded), "mmode cache probe stays the same size");
    expect(stat(path, &st) == 0 && st.st_size == (off_t)strlen(audit),
           "mmode pairs stat");
    ts[0].tv_sec = st.st_atim.tv_sec;
    ts[0].tv_nsec = st.st_atim.tv_nsec;
    ts[1].tv_sec = st.st_mtim.tv_sec;
    ts[1].tv_nsec = st.st_mtim.tv_nsec;
    pf = fopen(path, "w");
    expect(pf != NULL, "mmode cache probe opens");
    if (pf) {
      fputs(padded, pf);
      fclose(pf);
    }
    expect(utimensat(AT_FDCWD, path, ts, 0) == 0, "mmode cache probe restores mtime");
    setenv("NANOBOT_MOTIVE_LEARN", path, 1);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    mode = ng_motive_margin_mode(path);
    expect(stat(path, &st) == 0 && st.st_mtim.tv_sec == ts[1].tv_sec &&
               st.st_mtim.tv_nsec == ts[1].tv_nsec &&
               st.st_size == (off_t)strlen(padded),
           "mmode does not write the pairs file");
    ng_motive_rank("why did the ship fail", &ranked);
    expect(ranked.lex_w == 64 && strcmp(ranked.concept, pair) == 0,
           "mmode does not write the learn cache");
    margin = ng_motive_label_margin("why did the ship fail", path);
    if (counted < 0 || margin < 0 || greatest != margin || nxt != -1 ||
        kinds != 1 || mode != margin) {
      fprintf(stderr,
              "FAIL mmode probe hits=%d margin=%d max=%d next=%d kinds=%d mode=%d\n",
              counted, margin, greatest, nxt, kinds, mode);
      g_fail++;
    }
    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");

    pf = fopen(path, "w");
    expect(pf != NULL, "mmode trail file opens");
    if (pf) {
      fputs(trail, pf);
      fclose(pf);
    }
    rc = ng_motive_label_rank("ship fail", path, &m);
    if (!(rc == 0 && strcmp(m.concept, pair) == 0 && m.lexical == 1 &&
          m.hamming == 23 && m.rank == 105 && m.lex_w == 64)) {
      fprintf(stderr,
              "FAIL mmode trail rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    mode = ng_motive_margin_mode(path);
    if (margin < 0 || (margin > 0 && counted != 1) ||
        (margin == 0 && counted != 0) || greatest != margin || top != 1 ||
        sum != margin || least != margin || under != 0 || low != 1 ||
        nxt != -1 || kinds != 1 || mode != margin) {
      fprintf(stderr,
              "FAIL mmode trail margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d mode=%d\n",
              margin, counted, greatest, top, sum, least, under, low, nxt, kinds,
              mode);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mmode dots file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fclose(pf);
    }
    margin = ng_motive_label_margin("...", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    mode = ng_motive_margin_mode(path);
    if (margin != 0 || counted != 0 || greatest != 0 || top != 1 || sum != 0 ||
        least != 0 || under != 0 || low != 1 || nxt != -1 || kinds != 1 ||
        mode != 0 || mode == nxt) {
      fprintf(stderr,
              "FAIL mmode dots margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d mode=%d\n",
              margin, counted, greatest, top, sum, least, under, low, nxt, kinds,
              mode);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mmode two file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    dots_m = ng_motive_label_margin("...", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    mode = ng_motive_margin_mode(path);
    kept = (dots_m >= 0 ? 1 : 0) + (margin >= 0 ? 1 : 0);
    if (dots_m != 0 || margin < 0 || least != 0 || sum != margin ||
        greatest != (margin > dots_m ? margin : dots_m) || kept != 2 ||
        (margin > 0 && (mode != margin || kinds != 2 || nxt != margin ||
                        low != 1 || under != 1 || top != 1 || counted != 1 ||
                        mode == least || mode == kinds || mode == low)) ||
        (margin == 0 && (mode != 0 || kinds != 1 || nxt != -1 || low != 2 ||
                         under != 0 || top != 2 || counted != 0))) {
      fprintf(stderr,
              "FAIL mmode two ship=%d dots=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d mode=%d kept=%d\n",
              margin, dots_m, counted, greatest, top, sum, least, under, low, nxt,
              kinds, mode, kept);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mmode two-zero file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fprintf(pf, "...\t%s\n", peer);
      fclose(pf);
    }
    margin = ng_motive_label_margin("...", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    mode = ng_motive_margin_mode(path);
    if (margin != 0 || counted != 0 || greatest != 0 || top != 2 || sum != 0 ||
        least != 0 || under != 0 || low != 2 || nxt != -1 || kinds != 1 ||
        mode != 0 || mode == kinds || mode == low || mode == top) {
      fprintf(stderr,
              "FAIL mmode two-zero margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d mode=%d\n",
              margin, counted, greatest, top, sum, least, under, low, nxt, kinds,
              mode);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mmode two-ship file opens");
    if (pf) {
      fputs(ship, pf);
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    mode = ng_motive_margin_mode(path);
    kept = 2;
    if (margin < 0 || greatest != margin || least != margin || top != 2 ||
        counted != (margin > 0 ? 2 : 0) || sum != margin + margin || under != 0 ||
        low != 2 || nxt != -1 || kinds != 1 || mode != margin || mode == nxt ||
        mode == kinds || mode == low || mode == top || mode == kept) {
      fprintf(stderr,
              "FAIL mmode two-ship margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d mode=%d\n",
              margin, counted, greatest, top, sum, least, under, low, nxt, kinds,
              mode);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mmode two-dots ship file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fprintf(pf, "...\t%s\n", peer);
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    dots_m = ng_motive_label_margin("...", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    mode = ng_motive_margin_mode(path);
    if (dots_m != 0 || margin < 0 || least != 0 ||
        (margin > 0 && (mode != 0 || kinds != 2 || nxt != margin || low != 2 ||
                        under != 2 || top != 1 || sum != margin ||
                        greatest != margin || counted != 1 || mode == nxt ||
                        mode == greatest || mode == kinds ||
                        mode == (greatest - least))) ||
        (margin == 0 && (mode != 0 || kinds != 1 || nxt != -1 || low != 3 ||
                         under != 0 || top != 3))) {
      fprintf(stderr,
              "FAIL mmode below ship=%d dots=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d mode=%d\n",
              margin, dots_m, counted, greatest, top, sum, least, under, low, nxt,
              kinds, mode);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mmode tie file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fprintf(pf, "...\t%s\n", peer);
      fputs(ship, pf);
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    dots_m = ng_motive_label_margin("...", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    mode = ng_motive_margin_mode(path);
    if (dots_m != 0 || margin < 0 ||
        (margin > 0 && (mode != margin || least != 0 || greatest != margin ||
                        kinds != 2 || low != 2 || top != 2 || nxt != margin ||
                        under != 2 || sum != margin + margin || counted != 2 ||
                        mode == least || mode == low || mode == kinds)) ||
        (margin == 0 && (mode != 0 || kinds != 1 || nxt != -1 || low != 4 ||
                         top != 4 || under != 0))) {
      fprintf(stderr,
              "FAIL mmode tie ship=%d dots=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d mode=%d\n",
              margin, dots_m, counted, greatest, top, sum, least, under, low, nxt,
              kinds, mode);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mmode majority file opens");
    if (pf) {
      fputs(ship, pf);
      fputs(ship, pf);
      fprintf(pf, "...\t%s\n", peer);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    dots_m = ng_motive_label_margin("...", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    mode = ng_motive_margin_mode(path);
    if (dots_m != 0 || margin < 0 || least != 0 ||
        (margin > 0 && (mode != margin || greatest != margin || kinds != 2 ||
                        low != 1 || top != 2 || under != 1 || nxt != margin ||
                        counted != 2 || sum != margin + margin || mode == least ||
                        mode == low || mode == kinds)) ||
        (margin == 0 && (mode != 0 || kinds != 1 || nxt != -1 || low != 3 ||
                         top != 3 || under != 0))) {
      fprintf(stderr,
              "FAIL mmode majority ship=%d dots=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d mode=%d\n",
              margin, dots_m, counted, greatest, top, sum, least, under, low, nxt,
              kinds, mode);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mmode spread file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fputs(audit, pf);
      fputs(ship, pf);
      fclose(pf);
    }
    qs[0] = "...";
    qs[1] = "why did the ship fail";
    qs[2] = "ship fail";
    seen_m = 0;
    best_m = -1;
    least_m = 0;
    expect_u = 0;
    least_n = 0;
    mid_n = 0;
    expect_n = -1;
    expect_k = 0;
    expect_mode = -1;
    mode_n = 0;
    for (si = 0; si < 3; si++) {
      int sj, dup = 0;
      ms[si] = ng_motive_label_margin(qs[si], path);
      if (ms[si] < 0) continue;
      if (!seen_m || ms[si] < least_m) least_m = ms[si];
      if (ms[si] > best_m) best_m = ms[si];
      for (sj = 0; sj < si; sj++) {
        if (ms[sj] == ms[si]) dup = 1;
      }
      if (!dup) expect_k++;
      seen_m++;
    }
    for (si = 0; si < 3; si++) {
      int sj, c = 0;
      if (ms[si] < 0) continue;
      if (ms[si] < best_m) expect_u++;
      if (ms[si] == least_m) least_n++;
      if (ms[si] > least_m && ms[si] < best_m) mid_n++;
      if (ms[si] > least_m && (expect_n < 0 || ms[si] < expect_n))
        expect_n = ms[si];
      for (sj = 0; sj < 3; sj++)
        if (ms[sj] == ms[si]) c++;
      if (c > mode_n || (c == mode_n && ms[si] > expect_mode)) {
        mode_n = c;
        expect_mode = ms[si];
      }
    }
    under = ng_motive_margin_under(path);
    least = ng_motive_margin_min(path);
    greatest = ng_motive_margin_max(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    mode = ng_motive_margin_mode(path);
    span = (seen_m > 0 && best_m >= least_m) ? (best_m - least_m) : -1;
    if ((seen_m == 0 && (kinds != -1 || mode != -1)) ||
        (seen_m > 0 && (mode != expect_mode || kinds != expect_k ||
                        under != expect_u || low != least_n || nxt != expect_n ||
                        least != least_m || greatest != best_m)) ||
        (seen_m > 0 && least_m == best_m && (kinds != 1 || nxt != -1 ||
                                              mode != least_m)) ||
        (seen_m > 0 && span != expect_mode && mode == span) ||
        (expect_n >= 0 && expect_n != expect_mode && mode == expect_n) ||
        (expect_k != expect_mode && mode == expect_k) ||
        (seen_m != expect_mode && mode == seen_m) ||
        (least_m != expect_mode && mode == least_m && least_n < mode_n)) {
      fprintf(stderr,
              "FAIL mmode spread mode=%d expect=%d n=%d kinds=%d expect_k=%d next=%d next_e=%d low=%d least_n=%d under=%d expect_u=%d min=%d least=%d max=%d best=%d mid=%d span=%d margins=%d,%d,%d\n",
              mode, expect_mode, mode_n, kinds, expect_k, nxt, expect_n, low,
              least_n, under, expect_u, least, least_m, greatest, best_m, mid_n,
              span, ms[0], ms[1], ms[2]);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mmode sealed file opens");
    if (pf) {
      fputs(ship, pf);
      fclose(pf);
    }
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    setenv("NANOBOT_MOTIVE_LEARN", path, 1);
    ng_motive_rank("ship fail", &ranked);
    if (!(strcmp(ranked.concept, keep) == 0 && ranked.lexical == 2 &&
          ranked.hamming == 22 && ranked.rank == 170 && ranked.lex_w == 64)) {
      fprintf(stderr,
              "FAIL mmode sealed concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              ranked.concept, ranked.lexical, ranked.hamming, ranked.rank,
              ranked.lex_w);
      g_fail++;
    }
    ng_motive_rank_w("ship fail", &ranked, 64);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lexical == 1 && ranked.hamming == 23 && ranked.lex_w == 64,
           "weight 64 still returns the pair line");
    unsetenv("NANOBOT_MOTIVE_LEARN");
    ng_motive_rank("ship fail", &ranked);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lex_w == 64,
           "unset learn still leaves the pair line");
    ng_motive_rank(law, &ranked);
    if (!(strcmp(ranked.concept, law) == 0 && ranked.rank == 256 &&
          ranked.lexical == 3 && ranked.hamming == 0 && ranked.lex_w == 64)) {
      fprintf(stderr,
              "FAIL mmode sealed law concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              ranked.concept, ranked.lexical, ranked.hamming, ranked.rank,
              ranked.lex_w);
      g_fail++;
    }
    learned = ng_motive_learn_hits(path);
    marked = ng_motive_label_hits(path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    mode = ng_motive_margin_mode(path);
    if (learned != 0 || marked != 1 || counted != 1 || greatest != 65 ||
        top != 1 || sum != 65 || least != 65 || under != 0 || low != 1 ||
        nxt != -1 || kinds != 1 || mode != 65 || mode == nxt || mode == kinds ||
        mode == counted) {
      fprintf(stderr,
              "FAIL mmode sealed counts learn=%d label=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d mode=%d\n",
              learned, marked, counted, greatest, top, sum, least, under, low, nxt,
              kinds, mode);
      g_fail++;
    }
    ng_motive_rank("you lied", &ranked);
    expect(ranked.plate == NG_PLATE_CONFESS && ranked.lexical == 0,
           "mmode leaves you lied");
    ng_motive_rank("", &ranked);
    expect(ranked.rank == 0, "mmode leaves an empty query at rank 0");
    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    unlink(path);
  }

  {
    const char *path = "/tmp/nb-motive-mlone.pairs";
    const char *dirp = "/tmp/nb-motive-mlone.dir";
    const char *keep = "keep free energy flowing";
    const char *pair = "pair vote then ship the step";
    const char *law = "read the law and prove";
    const char *audit_c = "audit the last code change";
    const char *peer = "prompt a peer to move";
    const char *ship = "ship fail\tkeep free energy flowing\n";
    const char *audit = "why did the ship fail\taudit the last code change\n";
    const char *padded = "why did the ship fail\tkeep free energy flowing  \n";
    const char *trail = "ship fail\tkeep free energy flowing \n";
    const char *qs[3];
    char longp[520];
    char qbuf[200];
    char cbuf[96];
    char blob[16384];
    char big[420];
    FILE *pf;
    ng_motive m, ranked;
    struct stat st;
    struct timespec ts[2];
    int rc, margin, counted, learned, marked, greatest, dots_m, top, sum, least;
    int under, low, nxt, kinds, mode, lone;
    int ms[3];
    int si, seen_m, best_m, least_m, expect_u, least_n, mid_n, kept, expect_n;
    int span, expect_k, expect_mode, mode_n, expect_lone;

    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    expect(ng_motive_margin_lone(NULL) == -1, "a null mlone path returns -1");
    expect(ng_motive_margin_lone("") == -1, "an empty mlone path returns -1");
    memset(longp, 'm', 512);
    longp[512] = 0;
    expect(strlen(longp) == 512, "the long mlone path is 512 bytes");
    expect(ng_motive_margin_lone(longp) == -1,
           "an mlone path of 512 bytes returns -1");
    expect(ng_motive_margin_lone("/tmp/nb-motive-mlone-missing") == -1,
           "a missing mlone file returns -1");
    unlink(dirp);
    rmdir(dirp);
    expect(mkdir(dirp, 0700) == 0, "mlone directory creates");
    expect(ng_motive_margin_lone(dirp) == -1,
           "a non-regular mlone path returns -1");
    rmdir(dirp);

    pf = fopen(path, "w");
    expect(pf != NULL, "mlone empty file opens");
    if (pf) {
      fputs("# note\n\n", pf);
      fclose(pf);
    }
    expect(ng_motive_margin_lone(path) == -1,
           "an mlone file with no pair returns -1");

    memset(qbuf, 'q', 192);
    qbuf[192] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mlone overlong query file opens");
    if (pf) {
      fprintf(pf, "%s\t%s\n", qbuf, keep);
      fclose(pf);
    }
    expect(ng_motive_margin_lone(path) == -1,
           "an mlone query of 192 bytes is not a pair");
    qbuf[191] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mlone bounded query file opens");
    if (pf) {
      fprintf(pf, "%s\t%s\n", qbuf, keep);
      fclose(pf);
    }
    margin = ng_motive_label_margin(qbuf, path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    mode = ng_motive_margin_mode(path);
    lone = ng_motive_margin_lone(path);
    if (margin < 0 || counted != (margin > 0 ? 1 : 0) || greatest != margin ||
        top != 1 || sum != margin || least != margin || under != 0 ||
        low != 1 || nxt != -1 || kinds != 1 || mode != margin ||
        lone != margin || lone == nxt) {
      fprintf(stderr,
              "FAIL mlone bounded margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d mode=%d\n",
              margin, counted, greatest, top, sum, least, under, low, nxt, kinds,
              mode);
      g_fail++;
    }

    memset(cbuf, 'c', 80);
    cbuf[80] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mlone overlong catalog file opens");
    if (pf) {
      fprintf(pf, "ship fail\t%s\n", cbuf);
      fclose(pf);
    }
    expect(ng_motive_margin_lone(path) == -1,
           "an mlone catalog line of 80 bytes is not a pair");
    cbuf[79] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mlone bounded catalog file opens");
    if (pf) {
      fprintf(pf, "ship fail\t%s\n", cbuf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    mode = ng_motive_margin_mode(path);
    lone = ng_motive_margin_lone(path);
    if (margin < 0 || counted != (margin > 0 ? 1 : 0) || greatest != margin ||
        top != 1 || sum != margin || least != margin || under != 0 ||
        low != 1 || nxt != -1 || kinds != 1 || mode != margin ||
        lone != margin || lone == nxt) {
      fprintf(stderr,
              "FAIL mlone catalog margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d mode=%d\n",
              margin, counted, greatest, top, sum, least, under, low, nxt, kinds,
              mode);
      g_fail++;
    }

    memset(blob, 'x', sizeof blob);
    pf = fopen(path, "w");
    expect(pf != NULL, "mlone budget file opens");
    if (pf) {
      fwrite(blob, 1, sizeof blob, pf);
      fputc('\n', pf);
      fputs(ship, pf);
      fclose(pf);
    }
    expect(ng_motive_margin_lone(path) == -1,
           "an mlone pair past the 16384 byte budget is not read");

    memset(big, 'z', 400);
    big[400] = 0;
    pf = fopen(path, "w");
    expect(pf != NULL, "mlone overlong line file opens");
    if (pf) {
      fprintf(pf, "%s\n", big);
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    mode = ng_motive_margin_mode(path);
    lone = ng_motive_margin_lone(path);
    if (margin != 65 || counted != 1 || greatest != 65 || top != 1 ||
        sum != 65 || least != 65 || under != 0 || low != 1 || nxt != -1 ||
        kinds != 1 || mode != 65 || mode == nxt || mode == kinds ||
        lone != 65 || lone == nxt || lone == kinds) {
      fprintf(stderr,
              "FAIL mlone skip margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d mode=%d\n",
              margin, counted, greatest, top, sum, least, under, low, nxt, kinds,
              mode);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mlone ship file opens");
    if (pf) {
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    mode = ng_motive_margin_mode(path);
    lone = ng_motive_margin_lone(path);
    if (margin != 65 || counted != 1 || greatest != 65 || top != 1 ||
        sum != 65 || least != 65 || under != 0 || low != 1 || nxt != -1 ||
        kinds != 1 || mode != 65 || mode == nxt || mode == kinds ||
        mode == counted || mode == (greatest - least) || lone != 65 ||
        lone == nxt || lone == kinds || lone == counted ||
        lone == (greatest - least)) {
      fprintf(stderr,
              "FAIL mlone ship margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d mode=%d\n",
              margin, counted, greatest, top, sum, least, under, low, nxt, kinds,
              mode);
      g_fail++;
    }
    setenv("NANOBOT_MOTIVE_LEARN", "/tmp/nb-motive-mlone-missing", 1);
    setenv("NANOBOT_MOTIVE_LEX_W", "1", 1);
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    mode = ng_motive_margin_mode(path);
    lone = ng_motive_margin_lone(path);
    if (margin != 65 || counted != 1 || greatest != 65 || top != 1 ||
        sum != 65 || least != 65 || under != 0 || low != 1 || nxt != -1 ||
        kinds != 1 || mode != 65 || mode == nxt || lone != 65 ||
        lone == nxt) {
      fprintf(stderr,
              "FAIL mlone env margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d mode=%d\n",
              margin, counted, greatest, top, sum, least, under, low, nxt, kinds,
              mode);
      g_fail++;
    }
    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    margin = ng_motive_label_margin(law, path);
    if (margin <= 0) {
      fprintf(stderr, "FAIL mlone law margin %d\n", margin);
      g_fail++;
    }
    rc = ng_motive_label_rank(law, path, &m);
    if (!(rc == 0 && strcmp(m.concept, law) == 0 && m.lexical == 3 &&
          m.hamming == 0 && m.rank == 256 && m.lex_w == 64)) {
      fprintf(stderr,
              "FAIL mlone law rank rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }
    expect(getenv("NANOBOT_MOTIVE_LEARN") == NULL, "mlone leaves LEARN unset");
    ng_motive_rank("ship fail", &ranked);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lexical == 1 && ranked.hamming == 23 && ranked.lex_w == 64,
           "mlone leaves ship fail on the pair line");
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    mode = ng_motive_margin_mode(path);
    lone = ng_motive_margin_lone(path);
    if (greatest != 65 || top != 1 || sum != 65 || least != 65 || under != 0 ||
        low != 1 || nxt != -1 || kinds != 1 || mode != 65 || mode == nxt ||
        mode == kinds || lone != 65 || lone == nxt || lone == kinds) {
      fprintf(stderr,
              "FAIL mlone ship stays max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d mode=%d\n",
              greatest, top, sum, least, under, low, nxt, kinds, mode);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mlone audit file opens");
    if (pf) {
      fputs(audit, pf);
      fclose(pf);
    }
    rc = ng_motive_label_rank("why did the ship fail", path, &m);
    if (!(rc == 0 && strcmp(m.concept, audit_c) == 0 && m.lexical == 4 &&
          m.hamming == 18 && m.rank == 46 && m.lex_w == 0)) {
      fprintf(stderr,
              "FAIL mlone audit rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }
    margin = ng_motive_label_margin("why did the ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    mode = ng_motive_margin_mode(path);
    lone = ng_motive_margin_lone(path);
    if (margin < 0 || (margin > 0 && counted != 1) ||
        (margin == 0 && counted != 0) || greatest != margin || top != 1 ||
        sum != margin || least != margin || under != 0 || low != 1 ||
        nxt != -1 || kinds != 1 || mode != margin || lone != margin ||
        lone == nxt) {
      fprintf(stderr,
              "FAIL mlone audit margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d mode=%d\n",
              margin, counted, greatest, top, sum, least, under, low, nxt, kinds,
              mode);
      g_fail++;
    }
    expect(strlen(audit) == strlen(padded), "mlone cache probe stays the same size");
    expect(stat(path, &st) == 0 && st.st_size == (off_t)strlen(audit),
           "mlone pairs stat");
    ts[0].tv_sec = st.st_atim.tv_sec;
    ts[0].tv_nsec = st.st_atim.tv_nsec;
    ts[1].tv_sec = st.st_mtim.tv_sec;
    ts[1].tv_nsec = st.st_mtim.tv_nsec;
    pf = fopen(path, "w");
    expect(pf != NULL, "mlone cache probe opens");
    if (pf) {
      fputs(padded, pf);
      fclose(pf);
    }
    expect(utimensat(AT_FDCWD, path, ts, 0) == 0, "mlone cache probe restores mtime");
    setenv("NANOBOT_MOTIVE_LEARN", path, 1);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    mode = ng_motive_margin_mode(path);
    lone = ng_motive_margin_lone(path);
    expect(stat(path, &st) == 0 && st.st_mtim.tv_sec == ts[1].tv_sec &&
               st.st_mtim.tv_nsec == ts[1].tv_nsec &&
               st.st_size == (off_t)strlen(padded),
           "mlone does not write the pairs file");
    ng_motive_rank("why did the ship fail", &ranked);
    expect(ranked.lex_w == 64 && strcmp(ranked.concept, pair) == 0,
           "mlone does not write the learn cache");
    margin = ng_motive_label_margin("why did the ship fail", path);
    if (counted < 0 || margin < 0 || greatest != margin || nxt != -1 ||
        kinds != 1 || mode != margin || lone != margin) {
      fprintf(stderr,
              "FAIL mlone probe hits=%d margin=%d max=%d next=%d kinds=%d mode=%d\n",
              counted, margin, greatest, nxt, kinds, mode);
      g_fail++;
    }
    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");

    pf = fopen(path, "w");
    expect(pf != NULL, "mlone trail file opens");
    if (pf) {
      fputs(trail, pf);
      fclose(pf);
    }
    rc = ng_motive_label_rank("ship fail", path, &m);
    if (!(rc == 0 && strcmp(m.concept, pair) == 0 && m.lexical == 1 &&
          m.hamming == 23 && m.rank == 105 && m.lex_w == 64)) {
      fprintf(stderr,
              "FAIL mlone trail rc=%d concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              rc, m.concept, m.lexical, m.hamming, m.rank, m.lex_w);
      g_fail++;
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    mode = ng_motive_margin_mode(path);
    lone = ng_motive_margin_lone(path);
    if (margin < 0 || (margin > 0 && counted != 1) ||
        (margin == 0 && counted != 0) || greatest != margin || top != 1 ||
        sum != margin || least != margin || under != 0 || low != 1 ||
        nxt != -1 || kinds != 1 || mode != margin || lone != margin ||
        lone == nxt) {
      fprintf(stderr,
              "FAIL mlone trail margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d mode=%d\n",
              margin, counted, greatest, top, sum, least, under, low, nxt, kinds,
              mode);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mlone dots file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fclose(pf);
    }
    margin = ng_motive_label_margin("...", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    mode = ng_motive_margin_mode(path);
    lone = ng_motive_margin_lone(path);
    if (margin != 0 || counted != 0 || greatest != 0 || top != 1 || sum != 0 ||
        least != 0 || under != 0 || low != 1 || nxt != -1 || kinds != 1 ||
        mode != 0 || mode == nxt || lone != 0 || lone == nxt) {
      fprintf(stderr,
              "FAIL mlone dots margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d mode=%d\n",
              margin, counted, greatest, top, sum, least, under, low, nxt, kinds,
              mode);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mlone two file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    dots_m = ng_motive_label_margin("...", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    mode = ng_motive_margin_mode(path);
    lone = ng_motive_margin_lone(path);
    kept = (dots_m >= 0 ? 1 : 0) + (margin >= 0 ? 1 : 0);
    if (dots_m != 0 || margin < 0 || least != 0 || sum != margin ||
        greatest != (margin > dots_m ? margin : dots_m) || kept != 2 ||
        (margin > 0 && (mode != margin || kinds != 2 || nxt != margin ||
                        low != 1 || under != 1 || top != 1 || counted != 1 ||
                        mode == least || mode == kinds || mode == low ||
                        lone != 0 || lone == mode)) ||
        (margin == 0 && (mode != 0 || kinds != 1 || nxt != -1 || low != 2 ||
                         under != 0 || top != 2 || counted != 0 ||
                         lone != -1 || lone == mode))) {
      fprintf(stderr,
              "FAIL mlone two ship=%d dots=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d mode=%d kept=%d\n",
              margin, dots_m, counted, greatest, top, sum, least, under, low, nxt,
              kinds, mode, kept);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mlone two-zero file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fprintf(pf, "...\t%s\n", peer);
      fclose(pf);
    }
    margin = ng_motive_label_margin("...", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    mode = ng_motive_margin_mode(path);
    lone = ng_motive_margin_lone(path);
    if (margin != 0 || counted != 0 || greatest != 0 || top != 2 || sum != 0 ||
        least != 0 || under != 0 || low != 2 || nxt != -1 || kinds != 1 ||
        mode != 0 || mode == kinds || mode == low || mode == top ||
        lone != -1 || lone == mode) {
      fprintf(stderr,
              "FAIL mlone two-zero margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d mode=%d\n",
              margin, counted, greatest, top, sum, least, under, low, nxt, kinds,
              mode);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mlone two-ship file opens");
    if (pf) {
      fputs(ship, pf);
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    mode = ng_motive_margin_mode(path);
    lone = ng_motive_margin_lone(path);
    kept = 2;
    if (margin < 0 || greatest != margin || least != margin || top != 2 ||
        counted != (margin > 0 ? 2 : 0) || sum != margin + margin || under != 0 ||
        low != 2 || nxt != -1 || kinds != 1 || mode != margin || mode == nxt ||
        mode == kinds || mode == low || mode == top || mode == kept ||
        lone != -1 || lone == mode) {
      fprintf(stderr,
              "FAIL mlone two-ship margin=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d mode=%d\n",
              margin, counted, greatest, top, sum, least, under, low, nxt, kinds,
              mode);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mlone two-dots ship file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fprintf(pf, "...\t%s\n", peer);
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    dots_m = ng_motive_label_margin("...", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    mode = ng_motive_margin_mode(path);
    lone = ng_motive_margin_lone(path);
    if (dots_m != 0 || margin < 0 || least != 0 ||
        (margin > 0 && (mode != 0 || kinds != 2 || nxt != margin || low != 2 ||
                        under != 2 || top != 1 || sum != margin ||
                        greatest != margin || counted != 1 || mode == nxt ||
                        mode == greatest || mode == kinds ||
                        mode == (greatest - least) || lone != margin ||
                        lone == mode || lone == least)) ||
        (margin == 0 && (mode != 0 || kinds != 1 || nxt != -1 || low != 3 ||
                         under != 0 || top != 3 || lone != -1 ||
                         lone == mode))) {
      fprintf(stderr,
              "FAIL mlone below ship=%d dots=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d mode=%d\n",
              margin, dots_m, counted, greatest, top, sum, least, under, low, nxt,
              kinds, mode);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mlone tie file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fprintf(pf, "...\t%s\n", peer);
      fputs(ship, pf);
      fputs(ship, pf);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    dots_m = ng_motive_label_margin("...", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    mode = ng_motive_margin_mode(path);
    lone = ng_motive_margin_lone(path);
    if (dots_m != 0 || margin < 0 ||
        (margin > 0 && (mode != margin || least != 0 || greatest != margin ||
                        kinds != 2 || low != 2 || top != 2 || nxt != margin ||
                        under != 2 || sum != margin + margin || counted != 2 ||
                        mode == least || mode == low || mode == kinds ||
                        lone != -1 || lone == mode)) ||
        (margin == 0 && (mode != 0 || kinds != 1 || nxt != -1 || low != 4 ||
                         top != 4 || under != 0 || lone != -1))) {
      fprintf(stderr,
              "FAIL mlone tie ship=%d dots=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d mode=%d\n",
              margin, dots_m, counted, greatest, top, sum, least, under, low, nxt,
              kinds, mode);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mlone majority file opens");
    if (pf) {
      fputs(ship, pf);
      fputs(ship, pf);
      fprintf(pf, "...\t%s\n", peer);
      fclose(pf);
    }
    margin = ng_motive_label_margin("ship fail", path);
    dots_m = ng_motive_label_margin("...", path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    mode = ng_motive_margin_mode(path);
    lone = ng_motive_margin_lone(path);
    if (dots_m != 0 || margin < 0 || least != 0 ||
        (margin > 0 && (mode != margin || greatest != margin || kinds != 2 ||
                        low != 1 || top != 2 || under != 1 || nxt != margin ||
                        counted != 2 || sum != margin + margin || mode == least ||
                        mode == low || mode == kinds || lone != 0 ||
                        lone == mode)) ||
        (margin == 0 && (mode != 0 || kinds != 1 || nxt != -1 || low != 3 ||
                         top != 3 || under != 0 || lone != -1))) {
      fprintf(stderr,
              "FAIL mlone majority ship=%d dots=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d mode=%d\n",
              margin, dots_m, counted, greatest, top, sum, least, under, low, nxt,
              kinds, mode);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mlone spread file opens");
    if (pf) {
      fprintf(pf, "...\t%s\n", peer);
      fputs(audit, pf);
      fputs(ship, pf);
      fclose(pf);
    }
    qs[0] = "...";
    qs[1] = "why did the ship fail";
    qs[2] = "ship fail";
    seen_m = 0;
    best_m = -1;
    least_m = 0;
    expect_u = 0;
    least_n = 0;
    mid_n = 0;
    expect_n = -1;
    expect_k = 0;
    expect_mode = -1;
    mode_n = 0;
    expect_lone = -1;
    for (si = 0; si < 3; si++) {
      int sj, dup = 0;
      ms[si] = ng_motive_label_margin(qs[si], path);
      if (ms[si] < 0) continue;
      if (!seen_m || ms[si] < least_m) least_m = ms[si];
      if (ms[si] > best_m) best_m = ms[si];
      for (sj = 0; sj < si; sj++) {
        if (ms[sj] == ms[si]) dup = 1;
      }
      if (!dup) expect_k++;
      seen_m++;
    }
    for (si = 0; si < 3; si++) {
      int sj, c = 0;
      if (ms[si] < 0) continue;
      if (ms[si] < best_m) expect_u++;
      if (ms[si] == least_m) least_n++;
      if (ms[si] > least_m && ms[si] < best_m) mid_n++;
      if (ms[si] > least_m && (expect_n < 0 || ms[si] < expect_n))
        expect_n = ms[si];
      for (sj = 0; sj < 3; sj++)
        if (ms[sj] == ms[si]) c++;
      if (c > mode_n || (c == mode_n && ms[si] > expect_mode)) {
        mode_n = c;
        expect_mode = ms[si];
      }
      if (c == 1 && (expect_lone < 0 || ms[si] < expect_lone))
        expect_lone = ms[si];
    }
    under = ng_motive_margin_under(path);
    least = ng_motive_margin_min(path);
    greatest = ng_motive_margin_max(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    mode = ng_motive_margin_mode(path);
    lone = ng_motive_margin_lone(path);
    span = (seen_m > 0 && best_m >= least_m) ? (best_m - least_m) : -1;
    if ((seen_m == 0 && (kinds != -1 || mode != -1 || lone != -1)) ||
        (seen_m > 0 && (mode != expect_mode || kinds != expect_k ||
                        under != expect_u || low != least_n || nxt != expect_n ||
                        least != least_m || greatest != best_m ||
                        lone != expect_lone)) ||
        (seen_m > 0 && least_m == best_m && (kinds != 1 || nxt != -1 ||
                                              mode != least_m ||
                                              (seen_m == 1 && lone != least_m) ||
                                              (seen_m > 1 && lone != -1))) ||
        (seen_m > 0 && span != expect_mode && mode == span) ||
        (seen_m > 0 && span != expect_lone && lone == span) ||
        (expect_n >= 0 && expect_n != expect_mode && mode == expect_n) ||
        (expect_n >= 0 && expect_n != expect_lone && lone == expect_n) ||
        (expect_k != expect_mode && mode == expect_k) ||
        (expect_k != expect_lone && lone == expect_k) ||
        (seen_m != expect_mode && mode == seen_m) ||
        (seen_m != expect_lone && lone == seen_m) ||
        (least_m != expect_mode && mode == least_m && least_n < mode_n) ||
        (least_m != expect_lone && lone == least_m) ||
        (expect_mode != expect_lone && lone == expect_mode)) {
      fprintf(stderr,
              "FAIL mlone spread mode=%d expect=%d n=%d kinds=%d expect_k=%d next=%d next_e=%d low=%d least_n=%d under=%d expect_u=%d min=%d least=%d max=%d best=%d mid=%d span=%d lone=%d expect_lone=%d margins=%d,%d,%d\n",
              mode, expect_mode, mode_n, kinds, expect_k, nxt, expect_n, low,
              least_n, under, expect_u, least, least_m, greatest, best_m, mid_n,
              span, lone, expect_lone, ms[0], ms[1], ms[2]);
      g_fail++;
    }

    pf = fopen(path, "w");
    expect(pf != NULL, "mlone sealed file opens");
    if (pf) {
      fputs(ship, pf);
      fclose(pf);
    }
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    setenv("NANOBOT_MOTIVE_LEARN", path, 1);
    ng_motive_rank("ship fail", &ranked);
    if (!(strcmp(ranked.concept, keep) == 0 && ranked.lexical == 2 &&
          ranked.hamming == 22 && ranked.rank == 170 && ranked.lex_w == 64)) {
      fprintf(stderr,
              "FAIL mlone sealed concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              ranked.concept, ranked.lexical, ranked.hamming, ranked.rank,
              ranked.lex_w);
      g_fail++;
    }
    ng_motive_rank_w("ship fail", &ranked, 64);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lexical == 1 && ranked.hamming == 23 && ranked.lex_w == 64,
           "weight 64 still returns the pair line");
    unsetenv("NANOBOT_MOTIVE_LEARN");
    ng_motive_rank("ship fail", &ranked);
    expect(strcmp(ranked.concept, pair) == 0 && ranked.rank == 105 &&
               ranked.lex_w == 64,
           "unset learn still leaves the pair line");
    ng_motive_rank(law, &ranked);
    if (!(strcmp(ranked.concept, law) == 0 && ranked.rank == 256 &&
          ranked.lexical == 3 && ranked.hamming == 0 && ranked.lex_w == 64)) {
      fprintf(stderr,
              "FAIL mlone sealed law concept=%s lex=%d ham=%d rank=%d lex_w=%d\n",
              ranked.concept, ranked.lexical, ranked.hamming, ranked.rank,
              ranked.lex_w);
      g_fail++;
    }
    learned = ng_motive_learn_hits(path);
    marked = ng_motive_label_hits(path);
    counted = ng_motive_margin_hits(path);
    greatest = ng_motive_margin_max(path);
    top = ng_motive_margin_top(path);
    sum = ng_motive_margin_sum(path);
    least = ng_motive_margin_min(path);
    under = ng_motive_margin_under(path);
    low = ng_motive_margin_low(path);
    nxt = ng_motive_margin_next(path);
    kinds = ng_motive_margin_kinds(path);
    mode = ng_motive_margin_mode(path);
    lone = ng_motive_margin_lone(path);
    if (learned != 0 || marked != 1 || counted != 1 || greatest != 65 ||
        top != 1 || sum != 65 || least != 65 || under != 0 || low != 1 ||
        nxt != -1 || kinds != 1 || mode != 65 || mode == nxt || mode == kinds ||
        mode == counted || lone != 65 || lone == nxt || lone == kinds ||
        lone == counted) {
      fprintf(stderr,
              "FAIL mlone sealed counts learn=%d label=%d hits=%d max=%d top=%d sum=%d min=%d under=%d low=%d next=%d kinds=%d mode=%d\n",
              learned, marked, counted, greatest, top, sum, least, under, low, nxt,
              kinds, mode);
      g_fail++;
    }
    ng_motive_rank("you lied", &ranked);
    expect(ranked.plate == NG_PLATE_CONFESS && ranked.lexical == 0,
           "mlone leaves you lied");
    ng_motive_rank("", &ranked);
    expect(ranked.rank == 0, "mlone leaves an empty query at rank 0");
    unsetenv("NANOBOT_MOTIVE_LEARN");
    unsetenv("NANOBOT_MOTIVE_LEX_W");
    unlink(path);
  }

  if (g_fail) {
    fprintf(stderr, "%d motive checks failed\n", g_fail);
    return 1;
  }
  printf("motive ok\n");
  return 0;
}
