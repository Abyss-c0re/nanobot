#include "ng_sched.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>

#define NG_LLM_SLOTS_MAX 8

static int g_enabled = 0;
static int g_slots = 1;
static int g_fd[NG_LLM_SLOTS_MAX];
static int g_held = -1; /* slot index held by this process, or -1 */
static char g_path_override[512];
static char g_base_path[512];
static int g_inited;

static void ensure_defaults(void) {
  int i;
  if (g_inited) return;
  g_inited = 1;
  for (i = 0; i < NG_LLM_SLOTS_MAX; i++) g_fd[i] = -1;
  {
    const char *e = getenv("NANOBOT_LLM_SLOTS");
    if (e && e[0]) {
      int n = atoi(e);
      if (n < 1) n = 1;
      if (n > NG_LLM_SLOTS_MAX) n = NG_LLM_SLOTS_MAX;
      g_slots = n;
    }
  }
  {
    const char *e = getenv("NANOBOT_LLM_SERIAL");
    if (e && e[0]) {
      if (e[0] == '0' || e[0] == 'n' || e[0] == 'N' || e[0] == 'f' || e[0] == 'F')
        g_enabled = 0;
      else
        g_enabled = 1;
    }
  }
}

void ng_llm_sched_set_enabled(int on) {
  ensure_defaults();
  g_enabled = on ? 1 : 0;
}
int ng_llm_sched_enabled(void) {
  ensure_defaults();
  return g_enabled;
}

void ng_llm_sched_set_path(const char *path) {
  ensure_defaults();
  if (path && path[0])
    snprintf(g_path_override, sizeof g_path_override, "%s", path);
  else
    g_path_override[0] = 0;
}

void ng_llm_sched_set_slots(int n) {
  ensure_defaults();
  if (n < 1) n = 1;
  if (n > NG_LLM_SLOTS_MAX) n = NG_LLM_SLOTS_MAX;
  g_slots = n;
}

int ng_llm_sched_slots(void) {
  ensure_defaults();
  return g_slots;
}

static void compute_base(void) {
  const char *e = getenv("NANOBOT_LLM_LOCK");
  if (g_path_override[0]) {
    snprintf(g_base_path, sizeof g_base_path, "%s", g_path_override);
    return;
  }
  if (e && e[0]) {
    snprintf(g_base_path, sizeof g_base_path, "%s", e);
    return;
  }
  /* Shared across all nanobot homes on this machine (fleet + grokium). */
  e = getenv("XDG_RUNTIME_DIR");
  if (e && e[0]) {
    snprintf(g_base_path, sizeof g_base_path, "%s/nanobot-llm.lock", e);
    return;
  }
  snprintf(g_base_path, sizeof g_base_path, "/tmp/nanobot-llm-%d.lock", (int)getuid());
}

const char *ng_llm_sched_path(void) {
  ensure_defaults();
  compute_base();
  return g_base_path;
}

static void slot_path(int slot, char *out, size_t n) {
  compute_base();
  if (g_slots <= 1 || slot <= 0)
    snprintf(out, n, "%s", g_base_path);
  else
    snprintf(out, n, "%s.%d", g_base_path, slot);
}

static int open_slot(int slot) {
  char path[576];
  slot_path(slot, path, sizeof path);
  if (g_fd[slot] >= 0) return g_fd[slot];
  /* ensure parent dir exists for runtime path */
  {
    char *slash = strrchr(path, '/');
    if (slash && slash != path) {
      char dir[576];
      size_t dlen = (size_t)(slash - path);
      if (dlen < sizeof dir) {
        memcpy(dir, path, dlen);
        dir[dlen] = 0;
        mkdir(dir, 0755);
      }
    }
  }
  g_fd[slot] = open(path, O_CREAT | O_RDWR, 0600);
  return g_fd[slot];
}

int ng_llm_sched_try_acquire(void) {
  int s;
  ensure_defaults();
  if (!g_enabled) return 1;
  if (g_held >= 0) return 1;
  for (s = 0; s < g_slots; s++) {
    int fd = open_slot(s);
    if (fd < 0) continue;
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
      g_held = s;
      return 1;
    }
  }
  return 0;
}

void ng_llm_sched_acquire(void) {
  ensure_defaults();
  if (!g_enabled) return;
  if (g_held >= 0) return;
  /* Spin: try free slot, else block on slot 0 then re-scan */
  for (;;) {
    int s;
    if (ng_llm_sched_try_acquire()) return;
    {
      int fd = open_slot(0);
      if (fd >= 0) {
        while (flock(fd, LOCK_EX) != 0) {
          if (errno == EINTR) continue;
          break;
        }
        /* If multi-slot, release 0 and grab any free (may be 0). */
        if (g_slots > 1) {
          flock(fd, LOCK_UN);
          for (s = 0; s < g_slots; s++) {
            int f2 = open_slot(s);
            if (f2 < 0) continue;
            if (flock(f2, LOCK_EX | LOCK_NB) == 0) {
              g_held = s;
              return;
            }
          }
          /* brief yield */
          usleep(20000);
          continue;
        }
        g_held = 0;
        return;
      }
    }
    usleep(50000);
  }
}

void ng_llm_sched_release(void) {
  ensure_defaults();
  if (!g_enabled) return;
  if (g_held < 0) return;
  if (g_fd[g_held] >= 0)
    flock(g_fd[g_held], LOCK_UN);
  g_held = -1;
}

char *ng_llm_sched_run(ng_llm_job_fn fn, void *userdata) {
  if (!fn) return NULL;
  ng_llm_sched_acquire();
  char *r = fn(userdata);
  ng_llm_sched_release();
  return r;
}

char *ng_llm_sched_status_json(void) {
  char *out = NULL;
  ensure_defaults();
  compute_base();
  asprintf(&out,
           "{\"enabled\":%s,\"slots\":%d,\"held\":%d,\"path\":\"%s\"}",
           g_enabled ? "true" : "false", g_slots, g_held, g_base_path);
  return out ? out : strdup("{\"enabled\":false}");
}
