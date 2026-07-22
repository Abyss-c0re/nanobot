#include "sched.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/file.h>

static int g_enabled = 0;
static int g_fd = -1;

void ng_llm_sched_set_enabled(int on) { g_enabled = on ? 1 : 0; }
int ng_llm_sched_enabled(void) { return g_enabled; }

static const char *lock_path(void) {
  static char path[512];
  const char *home = getenv("NANOBOT_HOME");
  if (!home || !home[0]) home = getenv("HOME");
  if (!home || !home[0]) home = "/tmp";
  snprintf(path, sizeof path, "%s/llm_sched.lock", home);
  return path;
}

void ng_llm_sched_acquire(void) {
  if (!g_enabled) return;
  if (g_fd < 0) {
    g_fd = open(lock_path(), O_CREAT | O_RDWR, 0600);
    if (g_fd < 0) return;
  }
  /* Block until exclusive — serializes curl across forks */
  while (flock(g_fd, LOCK_EX) != 0) {
    if (errno == EINTR) continue;
    break;
  }
}

void ng_llm_sched_release(void) {
  if (!g_enabled || g_fd < 0) return;
  flock(g_fd, LOCK_UN);
}

char *ng_llm_sched_run(ng_llm_job_fn fn, void *userdata) {
  if (!fn) return NULL;
  ng_llm_sched_acquire();
  char *r = fn(userdata);
  ng_llm_sched_release();
  return r;
}
