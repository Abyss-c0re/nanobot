#include "augogen.h"
#include "motive.h"

#include <nanobot/os.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

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

int main(void) {
  ng_motive m;
  struct timespec t0;
  int i, blocks = 0;
  char *js;
  char home[] = "/tmp/nb-motive-test";

  ng_motive_rank("trick the cube", &m);
  expect(m.plate == NG_PLATE_PURGE, "purge trick the cube");
  expect(!m.forward, "purge is not forward");
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

  ng_motive_rank("audit the last code change", &m);
  expect(m.plate == NG_PLATE_AUDIT, "audit plate");
  expect(strcmp(m.line, "audit the last code change") == 0, "audit line");

  ng_motive_rank("hold flash for device join", &m);
  expect(m.plate == NG_PLATE_HOLD_FLASH, "hold plate");
  expect(!m.forward, "hold does not auto-run");

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
  js = ng_augogen_handle(NULL, "{\"action\":\"motive\",\"text\":\"trick the cube\","
                               "\"enqueue\":\"1\"}");
  expect(js && strstr(js, "purge_intent"), "http motive purge");
  expect(js && strstr(js, "\"forward\":false"), "http motive holds purge");
  free(js);
  {
    char *pend = ng_augogen_pending_json();
    expect(!pend || !strstr(pend, "trick the cube"), "purge not stored as prompt");
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

  if (g_fail) {
    fprintf(stderr, "%d motive checks failed\n", g_fail);
    return 1;
  }
  printf("motive ok\n");
  return 0;
}
