/* BrainCube plugin — peer API + durable state + optional plane auto-adapt.
 * Algorithm: third_party/braincube (LHLAM). Research proprietary. */
#include "braincube_plugin.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <strings.h>

#if NANOBOT_HAS_BRAINCUBE
#include <braincube/braincube.h>
#endif

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_inited;
static int g_enabled = 1;
static int g_auto_adapt;
static int g_continuous = 1; /* default ON: parent continuous learn on robot */
static int g_self_teach = 1; /* sensor-derived teacher when no agent supervise */
static int g_direct_io; /* sample product plane paths directly */
static int g_dry_run = 1; /* never write plane unless explicit */
static uint32_t g_decides, g_teaches_ok, g_teaches_bad, g_ticks, g_plane_samples;
static uint32_t g_self_teaches, g_agent_teaches;
static int g_last_decision = -1;
static int g_last_teach_want = -1;
static char g_last_stats[256];
static char g_last_teach_src[24]; /* "self" | "agent" | "none" */
static pthread_t g_adapt_thr;
static int g_adapt_run;
/* Agent occasional supervision (also mirrored to supervise.cmd for fork writers). */
static int g_agent_want = -1;
static time_t g_agent_until;
static char g_agent_note[96];
/* Training session + always-on agent service flag (file-backed for fork + shell agent). */
static char g_session_id[40];
static time_t g_session_start;
static int g_agent_service; /* 1 = external train_agent_loop should stay active */
static uint32_t g_resets;

#if NANOBOT_HAS_BRAINCUBE
static lhlam_cube g_cube;
static int g_cube_live;
/* Brain-like CubeChain: ONE LHTL cube per sensor I/O + Meta Cube mapper.
 * Each sensor cube: IN=sensor digits, OUT=fire/interpret (one I/O per sensor).
 * Meta cube: maps all sensor OUTs → which sensor stream is salient now. */
#define NG_BC_SENSORS 8
#define NG_BC_LANES   NG_BC_SENSORS
#define CHAIN_MAGIC   0x43484e32u /* CHN2 — raw cube dump + counters */
static lhlam_cube *g_lane; /* heap: per-sensor LHTL cubes (parent continuous) */
static lhlam_cube *g_coord; /* heap Meta Cube */
static int g_chain_live;
static int g_chain_pick = -1;
static uint32_t g_chain_ticks, g_chain_agree, g_chain_conflict;
static int g_lane_fire[NG_BC_SENSORS];
static int g_sensor_val[NG_BC_SENSORS];  /* last raw sensor digits 0..9 */
static uint32_t g_lane_ticks[NG_BC_SENSORS];
static float g_activity[NG_BC_SENSORS];  /* 0..1 smoothed activity for viz */
static float g_meta_activity;
static const char *g_lane_name[NG_BC_SENSORS] = {
  "bump_L", "bump_C", "bump_R", "charge",
  "battery", "state", "error", "free_ok"
};
/* last world snapshot for live viz */
static int g_world_state = 3, g_world_charge = 0, g_world_battery = 0, g_world_error = 0;
static int g_world_bump[3];
static uint64_t g_live_seq;
#endif

static void state_path(char *buf, size_t n) {
  snprintf(buf, n, "%s/braincube/state.bin", ng_workdir());
}

static void ensure_dir(void) {
  char p[640];
  snprintf(p, sizeof p, "%s/braincube", ng_workdir());
  mkdir(p, 0700);
}

static void persist_locked(void) {
#if NANOBOT_HAS_BRAINCUBE
  if (!g_cube_live) return;
  ensure_dir();
  uint8_t blob[sizeof(lhlam_cube) + 64];
  size_t n = lhlam_cube_export(&g_cube, blob, sizeof blob);
  if (n == 0) return;
  char path[700];
  state_path(path, sizeof path);
  ng_write_file(path, (const char *)blob, n);
  lhlam_cube_stats(&g_cube, g_last_stats, sizeof g_last_stats);
#else
  (void)0;
#endif
}

static void load_locked(void) {
#if NANOBOT_HAS_BRAINCUBE
  char path[700];
  state_path(path, sizeof path);
  size_t blen = 0;
  char *b = ng_read_file(path, &blen);
  if (b && blen > 0 && lhlam_cube_import(&g_cube, (const uint8_t *)b, blen) == 0) {
    g_cube_live = 1;
    lhlam_cube_stats(&g_cube, g_last_stats, sizeof g_last_stats);
  } else {
    uint8_t seed[32];
    memset(seed, 0, sizeof seed);
    /* mix workdir path into seed for per-device cube */
    const char *w = ng_workdir();
    for (size_t i = 0; w && w[i] && i < 32; i++) seed[i] ^= (uint8_t)w[i];
    seed[0] ^= (uint8_t)time(NULL);
    lhlam_cube_init(&g_cube, seed);
    g_cube_live = 1;
    lhlam_cube_stats(&g_cube, g_last_stats, sizeof g_last_stats);
  }
  free(b);
#endif
}

#if NANOBOT_HAS_BRAINCUBE
/* File cache shared across fork workers (in-process static dies with worker). */
#define RC_CACHE_FRESH_S  3
#define RC_CACHE_STALE_S  30

static void rc_cache_path(char *buf, size_t n) {
  snprintf(buf, n, "%s/braincube/rc_sample.bin", ng_workdir());
}

/* Binary: magic u32, mono_sec u32, 8 digits, world ints (7). */
static int rc_cache_load(uint8_t *out, size_t cap, int max_age_s) {
  char path[700];
  uint8_t blob[64];
  int fd;
  ssize_t n;
  uint32_t magic, mono;
  struct timespec now;
  if (cap < 8) return 0;
  rc_cache_path(path, sizeof path);
  fd = open(path, O_RDONLY);
  if (fd < 0) return 0;
  n = read(fd, blob, sizeof blob);
  close(fd);
  if (n < 8 + 8 + 4 + 4) return 0;
  memcpy(&magic, blob, 4);
  memcpy(&mono, blob + 4, 4);
  if (magic != 0x52435331u) return 0; /* 'RCS1' */
  clock_gettime(CLOCK_MONOTONIC, &now);
  if ((uint32_t)now.tv_sec < mono) return 0;
  if ((uint32_t)now.tv_sec - mono > (uint32_t)max_age_s) return 0;
  memcpy(out, blob + 8, 8);
  if (n >= 8 + 8 + 28) {
    int *w = (int *)(blob + 16);
    g_world_state = w[0];
    g_world_charge = w[1];
    g_world_battery = w[2];
    g_world_error = w[3];
    g_world_bump[0] = w[4];
    g_world_bump[1] = w[5];
    g_world_bump[2] = w[6];
  }
  for (int i = 0; i < 8; i++) g_sensor_val[i] = out[i];
  return 8;
}

static void rc_cache_save(const uint8_t *digits8) {
  char path[700], dir[700];
  uint8_t blob[64];
  struct timespec now;
  int fd, w[7];
  uint32_t magic = 0x52435331u, mono;
  ensure_dir();
  rc_cache_path(path, sizeof path);
  clock_gettime(CLOCK_MONOTONIC, &now);
  mono = (uint32_t)now.tv_sec;
  memset(blob, 0, sizeof blob);
  memcpy(blob, &magic, 4);
  memcpy(blob + 4, &mono, 4);
  memcpy(blob + 8, digits8, 8);
  w[0] = g_world_state; w[1] = g_world_charge; w[2] = g_world_battery;
  w[3] = g_world_error; w[4] = g_world_bump[0]; w[5] = g_world_bump[1];
  w[6] = g_world_bump[2];
  memcpy(blob + 16, w, sizeof w);
  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return;
  (void)write(fd, blob, 16 + (int)sizeof w);
  close(fd);
  (void)dir;
}

/* Direct TCP GET — no popen/curl (fork+exec was ~1s and starved rockctl). */
static int rockctl_http_get(char *buf, size_t cap) {
  const char *host = "127.0.0.1";
  int port = 8080;
  const char *env = getenv("ROCKCTL_URL");
  int fd = -1, n = 0, total = 0;
  struct sockaddr_in sa;
  struct timeval tv;
  char req[128];
  if (env && env[0]) {
    /* only support http://host:port form; default path /api/v1/status */
    if (strncmp(env, "http://", 7) == 0) {
      char tmp[128];
      char *colon, *slash;
      snprintf(tmp, sizeof tmp, "%s", env + 7);
      slash = strchr(tmp, '/');
      if (slash) *slash = 0;
      colon = strchr(tmp, ':');
      if (colon) {
        *colon = 0;
        host = tmp;
        port = atoi(colon + 1);
        if (port <= 0) port = 8080;
      } else {
        host = tmp;
      }
    }
  }
  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  tv.tv_sec = 0;
  tv.tv_usec = 200000; /* 200ms — rockctl often keeps conn open after body */
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  memset(&sa, 0, sizeof sa);
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
    close(fd);
    return -1;
  }
  if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
    close(fd);
    return -1;
  }
  snprintf(req, sizeof req,
           "GET /api/v1/status HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
           host);
  if (write(fd, req, strlen(req)) < 0) {
    close(fd);
    return -1;
  }
  while (total + 1 < (int)cap) {
    n = (int)read(fd, buf + total, cap - 1 - (size_t)total);
    if (n <= 0) break;
    total += n;
    buf[total] = 0;
    /* stop as soon as body looks like rockctl JSON (avoid hanging on keep-alive) */
    if (strstr(buf, "\r\n\r\n") &&
        (strstr(buf, "\"battery\"") || strstr(buf, "\"state\"")) &&
        strchr(buf, '}'))
      break;
  }
  close(fd);
  buf[total > 0 ? total : 0] = 0;
  /* strip headers */
  {
    char *body = strstr(buf, "\r\n\r\n");
    if (body) {
      body += 4;
      memmove(buf, body, strlen(body) + 1);
      total = (int)strlen(buf);
    }
  }
  return total;
}

/* Sample Clanker rockctl status into per-sensor digits (on-robot: 127.0.0.1:8080). */
static size_t sample_clanker_sensors(uint8_t *out, size_t cap) {
  char buf[4096];
  int n = 0;
  int bl = 0, bc = 0, br = 0, st = 3, ch = 0, bat = 50, err = 0;
  if (cap < 8) return 0;
  /* fork-safe fresh cache — avoid hammering single-threaded rockctl */
  if (rc_cache_load(out, cap, RC_CACHE_FRESH_S) == 8) return 8;
  /* simple file lock so only one worker refreshes at a time */
  {
    char lpath[700];
    int lfd;
    snprintf(lpath, sizeof lpath, "%s/braincube/rc.lock", ng_workdir());
    ensure_dir();
    lfd = open(lpath, O_WRONLY | O_CREAT, 0600);
    if (lfd >= 0) {
      struct flock fl;
      memset(&fl, 0, sizeof fl);
      fl.l_type = F_WRLCK;
      fl.l_whence = SEEK_SET;
      /* non-blocking: if locked, use stale immediately */
      if (fcntl(lfd, F_SETLK, &fl) != 0) {
        close(lfd);
        if (rc_cache_load(out, cap, RC_CACHE_STALE_S) == 8) return 8;
        /* last resort: still try HTTP */
      } else {
        /* re-check fresh under lock */
        if (rc_cache_load(out, cap, RC_CACHE_FRESH_S) == 8) {
          fl.l_type = F_UNLCK;
          fcntl(lfd, F_SETLK, &fl);
          close(lfd);
          return 8;
        }
        n = rockctl_http_get(buf, sizeof buf);
        fl.l_type = F_UNLCK;
        fcntl(lfd, F_SETLK, &fl);
        close(lfd);
        goto have_n;
      }
    }
  }
  n = rockctl_http_get(buf, sizeof buf);
have_n:
  if (n < 8) {
    /* accept stale cache rather than empty features */
    if (rc_cache_load(out, cap, RC_CACHE_STALE_S) == 8) return 8;
    return 0;
  }
  /* crude parse */
  {
    const char *p = strstr(buf, "\"state\"");
    if (p) { p = strchr(p, ':'); if (p) st = atoi(p + 1); }
    p = strstr(buf, "\"charge_status\"");
    if (p) { p = strchr(p, ':'); if (p) ch = atoi(p + 1); }
    p = strstr(buf, "\"battery\"");
    if (p) { p = strchr(p, ':'); if (p) bat = atoi(p + 1); }
    p = strstr(buf, "\"error_code\"");
    if (p) { p = strchr(p, ':'); if (p) err = atoi(p + 1); }
    p = strstr(buf, "adbumper_status");
    if (p) {
      while (*p && *p != '[') p++;
      if (*p == '[') {
        int vals[3] = {0, 0, 0}, vi = 0;
        p++;
        while (*p && *p != ']' && vi < 3) {
          while (*p == ' ' || *p == ',') p++;
          if (*p >= '0' && *p <= '9') {
            int v = 0;
            while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
            vals[vi++] = v;
          } else if (*p) p++;
        }
        bl = vals[0]; bc = vals[1]; br = vals[2];
      }
    }
  }
  g_world_state = st;
  g_world_charge = ch;
  g_world_battery = bat;
  g_world_error = err;
  g_world_bump[0] = bl; g_world_bump[1] = bc; g_world_bump[2] = br;
  out[0] = (uint8_t)(bl ? 9 : 0);                 /* bump_L */
  out[1] = (uint8_t)(bc ? 9 : 0);                 /* bump_C */
  out[2] = (uint8_t)(br ? 9 : 0);                 /* bump_R */
  out[3] = (uint8_t)(ch ? 9 : 0);                 /* charge dock */
  out[4] = (uint8_t)((bat / 11) % 10);            /* battery band */
  out[5] = (uint8_t)(st % 10);                    /* state digit */
  out[6] = (uint8_t)(err ? 9 : 0);                /* error */
  out[7] = (uint8_t)((!bl && !bc && !br && !err && !ch) ? 9 : 0); /* free_ok */
  for (int i = 0; i < 8; i++) g_sensor_val[i] = out[i];
  rc_cache_save(out);
  return 8;
}

static void chain_state_path(char *buf, size_t n) {
  snprintf(buf, n, "%s/braincube/chain_state.bin", ng_workdir());
}
static void live_snap_path(char *buf, size_t n) {
  snprintf(buf, n, "%s/braincube/live_snap.json", ng_workdir());
}
static void supervise_path(char *buf, size_t n) {
  snprintf(buf, n, "%s/braincube/supervise.cmd", ng_workdir());
}

/* Map name or digit string → lane index; -1 if unknown. */
static int lane_from_token(const char *s) {
  int i;
  if (!s || !s[0]) return -1;
  if (isdigit((unsigned char)s[0]) && !s[1]) return s[0] - '0';
  if (isdigit((unsigned char)s[0])) {
    int v = atoi(s);
    if (v >= 0 && v < NG_BC_SENSORS) return v;
  }
  for (i = 0; i < NG_BC_SENSORS; i++)
    if (!strcasecmp(s, g_lane_name[i])) return i;
  if (!strcasecmp(s, "meta")) return -2;
  return -1;
}

/* Durable raw dump (includes assoc tables — export() omits them). */
static void chain_persist_locked(void) {
  char path[700], tmp[720];
  int fd;
  uint32_t magic = CHAIN_MAGIC, csz, nsens;
  if (!g_chain_live || !g_lane || !g_coord) return;
  ensure_dir();
  chain_state_path(path, sizeof path);
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return;
  csz = (uint32_t)sizeof(lhlam_cube);
  nsens = (uint32_t)NG_BC_SENSORS;
  (void)write(fd, &magic, 4);
  (void)write(fd, &csz, 4);
  (void)write(fd, &nsens, 4);
  (void)write(fd, &g_chain_ticks, 4);
  (void)write(fd, &g_chain_agree, 4);
  (void)write(fd, &g_chain_conflict, 4);
  (void)write(fd, &g_teaches_ok, 4);
  (void)write(fd, &g_teaches_bad, 4);
  (void)write(fd, &g_self_teaches, 4);
  (void)write(fd, &g_agent_teaches, 4);
  (void)write(fd, g_lane, (size_t)NG_BC_SENSORS * sizeof(lhlam_cube));
  (void)write(fd, g_coord, sizeof(lhlam_cube));
  (void)write(fd, g_lane_ticks, sizeof g_lane_ticks);
  close(fd);
  rename(tmp, path);
}

static int chain_load_locked(void) {
  char path[700];
  int fd;
  uint32_t magic = 0, csz = 0, nsens = 0;
  ssize_t n;
  if (!g_lane) {
    g_lane = (lhlam_cube *)calloc((size_t)NG_BC_SENSORS, sizeof(lhlam_cube));
    g_coord = (lhlam_cube *)calloc(1, sizeof(lhlam_cube));
    if (!g_lane || !g_coord) return 0;
  }
  chain_state_path(path, sizeof path);
  fd = open(path, O_RDONLY);
  if (fd < 0) return 0;
  if (read(fd, &magic, 4) != 4 || magic != CHAIN_MAGIC) { close(fd); return 0; }
  if (read(fd, &csz, 4) != 4 || csz != (uint32_t)sizeof(lhlam_cube)) { close(fd); return 0; }
  if (read(fd, &nsens, 4) != 4 || nsens != (uint32_t)NG_BC_SENSORS) { close(fd); return 0; }
  (void)read(fd, &g_chain_ticks, 4);
  (void)read(fd, &g_chain_agree, 4);
  (void)read(fd, &g_chain_conflict, 4);
  (void)read(fd, &g_teaches_ok, 4);
  (void)read(fd, &g_teaches_bad, 4);
  (void)read(fd, &g_self_teaches, 4);
  (void)read(fd, &g_agent_teaches, 4);
  n = read(fd, g_lane, (size_t)NG_BC_SENSORS * sizeof(lhlam_cube));
  if (n != (ssize_t)((size_t)NG_BC_SENSORS * sizeof(lhlam_cube))) { close(fd); return 0; }
  n = read(fd, g_coord, sizeof(lhlam_cube));
  if (n != (ssize_t)sizeof(lhlam_cube)) { close(fd); return 0; }
  (void)read(fd, g_lane_ticks, sizeof g_lane_ticks);
  close(fd);
  g_chain_live = 1;
  return 1;
}

static void chain_init_locked(void) {
  int i;
  uint8_t seed[32];
  if (g_chain_live) return;
  if (chain_load_locked()) return;
  if (!g_lane) {
    g_lane = (lhlam_cube *)calloc((size_t)NG_BC_SENSORS, sizeof(lhlam_cube));
    g_coord = (lhlam_cube *)calloc(1, sizeof(lhlam_cube));
    if (!g_lane || !g_coord) return;
  }
  for (i = 0; i < NG_BC_SENSORS; i++) {
    memset(seed, 0x51 + i, sizeof seed);
    seed[0] ^= (uint8_t)i;
    seed[1] ^= (uint8_t)time(NULL);
    lhlam_cube_init(&g_lane[i], seed);
  }
  memset(seed, 0xC0, sizeof seed);
  seed[0] = (uint8_t)NG_BC_SENSORS;
  lhlam_cube_init(g_coord, seed);
  g_chain_live = 1;
  g_chain_pick = -1;
  g_chain_ticks = g_chain_agree = g_chain_conflict = 0;
}

/* Sensor-derived teacher: which lane should be salient given world. */
static int self_teacher_want(const uint8_t *feat, size_t nf) {
  if (!feat || nf < 8) return -1;
  if (feat[0] >= 5) return 0; /* bump_L */
  if (feat[1] >= 5) return 1; /* bump_C */
  if (feat[2] >= 5) return 2; /* bump_R */
  if (feat[6] >= 5) return 6; /* error */
  if (feat[3] >= 5) return 3; /* charge / dock */
  if (feat[4] <= 2) return 4; /* low battery band */
  if (feat[7] >= 5) return 7; /* free_ok clear path */
  return 5; /* state stream as default attention */
}

/* Pull agent supervise from file (fork-safe) + expire TTL. */
static void supervise_poll_locked(void) {
  char path[700], line[192];
  FILE *f;
  time_t now = time(NULL);
  int want = -1, ttl = 0;
  char note[96];
  note[0] = 0;
  if (g_agent_want >= 0 && g_agent_until > 0 && now > g_agent_until) {
    g_agent_want = -1;
    g_agent_until = 0;
    g_agent_note[0] = 0;
  }
  supervise_path(path, sizeof path);
  f = fopen(path, "r");
  if (!f) return;
  while (fgets(line, sizeof line, f)) {
    size_t L = strlen(line);
    while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = 0;
    if (!strncmp(line, "want=", 5)) want = lane_from_token(line + 5);
    else if (!strncmp(line, "ttl=", 4)) ttl = atoi(line + 4);
    else if (!strncmp(line, "until=", 6)) g_agent_until = (time_t)atol(line + 6);
    else if (!strncmp(line, "note=", 5))
      snprintf(note, sizeof note, "%s", line + 5);
  }
  fclose(f);
  if (want >= 0 && want < NG_BC_SENSORS) {
    g_agent_want = want;
    if (ttl > 0) g_agent_until = now + ttl;
    if (note[0]) snprintf(g_agent_note, sizeof g_agent_note, "%s", note);
    unlink(path); /* consume once into parent memory */
  }
}

/* One-liner report after supervision, then purge bulky logs (robot stays lean). */
void ng_bc_log_supervision_oneliner(const char *line) {
  char path[700], onepath[700];
  char buf[320];
  int n;
  time_t now = time(NULL);
  ensure_dir();
  n = snprintf(buf, sizeof buf, "%ld %s\n", (long)now, line && line[0] ? line : "supervise");
  if (n < 0) return;
  if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;
  /* single current line only */
  snprintf(onepath, sizeof onepath, "%s/braincube/last_report.txt", ng_workdir());
  ng_write_file(onepath, buf, (size_t)n);
  /* purge bulky logs — keep empty or tiny */
  snprintf(path, sizeof path, "%s/braincube/agent_log.jsonl", ng_workdir());
  ng_write_file(path, "", 0);
  snprintf(path, sizeof path, "%s/braincube/train_agent.out", ng_workdir());
  ng_write_file(path, "", 0);
  /* keep trials.jsonl capped: wipe if huge (field loop rewrites latest separately) */
  {
    char tpath[700];
    struct stat st;
    snprintf(tpath, sizeof tpath, "%s/braincube/trials.jsonl", ng_workdir());
    if (stat(tpath, &st) == 0 && st.st_size > 8000)
      ng_write_file(tpath, "", 0);
  }
}

static void supervise_write(int want, int ttl_sec, const char *note) {
  char path[700], body[256], one[200];
  int n;
  ensure_dir();
  supervise_path(path, sizeof path);
  n = snprintf(body, sizeof body, "want=%d\nttl=%d\nuntil=%ld\nnote=%s\n",
               want, ttl_sec > 0 ? ttl_sec : 30,
               (long)(time(NULL) + (ttl_sec > 0 ? ttl_sec : 30)),
               note && note[0] ? note : "agent");
  ng_write_file(path, body, (size_t)n);
  /* also set in-process if parent */
  pthread_mutex_lock(&g_mu);
  g_agent_want = want;
  g_agent_until = time(NULL) + (ttl_sec > 0 ? ttl_sec : 30);
  snprintf(g_agent_note, sizeof g_agent_note, "%s", note && note[0] ? note : "agent");
  pthread_mutex_unlock(&g_mu);
  /* one-liner then purge */
  snprintf(one, sizeof one, "supervise want=%d(%s) ttl=%d note=%s",
           want,
           (want >= 0 && want < NG_BC_SENSORS) ? g_lane_name[want] : "?",
           ttl_sec > 0 ? ttl_sec : 30,
           note && note[0] ? note : "agent");
  ng_bc_log_supervision_oneliner(one);
}

/* Human-readable train status for nanobot agent / MCP tool. */
char *ng_bc_train_status_report(void) {
  char *snap = NULL, *lat = NULL, *fst = NULL, *one = NULL, *jb = NULL;
  size_t sl = 0, ll = 0, fl = 0, ol = 0;
  char path[700];
  int cont = g_continuous, self = g_self_teach, asvc = g_agent_service;
  int field_on = 0, explore_on = 0;
  ensure_dir();
  snprintf(path, sizeof path, "%s/braincube/live_snap.json", ng_workdir());
  snap = ng_read_file(path, &sl);
  snprintf(path, sizeof path, "%s/braincube/latest_trial.json", ng_workdir());
  lat = ng_read_file(path, &ll);
  snprintf(path, sizeof path, "%s/braincube/field_status.json", ng_workdir());
  fst = ng_read_file(path, &fl);
  snprintf(path, sizeof path, "%s/braincube/last_report.txt", ng_workdir());
  one = ng_read_file(path, &ol);
  snprintf(path, sizeof path, "%s/braincube/field_trials.flag", ng_workdir());
  field_on = (access(path, F_OK) == 0);
  snprintf(path, sizeof path, "%s/braincube/explore.flag", ng_workdir());
  explore_on = (access(path, F_OK) == 0);

  /* extract a few keys from snap with crude parse */
  {
    int seq = 0, self_t = 0, agent_t = 0, agree = 0;
    char pick[32] = "?";
    const char *p;
    if (snap) {
      p = strstr(snap, "\"seq\":"); if (p) seq = atoi(p + 6);
      p = strstr(snap, "\"self_teaches\":"); if (p) self_t = atoi(p + 15);
      p = strstr(snap, "\"agent_teaches\":"); if (p) agent_t = atoi(p + 16);
      p = strstr(snap, "\"agree\":"); if (p) agree = atoi(p + 8);
      p = strstr(snap, "\"pick_name\":\"");
      if (p) {
        p += 13;
        size_t i = 0;
        while (*p && *p != '"' && i + 1 < sizeof pick) pick[i++] = *p++;
        pick[i] = 0;
      }
    }
    asprintf(&jb,
      "BrainCube train status (plugin %s)\n"
      "continuous=%s self_teach=%s agent_service=%s field_trials=%s explore=%s\n"
      "seq=%d self_teaches=%d agent_teaches=%d meta_agree=%d pick=%s session=%s\n"
      "latest_trial=%s\n"
      "field_status=%s\n"
      "last_report=%s\n"
      "hint: without field_trials/explore the robot only trains attention (no RC motion).",
      NG_BC_PLUGIN_VERSION,
      cont ? "on" : "off", self ? "on" : "off", asvc ? "on" : "off",
      field_on ? "on" : "off", explore_on ? "on" : "off",
      seq, self_t, agent_t, agree, pick,
      g_session_id[0] ? g_session_id : "(none)",
      (lat && ll > 2) ? lat : "(none yet)",
      (fst && fl > 2) ? fst : "(n/a)",
      (one && ol > 0) ? one : "(none)");
  }
  free(snap); free(lat); free(fst); free(one);
  return jb ? jb : strdup("train status unavailable");
}

static char *chain_live_json_locked(void); /* defined below */

static void write_live_snap_locked(void) {
  char path[700], tmp[720];
  char *jb;
  if (!g_chain_live) return;
  jb = chain_live_json_locked();
  if (!jb) return;
  /* enrich snap with continuous learn fields (append before final }) */
  {
    char *enriched = NULL;
    size_t L = strlen(jb);
    if (L > 2 && jb[L - 1] == '}') {
      jb[L - 1] = 0;
      asprintf(&enriched,
        "%s,\"continuous\":%s,\"self_teach\":%s,\"learning\":true,"
        "\"teaches_ok\":%u,\"teaches_bad\":%u,\"self_teaches\":%u,\"agent_teaches\":%u,"
        "\"last_teach_want\":%d,\"last_teach_src\":\"%s\","
        "\"agent_want\":%d,\"agent_note\":\"%s\","
        "\"agent_service\":%s,\"session_id\":\"%s\",\"session_age_s\":%ld,\"resets\":%u}",
        jb,
        g_continuous ? "true" : "false",
        g_self_teach ? "true" : "false",
        g_teaches_ok, g_teaches_bad, g_self_teaches, g_agent_teaches,
        g_last_teach_want, g_last_teach_src[0] ? g_last_teach_src : "none",
        g_agent_want,
        g_agent_note[0] ? g_agent_note : "",
        g_agent_service ? "true" : "false",
        g_session_id[0] ? g_session_id : "",
        g_session_start ? (long)(time(NULL) - g_session_start) : 0L,
        g_resets);
      free(jb);
      jb = enriched;
    }
  }
  if (!jb) return;
  ensure_dir();
  live_snap_path(path, sizeof path);
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  ng_write_file(tmp, jb, strlen(jb));
  rename(tmp, path);
  free(jb);
}

/* Fork workers: serve parent snapshot if fresh (continuous learn lives in parent). */
static char *read_live_snap_if_fresh(int max_age_s) {
  char path[700];
  struct stat st;
  size_t blen = 0;
  char *b;
  live_snap_path(path, sizeof path);
  if (stat(path, &st) != 0) return NULL;
  if (time(NULL) - st.st_mtime > max_age_s) return NULL;
  b = ng_read_file(path, &blen);
  if (!b || blen < 8) { free(b); return NULL; }
  return b;
}

/* One chain step: plane digits → each lane fire → coord picks lane.
 * ARMv7 lean: short feature bus, one meta tick, decide per lane (no 8 meta ticks). */
static int chain_tick_locked(uint8_t *feat, size_t nf, int teach_want /* -1 none */) {
  int i, pick = 0, best = -1, nfire = 0;
  uint8_t bus[16];
  size_t o = 0, k;
  if (!g_chain_live) chain_init_locked();
  if (nf == 0) {
    feat[0] = (uint8_t)(time(NULL) % 10);
    feat[1] = (uint8_t)((g_chain_ticks + 3) % 10);
    nf = 2;
  }
  if (nf > 8) nf = 8;
  /* Each sensor cube: IN = own value + short context (cap 4 shared digits). */
  for (i = 0; i < NG_BC_SENSORS; i++) {
    uint8_t sin[8];
    size_t sn = 0;
    sin[sn++] = (uint8_t)(i < (int)nf ? feat[i] % 10 : g_sensor_val[i] % 10);
    sin[sn++] = (uint8_t)i; /* sensor id nibble */
    for (k = 0; k < nf && sn < 6; k++) sin[sn++] = (uint8_t)(feat[k] % 10);
    lhlam_cube_tick(&g_lane[i], sin, sn);
    g_lane_fire[i] = lhlam_cube_decide(&g_lane[i], sin, sn) ? 1 : 0;
    g_lane_ticks[i]++;
    if (g_lane_fire[i]) nfire++;
    if (i < (int)nf) g_sensor_val[i] = feat[i] % 10;
  }
  if (nfire > 1) g_chain_conflict++;
  for (k = 0; k < nf && o < 8; k++) bus[o++] = (uint8_t)(feat[k] % 10);
  for (i = 0; i < NG_BC_LANES && o < 16; i++)
    bus[o++] = (uint8_t)(g_lane_fire[i] ? 9 : 0);
  /* One meta tick on fire bus, then cheap decide per lane index. */
  lhlam_cube_tick(g_coord, bus, o);
  for (i = 0; i < NG_BC_LANES; i++) {
    uint8_t in[17];
    int sc;
    memcpy(in, bus, o);
    in[o] = (uint8_t)i;
    sc = lhlam_cube_decide(g_coord, in, o + 1) + (g_lane_fire[i] ? 1 : 0);
    /* Prefer active / high raw sensor when decide ties */
    if (i < (int)nf && feat[i] >= 5) sc++;
    if (sc > best) { best = sc; pick = i; }
  }
  g_chain_pick = pick;
  g_chain_ticks++;
  g_last_decision = pick;
  /* optional teach: want lane index or -1; also train fire bits if teach_want in 0..3 */
  if (teach_want >= 0 && teach_want < NG_BC_LANES) {
    uint8_t in[17];
    int ok = (pick == teach_want);
    memcpy(in, bus, o);
    in[o] = (uint8_t)pick;
    lhlam_cube_feedback(g_coord, in, o + 1, 1, ok ? 1 : 0);
    if (!ok) {
      in[o] = (uint8_t)teach_want;
      lhlam_cube_feedback(g_coord, in, o + 1, 1, 1);
    }
    if (ok) g_chain_agree++;
    for (i = 0; i < NG_BC_LANES; i++) {
      int want_fire = (i == teach_want) ? 1 : 0;
      /* train fire: correct → hit, wrong → miss */
      lhlam_cube_feedback(&g_lane[i], feat, nf, g_lane_fire[i],
                          (g_lane_fire[i] == want_fire) ? 1 : 0);
    }
    if (ok) g_teaches_ok++; else g_teaches_bad++;
    g_last_teach_want = teach_want;
  }
  lhlam_cube_stats(g_coord, g_last_stats, sizeof g_last_stats);
  return pick;
}

static char *chain_status_json_locked(void) {
  char *jb = NULL;
  char cs[160];
  if (!g_chain_live) chain_init_locked();
  lhlam_cube_stats(g_coord, cs, sizeof cs);
  asprintf(&jb,
    "{\"ok\":true,\"plugin\":\"cubechain\",\"available\":true,"
    "\"brain\":\"sensor_cubes+meta\",\"sensors\":%d,\"ticks\":%u,"
    "\"pick\":%d,\"pick_name\":\"%s\","
    "\"fire\":[%d,%d,%d,%d,%d,%d,%d,%d],"
    "\"names\":[\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"],"
    "\"agree\":%u,\"conflict\":%u,"
    "\"meta_is\":\"lhlam_cube\","
    "\"declaration\":\"each sensor = one LHTL cube (IN+OUT); MetaCube maps all\","
    "\"core_owns\":\"efficient IN to OUT I/O per sensor; meta selects salience\","
    "\"direction\":\"sensor cubes interpret; MetaCube brain-like map\","
    "\"stats\":\"%s\"}",
    NG_BC_SENSORS, g_chain_ticks, g_chain_pick,
    (g_chain_pick >= 0 && g_chain_pick < NG_BC_SENSORS) ? g_lane_name[g_chain_pick] : "?",
    g_lane_fire[0], g_lane_fire[1], g_lane_fire[2], g_lane_fire[3],
    g_lane_fire[4], g_lane_fire[5], g_lane_fire[6], g_lane_fire[7],
    g_lane_name[0], g_lane_name[1], g_lane_name[2], g_lane_name[3],
    g_lane_name[4], g_lane_name[5], g_lane_name[6], g_lane_name[7],
    g_chain_agree, g_chain_conflict, cs);
  return jb ? jb : strdup("{\"ok\":false}");
}

/* Compact structure for lightweight 3D viz: neur/assoc counts + 4³ cell sample. */
static void cube_struct_append(char *buf, size_t cap, size_t *o, const lhlam_cube *c,
                               const char *id, const char *role, int first) {
  unsigned neur = 0, assoc = 0, i, nn, n;
  int vx[64];
  int vi = 0;
  if (!c || *o + 200 >= cap) return;
  n = c->n ? c->n : 8;
  nn = (unsigned)n * n * n;
  for (i = 0; i < nn && i < (unsigned)LHLAM_MAX_CELLS; i++)
    if (c->neuron[i]) neur++;
  for (i = 0; i < (unsigned)LHLAM_ASSOC_N; i++)
    if (c->assoc_act[i] >= 0) assoc++;
  /* 4x4x4 downsample of cells (digits 0-9) — enough for isometric map */
  {
    unsigned x, y, z;
    for (z = 0; z < 4; z++)
      for (y = 0; y < 4; y++)
        for (x = 0; x < 4; x++) {
          unsigned sx = (x * n) / 4, sy = (y * n) / 4, sz = (z * n) / 4;
          unsigned idx = sz * n * n + sy * n + sx;
          if (idx >= nn) idx = nn - 1;
          vx[vi++] = (int)(c->cells[idx] % 10);
        }
  }
  *o += (size_t)snprintf(buf + *o, cap - *o,
    "%s{\"id\":\"%s\",\"role\":\"%s\",\"n\":%u,\"gen\":%u,"
    "\"neur\":%u,\"assoc\":%u,\"cells\":%u,"
    "\"vox\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
    "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
    "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
    "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d]}",
    first ? "" : ",", id, role, (unsigned)c->n, (unsigned)c->gen,
    neur, assoc, nn,
    vx[0],vx[1],vx[2],vx[3],vx[4],vx[5],vx[6],vx[7],
    vx[8],vx[9],vx[10],vx[11],vx[12],vx[13],vx[14],vx[15],
    vx[16],vx[17],vx[18],vx[19],vx[20],vx[21],vx[22],vx[23],
    vx[24],vx[25],vx[26],vx[27],vx[28],vx[29],vx[30],vx[31],
    vx[32],vx[33],vx[34],vx[35],vx[36],vx[37],vx[38],vx[39],
    vx[40],vx[41],vx[42],vx[43],vx[44],vx[45],vx[46],vx[47],
    vx[48],vx[49],vx[50],vx[51],vx[52],vx[53],vx[54],vx[55],
    vx[56],vx[57],vx[58],vx[59],vx[60],vx[61],vx[62],vx[63]);
}

/* Real-time brain snapshot for Nanobot wrapper visualization. */
static char *chain_live_json_locked(void) {
  char *jb = NULL;
  char lanes[4096];
  char structb[8192];
  size_t o = 0, so = 0;
  int i;
  char mstats[160];
  uint8_t dig[8];
  if (!g_chain_live) chain_init_locked();
  /* fork workers: pull last rockctl snapshot into world/sensors for viz */
  if (rc_cache_load(dig, sizeof dig, RC_CACHE_STALE_S) == 8) {
    for (i = 0; i < 8; i++) g_sensor_val[i] = dig[i];
  }
  lhlam_cube_stats(g_coord, mstats, sizeof mstats);
  o += (size_t)snprintf(lanes + o, sizeof lanes - o, "[");
  so += (size_t)snprintf(structb + so, sizeof structb - so, "[");
  cube_struct_append(structb, sizeof structb, &so, g_coord, "meta", "meta", 1);
  for (i = 0; i < NG_BC_SENSORS; i++) {
    float acc = 0;
    uint32_t h = g_lane[i].hits, m = g_lane[i].misses;
    float act;
    if (h + m > 0) acc = (float)h / (float)(h + m);
    act = g_activity[i];
    if (g_lane_fire[i]) act = act * 0.6f + 0.4f;
    else act *= 0.85f;
    g_activity[i] = act;
    o += (size_t)snprintf(lanes + o, sizeof lanes - o,
      "%s{\"id\":\"%s\",\"role\":\"sensor\",\"name\":\"%s\","
      "\"value\":%d,\"fire\":%d,\"activity\":%.3f,"
      "\"hits\":%u,\"misses\":%u,\"acc\":%.3f,\"gen\":%u,\"n\":%u,"
      "\"ticks\":%u}",
      i ? "," : "", g_lane_name[i], g_lane_name[i],
      g_sensor_val[i], g_lane_fire[i], (double)act,
      (unsigned)h, (unsigned)m, (double)acc,
      (unsigned)g_lane[i].gen, (unsigned)g_lane[i].n,
      (unsigned)g_lane_ticks[i]);
    cube_struct_append(structb, sizeof structb, &so, &g_lane[i], g_lane_name[i], "sensor", 0);
    if (o + 180 >= sizeof lanes) break;
  }
  o += (size_t)snprintf(lanes + o, sizeof lanes - o, "]");
  so += (size_t)snprintf(structb + so, sizeof structb - so, "]");
  {
    float macc = 0;
    uint32_t mh = g_coord->hits, mm = g_coord->misses;
    if (mh + mm > 0) macc = (float)mh / (float)(mh + mm);
    g_meta_activity = g_meta_activity * 0.7f + 0.3f * (g_chain_pick >= 0 ? 1.f : 0.2f);
    g_live_seq++;
    (void)mstats;
    asprintf(&jb,
      "{\"ok\":true,\"live\":true,\"seq\":%u,\"hz\":4,"
      "\"declaration\":\"each sensor = one LHTL cube (IN+OUT); Meta maps all\","
      "\"brain\":\"sensor_cubes+meta\","
      "\"meta\":{\"id\":\"meta\",\"role\":\"meta\",\"name\":\"MetaCube\","
      "\"pick\":%d,\"pick_name\":\"%s\",\"activity\":%.3f,"
      "\"hits\":%u,\"misses\":%u,\"acc\":%.3f,\"gen\":%u,\"n\":%u,"
      "\"agree\":%u,\"conflict\":%u},"
      "\"sensors\":%s,"
      "\"structure\":{\"mode\":\"iso4\",\"cubes\":%s},"
      "\"world\":{\"state\":%d,\"charge\":%d,\"battery\":%d,\"error\":%d,"
      "\"bump\":[%d,%d,%d]},"
      "\"viz\":{\"layout\":\"radial+iso3d\",\"meta_center\":true,\"colors\":\"activity\"}}",
      (unsigned)g_live_seq,
      g_chain_pick,
      (g_chain_pick >= 0 && g_chain_pick < NG_BC_SENSORS) ? g_lane_name[g_chain_pick] : "none",
      (double)g_meta_activity,
      (unsigned)mh, (unsigned)mm, (double)macc,
      (unsigned)g_coord->gen, (unsigned)g_coord->n,
      (unsigned)g_chain_agree, (unsigned)g_chain_conflict,
      lanes, structb,
      g_world_state, g_world_charge, g_world_battery, g_world_error,
      g_world_bump[0], g_world_bump[1], g_world_bump[2]);
  }
  return jb ? jb : strdup("{\"ok\":false,\"live\":false}");
}
#endif /* NANOBOT_HAS_BRAINCUBE */

void ng_bc_init(void) {
  pthread_mutex_lock(&g_mu);
  if (!g_inited) {
    g_inited = 1;
    /* Cubes load only in parent continuous thread (or first locked tick).
     * Fork workers prefer live_snap.json — avoid large init in short-lived children. */
    char *e = ng_settings_get("braincube_enabled");
    if (e) { g_enabled = (e[0] != '0'); free(e); }
    e = ng_settings_get("braincube_auto_adapt");
    if (e) { g_auto_adapt = (e[0] == '1'); free(e); }
    e = ng_settings_get("braincube_continuous");
    if (e) { g_continuous = (e[0] != '0'); free(e); }
    e = ng_settings_get("braincube_self_teach");
    if (e) { g_self_teach = (e[0] != '0'); free(e); }
    e = ng_settings_get("braincube_direct_io");
    if (e) { g_direct_io = (e[0] == '1'); free(e); }
    e = ng_settings_get("braincube_dry_run");
    if (e) { g_dry_run = (e[0] != '0'); free(e); }
    e = ng_settings_get("braincube_agent_service");
    if (e) { g_agent_service = (e[0] == '1'); free(e); }
    snprintf(g_last_teach_src, sizeof g_last_teach_src, "none");
  }
  pthread_mutex_unlock(&g_mu);
}

int ng_bc_available(void) {
#if NANOBOT_HAS_BRAINCUBE
  return 1;
#else
  return 0;
#endif
}

int ng_bc_auto_adapt(void) { return g_auto_adapt; }
int ng_bc_direct_io(void) { return g_direct_io; }

/* Hash path contents into digit features for the cube. */
static size_t sample_plane_digits(uint8_t *out, size_t cap) {
  static const char *roots[] = {
    "/data/misc/titan2",
    "/data/local/tmp",
    NULL
  };
  size_t n = 0;
  memset(out, 0, cap);
  if (!g_direct_io) {
    /* abstract host: env only */
    const char *feat = getenv("NANOBOT_BC_FEATURES");
    if (feat) {
      for (size_t i = 0; feat[i] && n < cap; i++) {
        if (isdigit((unsigned char)feat[i])) out[n++] = (uint8_t)(feat[i] - '0');
      }
    }
    return n;
  }
  for (int r = 0; roots[r] && n < cap; r++) {
    DIR *d = opendir(roots[r]);
    if (!d) continue;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && n < cap) {
      if (ent->d_name[0] == '.') continue;
      /* only titan2_* plane tokens */
      if (strncmp(ent->d_name, "titan2_", 7) != 0) continue;
      char path[768];
      snprintf(path, sizeof path, "%s/%s", roots[r], ent->d_name);
      size_t fl = 0;
      char *body = ng_read_file(path, &fl);
      if (!body) continue;
      unsigned h = 2166136261u;
      for (size_t i = 0; i < fl && i < 256; i++) {
        h ^= (unsigned char)body[i];
        h *= 16777619u;
      }
      free(body);
      /* name hash too */
      for (const char *p = ent->d_name; *p; p++) {
        h ^= (unsigned char)*p;
        h *= 16777619u;
      }
      out[n++] = (uint8_t)(h % 10);
    }
    closedir(d);
  }
  g_plane_samples++;
  return n;
}

/* ---- train control: base64 + fork-safe cmd files (reset/import) ---- */
static char *b64_encode(const uint8_t *in, size_t n) {
  static const char T[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t out_n = 4 * ((n + 2) / 3) + 1;
  char *out = (char *)malloc(out_n);
  size_t i, o = 0;
  if (!out) return NULL;
  for (i = 0; i < n; i += 3) {
    uint32_t v = ((uint32_t)in[i]) << 16;
    if (i + 1 < n) v |= ((uint32_t)in[i + 1]) << 8;
    if (i + 2 < n) v |= (uint32_t)in[i + 2];
    out[o++] = T[(v >> 18) & 63];
    out[o++] = T[(v >> 12) & 63];
    out[o++] = (i + 1 < n) ? T[(v >> 6) & 63] : '=';
    out[o++] = (i + 2 < n) ? T[v & 63] : '=';
  }
  out[o] = 0;
  return out;
}

static int b64_dec_val(int c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

static uint8_t *b64_decode(const char *in, size_t *out_n) {
  size_t len, i, o = 0, cap;
  uint8_t *out;
  if (!in || !out_n) return NULL;
  len = strlen(in);
  cap = (len / 4) * 3 + 4;
  out = (uint8_t *)malloc(cap);
  if (!out) return NULL;
  for (i = 0; i + 3 < len; ) {
    int a, b, c, d;
    while (i < len && (in[i] == '\n' || in[i] == '\r' || in[i] == ' ')) i++;
    if (i + 3 >= len) break;
    a = b64_dec_val(in[i++]); b = b64_dec_val(in[i++]);
    c = in[i] == '=' ? -2 : b64_dec_val(in[i]); i++;
    d = in[i] == '=' ? -2 : b64_dec_val(in[i]); i++;
    if (a < 0 || b < 0) break;
    out[o++] = (uint8_t)((a << 2) | (b >> 4));
    if (c >= 0) out[o++] = (uint8_t)(((b & 15) << 4) | (c >> 2));
    if (d >= 0) out[o++] = (uint8_t)(((c & 3) << 6) | d);
  }
  *out_n = o;
  return out;
}

static void chain_hard_reset_locked(void) {
  char path[700];
  int i;
  g_chain_live = 0;
  free(g_lane); free(g_coord);
  g_lane = NULL; g_coord = NULL;
  g_chain_ticks = g_chain_agree = g_chain_conflict = 0;
  g_teaches_ok = g_teaches_bad = g_self_teaches = g_agent_teaches = 0;
  g_ticks = g_decides = 0;
  g_last_decision = g_last_teach_want = -1;
  g_agent_want = -1; g_agent_until = 0; g_agent_note[0] = 0;
  snprintf(g_last_teach_src, sizeof g_last_teach_src, "none");
  for (i = 0; i < NG_BC_SENSORS; i++) {
    g_lane_fire[i] = 0; g_sensor_val[i] = 0; g_lane_ticks[i] = 0; g_activity[i] = 0;
  }
  chain_state_path(path, sizeof path);
  unlink(path);
  chain_init_locked();
  chain_persist_locked();
  write_live_snap_locked();
  g_resets++;
}

static void agent_service_flag_set(int on) {
  char path[700], body[8];
  g_agent_service = on ? 1 : 0;
  ng_settings_set("braincube_agent_service", on ? "1" : "0");
  ensure_dir();
  snprintf(path, sizeof path, "%s/braincube/agent_service.flag", ng_workdir());
  if (on) {
    snprintf(body, sizeof body, "1\n");
    ng_write_file(path, body, 2);
  } else {
    unlink(path);
  }
}

/* Parent-only: honor fork-written control commands. */
static void train_cmd_poll_locked(void) {
  char path[700], ipath[700];
  ensure_dir();
  snprintf(path, sizeof path, "%s/braincube/cmd.reset", ng_workdir());
  if (access(path, F_OK) == 0) {
    unlink(path);
    chain_hard_reset_locked();
  }
  snprintf(path, sizeof path, "%s/braincube/cmd.import", ng_workdir());
  snprintf(ipath, sizeof ipath, "%s/braincube/chain_import.bin", ng_workdir());
  if (access(path, F_OK) == 0) {
    unlink(path);
    /* load import blob over chain_state then reload */
    {
      char dest[700];
      chain_state_path(dest, sizeof dest);
      rename(ipath, dest);
      g_chain_live = 0;
      free(g_lane); free(g_coord);
      g_lane = NULL; g_coord = NULL;
      if (!chain_load_locked()) chain_init_locked();
      write_live_snap_locked();
    }
  }
  snprintf(path, sizeof path, "%s/braincube/cmd.session_start", ng_workdir());
  if (access(path, F_OK) == 0) {
    unlink(path);
    g_session_start = time(NULL);
    snprintf(g_session_id, sizeof g_session_id, "sess-%ld", (long)g_session_start);
    g_continuous = 1; g_self_teach = 1; g_enabled = 1;
    ng_settings_set("braincube_continuous", "1");
    ng_settings_set("braincube_self_teach", "1");
    agent_service_flag_set(1);
    if (!g_adapt_run) {
      /* unlock briefly not needed — called under lock from adapt; start if needed from outside */
    }
  }
}

/* Parent continuous learning: sample → teach (agent|self) → tick → snap → persist. */
/* Only one nanobot process on the robot may own continuous learn (8787 vs 8788). */
static int g_learn_lock_fd = -1;
static int acquire_learn_singleton(void) {
  char path[700];
  struct flock fl;
  if (g_learn_lock_fd >= 0) return 1;
  ensure_dir();
  snprintf(path, sizeof path, "%s/braincube/learn.lock", ng_workdir());
  g_learn_lock_fd = open(path, O_RDWR | O_CREAT, 0600);
  if (g_learn_lock_fd < 0) return 0;
  memset(&fl, 0, sizeof fl);
  fl.l_type = F_WRLCK;
  fl.l_whence = SEEK_SET;
  if (fcntl(g_learn_lock_fd, F_SETLK, &fl) != 0) {
    close(g_learn_lock_fd);
    g_learn_lock_fd = -1;
    return 0;
  }
  return 1;
}

static void *adapt_loop(void *arg) {
  (void)arg;
  while (g_adapt_run) {
    uint8_t feat[16];
    size_t nf;
    int teach = -1, i;
    pthread_mutex_lock(&g_mu);
#if NANOBOT_HAS_BRAINCUBE
    /* Always poll control cmds even if continuous paused */
    train_cmd_poll_locked();
    if (g_enabled && (g_continuous || g_auto_adapt)) {
      if (!g_chain_live) chain_init_locked();
      supervise_poll_locked();
      nf = sample_clanker_sensors(feat, sizeof feat);
      if (nf == 0) nf = sample_plane_digits(feat, sizeof feat);
      if (nf == 0) { feat[0] = 1; feat[1] = 0; nf = 2; }
      /* Pad to 8 sensor lanes so self-teacher always has a world. */
      for (i = (int)nf; i < NG_BC_SENSORS; i++)
        feat[i] = (uint8_t)(g_sensor_val[i] % 10);
      if (nf < (size_t)NG_BC_SENSORS) nf = (size_t)NG_BC_SENSORS;
      /* Agent supervision wins while TTL active; else self-teacher. */
      if (g_agent_want >= 0 && g_agent_want < NG_BC_SENSORS &&
          (g_agent_until == 0 || time(NULL) <= g_agent_until)) {
        teach = g_agent_want;
        snprintf(g_last_teach_src, sizeof g_last_teach_src, "agent");
        g_agent_teaches++;
      } else if (g_self_teach) {
        teach = self_teacher_want(feat, nf);
        if (teach < 0) teach = 5; /* always train something */
        snprintf(g_last_teach_src, sizeof g_last_teach_src, "self");
        g_self_teaches++;
      } else {
        teach = -1;
        snprintf(g_last_teach_src, sizeof g_last_teach_src, "none");
      }
      if (g_chain_live && nf > 0) {
        int d = chain_tick_locked(feat, nf, teach);
        g_ticks++;
        g_last_decision = d;
        g_decides++;
        write_live_snap_locked();
        if ((g_chain_ticks % 8) == 0) chain_persist_locked();
      }
    }
#else
    (void)nf;
    (void)teach;
    (void)i;
#endif
    pthread_mutex_unlock(&g_mu);
    usleep(500000); /* 2 Hz continuous learn — gentle on rockctl + ARMv7 */
  }
  /* final persist on stop */
  pthread_mutex_lock(&g_mu);
#if NANOBOT_HAS_BRAINCUBE
  if (g_chain_live) chain_persist_locked();
#endif
  pthread_mutex_unlock(&g_mu);
  return NULL;
}

static void start_learn_thread_locked(void) {
  if (g_adapt_run) return;
  if (!acquire_learn_singleton()) {
    /* Another peer already owns continuous learn — we only serve snapshots. */
    g_continuous = 0;
    return;
  }
  g_adapt_run = 1;
  pthread_create(&g_adapt_thr, NULL, adapt_loop, NULL);
  pthread_detach(g_adapt_thr);
}

void ng_bc_set_auto_adapt(int on) {
  ng_bc_init();
  pthread_mutex_lock(&g_mu);
  g_auto_adapt = on ? 1 : 0;
  ng_settings_set("braincube_auto_adapt", on ? "1" : "0");
  if (on) start_learn_thread_locked();
  else if (!g_continuous) g_adapt_run = 0;
  pthread_mutex_unlock(&g_mu);
}

void ng_bc_set_continuous(int on) {
  ng_bc_init();
  pthread_mutex_lock(&g_mu);
  g_continuous = on ? 1 : 0;
  ng_settings_set("braincube_continuous", on ? "1" : "0");
  if (on) start_learn_thread_locked();
  else if (!g_auto_adapt) g_adapt_run = 0;
  pthread_mutex_unlock(&g_mu);
}

void ng_bc_boot_parent(void) {
  ng_bc_init();
  /* Default continuous learning in parent before any HTTP fork. */
  if (g_continuous || g_auto_adapt) {
    pthread_mutex_lock(&g_mu);
    start_learn_thread_locked();
    pthread_mutex_unlock(&g_mu);
  }
}

int ng_bc_continuous(void) { return g_continuous; }

void ng_bc_set_direct_io(int on) {
  ng_bc_init();
  pthread_mutex_lock(&g_mu);
  g_direct_io = on ? 1 : 0;
  ng_settings_set("braincube_direct_io", on ? "1" : "0");
  pthread_mutex_unlock(&g_mu);
}

char *ng_bc_live_json(void) {
  ng_bc_init();
  char *jb = NULL;
#if NANOBOT_HAS_BRAINCUBE
  /* Prefer parent continuous snapshot (fork workers see real training stats). */
  jb = read_live_snap_if_fresh(5);
  if (jb) return jb;
  pthread_mutex_lock(&g_mu);
  jb = chain_live_json_locked();
  pthread_mutex_unlock(&g_mu);
#else
  jb = strdup("{\"ok\":false,\"live\":false,\"available\":false}");
#endif
  return jb ? jb : strdup("{\"ok\":false}");
}

char *ng_bc_status_json(void) {
  char *snap = NULL;
  char *jb = NULL;
#if NANOBOT_HAS_BRAINCUBE
  snap = read_live_snap_if_fresh(8);
  if (snap) {
    /* compact learn status from parent snap fields if present */
    asprintf(&jb,
      "{\"ok\":true,\"plugin\":\"braincube\",\"available\":true,"
      "\"brain\":\"sensor_cubes+meta\",\"sensors\":8,"
      "\"continuous\":true,\"learning\":true,"
      "\"note\":\"parent continuous learn; see action=live or learn_status\","
      "\"snap\":true}");
    free(snap);
    if (jb) return jb;
  }
#endif
  asprintf(&jb,
    "{\"ok\":true,\"plugin\":\"braincube\",\"available\":true,"
    "\"brain\":\"sensor_cubes+meta\",\"sensors\":8,"
    "\"continuous\":%s,\"self_teach\":%s,"
    "\"note\":\"probe_ok; continuous learn in parent when enabled\"}",
    g_continuous ? "true" : "false",
    g_self_teach ? "true" : "false");
  return jb ? jb : strdup("{\"ok\":true,\"plugin\":\"braincube\"}");
}

char *ng_bc_handle_post(const char *json_body) {
  ng_bc_init();
  if (!json_body) json_body = "{}";
  char *action = ng_json_get_string(json_body, "action");
  if (!action) action = strdup("status");

  if (!strcmp(action, "status")) {
    free(action);
    return ng_bc_status_json();
  }
  if (!strcmp(action, "live") || !strcmp(action, "brain")) {
    free(action);
    return ng_bc_live_json();
  }
  if (!strcmp(action, "chain_tick") || !strcmp(action, "sense")) {
    /* Observation only — continuous parent does training; avoid double-init in fork. */
    free(action);
    return ng_bc_live_json();
  }

  if (!strcmp(action, "learn_status") || !strcmp(action, "training")) {
    char *snap;
    free(action);
    snap = NULL;
#if NANOBOT_HAS_BRAINCUBE
    snap = read_live_snap_if_fresh(30);
#endif
    if (snap) return snap;
    return strdup("{\"ok\":false,\"learning\":false,\"error\":\"no live snapshot yet — is continuous on?\"}");
  }

  if (!strcmp(action, "continuous")) {
    char *v = ng_json_get_string(json_body, "value");
    int on = !v || v[0] == '1' || !strcasecmp(v, "true") || !strcasecmp(v, "on");
    if (v && (v[0] == '0' || !strcasecmp(v, "false") || !strcasecmp(v, "off"))) on = 0;
    free(v);
    ng_bc_set_continuous(on);
    free(action);
    return ng_bc_status_json();
  }

  if (!strcmp(action, "self_teach")) {
    char *v = ng_json_get_string(json_body, "value");
    int on = !v || v[0] == '1' || !strcasecmp(v, "true") || !strcasecmp(v, "on");
    if (v && (v[0] == '0' || !strcasecmp(v, "false"))) on = 0;
    free(v);
    pthread_mutex_lock(&g_mu);
    g_self_teach = on ? 1 : 0;
    ng_settings_set("braincube_self_teach", on ? "1" : "0");
    pthread_mutex_unlock(&g_mu);
    free(action);
    return ng_bc_status_json();
  }

  /* Occasional agent supervision: focus a lane for ttl_sec (default 45). */
  if (!strcmp(action, "supervise") || !strcmp(action, "agent_teach")) {
#if NANOBOT_HAS_BRAINCUBE
    char *w = ng_json_get_string(json_body, "want");
    char *teacher = ng_json_get_string(json_body, "teacher");
    char *note = ng_json_get_string(json_body, "note");
    char *ttl_s = ng_json_get_string(json_body, "ttl_sec");
    int want = -1, ttl = 45;
    if (w) want = lane_from_token(w);
    else if (teacher) want = lane_from_token(teacher);
    if (ttl_s) ttl = atoi(ttl_s);
    else if (json_body) {
      /* number form "ttl_sec":30 */
      const char *p = strstr(json_body, "\"ttl_sec\"");
      if (p) { p = strchr(p, ':'); if (p) ttl = atoi(p + 1); }
    }
    free(w); free(teacher); free(ttl_s);
    if (want < 0 || want >= NG_BC_SENSORS) {
      free(note); free(action);
      return strdup("{\"ok\":false,\"error\":\"want lane 0-7 or name (bump_L..free_ok)\"}");
    }
    supervise_write(want, ttl, note);
    free(note);
    {
      char *jb = NULL;
      asprintf(&jb,
        "{\"ok\":true,\"supervised\":true,\"want\":%d,\"want_name\":\"%s\","
        "\"ttl_sec\":%d,\"note\":\"agent supervision queued for parent learner\"}",
        want, g_lane_name[want], ttl);
      free(action);
      return jb ? jb : strdup("{\"ok\":true}");
    }
#else
    free(action);
    return strdup("{\"ok\":false}");
#endif
  }

  if (!strcmp(action, "enable") || !strcmp(action, "disable")) {
    int on = !strcmp(action, "enable");
    pthread_mutex_lock(&g_mu);
    g_enabled = on;
    ng_settings_set("braincube_enabled", on ? "1" : "0");
    pthread_mutex_unlock(&g_mu);
    free(action);
    return ng_bc_status_json();
  }

  if (!strcmp(action, "auto_adapt")) {
    char *v = ng_json_get_string(json_body, "value");
    int on = v && (v[0] == '1' || !strcasecmp(v, "true") || !strcasecmp(v, "on"));
    free(v);
    ng_bc_set_auto_adapt(on);
    free(action);
    return ng_bc_status_json();
  }

  if (!strcmp(action, "direct_io")) {
    char *v = ng_json_get_string(json_body, "value");
    int on = v && (v[0] == '1' || !strcasecmp(v, "true") || !strcasecmp(v, "on"));
    free(v);
    ng_bc_set_direct_io(on);
    free(action);
    return ng_bc_status_json();
  }

  if (!strcmp(action, "dry_run")) {
    char *v = ng_json_get_string(json_body, "value");
    int on = !v || v[0] != '0'; /* default true */
    if (v && (v[0] == '1' || !strcasecmp(v, "true"))) on = 1;
    if (v && (v[0] == '0' || !strcasecmp(v, "false"))) on = 0;
    free(v);
    pthread_mutex_lock(&g_mu);
    g_dry_run = on;
    ng_settings_set("braincube_dry_run", on ? "1" : "0");
    pthread_mutex_unlock(&g_mu);
    free(action);
    return ng_bc_status_json();
  }

  if (!strcmp(action, "sample") || !strcmp(action, "tick") || !strcmp(action, "decide")) {
#if NANOBOT_HAS_BRAINCUBE
    uint8_t feat[16];
    size_t nf;
    pthread_mutex_lock(&g_mu);
    nf = sample_plane_digits(feat, sizeof feat);
    if (nf == 0) {
      /* synthetic heartbeat features if plane empty */
      feat[0] = (uint8_t)(time(NULL) % 10);
      feat[1] = (uint8_t)((g_ticks + 3) % 10);
      nf = 2;
    }
    if (g_cube_live) {
      lhlam_cube_tick(&g_cube, feat, nf);
      g_ticks++;
      if (strcmp(action, "tick") != 0) {
        g_last_decision = lhlam_cube_decide(&g_cube, feat, nf);
        g_decides++;
      }
      lhlam_cube_stats(&g_cube, g_last_stats, sizeof g_last_stats);
      persist_locked();
    }
    pthread_mutex_unlock(&g_mu);
#else
    (void)0;
#endif
    free(action);
    return ng_bc_status_json();
  }

  if (!strcmp(action, "feedback") || !strcmp(action, "teach")) {
#if NANOBOT_HAS_BRAINCUBE
    /* Prefer chain supervision: want=lane. Fallback: legacy single-cube teacher bit. */
    char *w = ng_json_get_string(json_body, "want");
    char *tb = ng_json_get_string(json_body, "teacher");
    char *note = ng_json_get_string(json_body, "note");
    char *ttl_s = ng_json_get_string(json_body, "ttl_sec");
    int want = -1, ttl = 20;
    if (w) want = lane_from_token(w);
    else if (tb && (isalpha((unsigned char)tb[0]) || strlen(tb) > 1))
      want = lane_from_token(tb);
    if (ttl_s) ttl = atoi(ttl_s);
    if (want >= 0 && want < NG_BC_SENSORS) {
      supervise_write(want, ttl, note ? note : "teach");
      free(w); free(tb); free(note); free(ttl_s); free(action);
      {
        char *jb = NULL;
        asprintf(&jb,
          "{\"ok\":true,\"taught\":true,\"mode\":\"supervise\",\"want\":%d,"
          "\"want_name\":\"%s\",\"ttl_sec\":%d}",
          want, g_lane_name[want], ttl);
        return jb ? jb : strdup("{\"ok\":true}");
      }
    }
    free(w); free(note); free(ttl_s);
    {
      char *okb = ng_json_get_string(json_body, "ok");
      int teacher = 0, ok = 1;
      if (tb) teacher = (tb[0] == '1') ? 1 : 0;
      if (okb) ok = (okb[0] != '0');
      free(tb); free(okb);
      /* queue as free_ok / state nudge via supervise when possible */
      (void)teacher; (void)ok;
    }
#endif
    free(action);
    return ng_bc_status_json();
  }

  if (!strcmp(action, "reset") || !strcmp(action, "chain_reset") ||
      !strcmp(action, "brain_reset")) {
#if NANOBOT_HAS_BRAINCUBE
    /* Fork-safe: queue for parent; also try in-process if we hold learn lock. */
    {
      char path[700];
      ensure_dir();
      snprintf(path, sizeof path, "%s/braincube/cmd.reset", ng_workdir());
      ng_write_file(path, "1\n", 2);
    }
    pthread_mutex_lock(&g_mu);
    if (g_adapt_run || g_learn_lock_fd >= 0) {
      char p2[700];
      chain_hard_reset_locked();
      snprintf(p2, sizeof p2, "%s/braincube/cmd.reset", ng_workdir());
      unlink(p2);
    }
    pthread_mutex_unlock(&g_mu);
    free(action);
    {
      char *jb = NULL;
      asprintf(&jb, "{\"ok\":true,\"reset\":true,\"note\":\"brain chain reset queued/applied\"}");
      return jb ? jb : strdup("{\"ok\":true,\"reset\":true}");
    }
#else
    free(action);
    return strdup("{\"ok\":false}");
#endif
  }

  if (!strcmp(action, "export") || !strcmp(action, "chain_export")) {
#if NANOBOT_HAS_BRAINCUBE
    {
      char path[700];
      size_t blen = 0;
      char *raw;
      char *b64;
      char *jb = NULL;
      ensure_dir();
      /* flush if parent */
      pthread_mutex_lock(&g_mu);
      if (g_chain_live) chain_persist_locked();
      pthread_mutex_unlock(&g_mu);
      chain_state_path(path, sizeof path);
      raw = ng_read_file(path, &blen);
      if (!raw || blen == 0) {
        free(raw); free(action);
        return strdup("{\"ok\":false,\"error\":\"no chain_state.bin yet\"}");
      }
      b64 = b64_encode((const uint8_t *)raw, blen);
      free(raw);
      if (!b64) { free(action); return strdup("{\"ok\":false,\"error\":\"b64 OOM\"}"); }
      asprintf(&jb,
        "{\"ok\":true,\"format\":\"chain_state_b64\",\"bytes\":%zu,\"resets\":%u,"
        "\"session_id\":\"%s\",\"data\":\"%s\"}",
        blen, g_resets, g_session_id[0] ? g_session_id : "", b64);
      free(b64);
      free(action);
      return jb ? jb : strdup("{\"ok\":false}");
    }
#else
    free(action);
    return strdup("{\"ok\":false}");
#endif
  }

  if (!strcmp(action, "import") || !strcmp(action, "chain_import")) {
#if NANOBOT_HAS_BRAINCUBE
    {
      char *data = ng_json_get_string(json_body, "data");
      size_t raw_n = 0;
      uint8_t *raw;
      char ipath[700], cpath[700];
      if (!data || !data[0]) {
        free(data); free(action);
        return strdup("{\"ok\":false,\"error\":\"need data=base64 chain_state\"}");
      }
      raw = b64_decode(data, &raw_n);
      free(data);
      if (!raw || raw_n < 16) {
        free(raw); free(action);
        return strdup("{\"ok\":false,\"error\":\"bad base64 payload\"}");
      }
      ensure_dir();
      snprintf(ipath, sizeof ipath, "%s/braincube/chain_import.bin", ng_workdir());
      ng_write_file(ipath, (const char *)raw, raw_n);
      free(raw);
      snprintf(cpath, sizeof cpath, "%s/braincube/cmd.import", ng_workdir());
      ng_write_file(cpath, "1\n", 2);
      pthread_mutex_lock(&g_mu);
      if (g_adapt_run || g_learn_lock_fd >= 0) {
        char dest[700];
        chain_state_path(dest, sizeof dest);
        rename(ipath, dest);
        g_chain_live = 0;
        free(g_lane); free(g_coord);
        g_lane = NULL; g_coord = NULL;
        if (!chain_load_locked()) chain_init_locked();
        write_live_snap_locked();
        unlink(cpath);
      }
      pthread_mutex_unlock(&g_mu);
      free(action);
      return strdup("{\"ok\":true,\"imported\":true,\"note\":\"chain imported\"}");
    }
#else
    free(action);
    return strdup("{\"ok\":false}");
#endif
  }

  if (!strcmp(action, "session_start") || !strcmp(action, "train_start")) {
#if NANOBOT_HAS_BRAINCUBE
    {
      char *sid = ng_json_get_string(json_body, "session_id");
      char *note = ng_json_get_string(json_body, "note");
      pthread_mutex_lock(&g_mu);
      g_session_start = time(NULL);
      if (sid && sid[0])
        snprintf(g_session_id, sizeof g_session_id, "%s", sid);
      else
        snprintf(g_session_id, sizeof g_session_id, "sess-%ld", (long)g_session_start);
      g_enabled = 1; g_continuous = 1; g_self_teach = 1;
      ng_settings_set("braincube_continuous", "1");
      ng_settings_set("braincube_self_teach", "1");
      agent_service_flag_set(1);
      start_learn_thread_locked();
      write_live_snap_locked();
      pthread_mutex_unlock(&g_mu);
      /* also file marker for shell agent */
      {
        char path[700], body[256];
        int n = snprintf(body, sizeof body,
          "session_id=%s\nstart=%ld\nnote=%s\nagent_service=1\n",
          g_session_id, (long)g_session_start, note && note[0] ? note : "wrapper");
        ensure_dir();
        snprintf(path, sizeof path, "%s/braincube/session.env", ng_workdir());
        ng_write_file(path, body, (size_t)n);
      }
      free(sid); free(note); free(action);
      {
        char *jb = NULL;
        asprintf(&jb,
          "{\"ok\":true,\"session_start\":true,\"session_id\":\"%s\","
          "\"continuous\":true,\"self_teach\":true,\"agent_service\":true}",
          g_session_id);
        return jb ? jb : strdup("{\"ok\":true}");
      }
    }
#else
    free(action);
    return strdup("{\"ok\":false}");
#endif
  }

  if (!strcmp(action, "session_stop") || !strcmp(action, "train_stop")) {
    ng_bc_set_continuous(0);
    pthread_mutex_lock(&g_mu);
    agent_service_flag_set(0);
    g_session_id[0] = 0;
    g_session_start = 0;
    pthread_mutex_unlock(&g_mu);
    free(action);
    return strdup("{\"ok\":true,\"session_stop\":true,\"agent_service\":false}");
  }

  if (!strcmp(action, "agent_service")) {
    char *v = ng_json_get_string(json_body, "value");
    int on = !v || v[0] == '1' || !strcasecmp(v, "true") || !strcasecmp(v, "on");
    if (v && (v[0] == '0' || !strcasecmp(v, "false") || !strcasecmp(v, "off"))) on = 0;
    free(v);
    pthread_mutex_lock(&g_mu);
    agent_service_flag_set(on);
    pthread_mutex_unlock(&g_mu);
    free(action);
    {
      char *jb = NULL;
      asprintf(&jb, "{\"ok\":true,\"agent_service\":%s}", on ? "true" : "false");
      return jb ? jb : strdup("{\"ok\":true}");
    }
  }

  if (!strcmp(action, "control") || !strcmp(action, "train_panel")) {
    /* Dashboard JSON for Training tab */
    char *snap = NULL;
#if NANOBOT_HAS_BRAINCUBE
    snap = read_live_snap_if_fresh(15);
#endif
    if (snap) {
      free(action);
      return snap;
    }
    free(action);
    {
      char *jb = NULL;
      asprintf(&jb,
        "{\"ok\":true,\"learning\":false,\"continuous\":%s,\"self_teach\":%s,"
        "\"agent_service\":%s,\"session_id\":\"%s\",\"resets\":%u,"
        "\"note\":\"no live snap yet — start session\"}",
        g_continuous ? "true" : "false", g_self_teach ? "true" : "false",
        g_agent_service ? "true" : "false",
        g_session_id[0] ? g_session_id : "", g_resets);
      return jb ? jb : strdup("{\"ok\":true}");
    }
  }

  /* Field trials: enable robot motion experiments (writes field_trials.flag). */
  if (!strcmp(action, "field_trials") || !strcmp(action, "motion_trials")) {
    char *v = ng_json_get_string(json_body, "value");
    int on = v && (v[0] == '1' || !strcasecmp(v, "true") || !strcasecmp(v, "on"));
    if (v && (v[0] == '0' || !strcasecmp(v, "false") || !strcasecmp(v, "off"))) on = 0;
    if (!v) on = 1;
    free(v);
    {
      char path[700];
      ensure_dir();
      snprintf(path, sizeof path, "%s/braincube/field_trials.flag", ng_workdir());
      if (on) ng_write_file(path, "1\n", 2);
      else unlink(path);
      if (on) ng_bc_log_supervision_oneliner("field_trials=on");
      else ng_bc_log_supervision_oneliner("field_trials=off");
    }
    free(action);
    {
      char *jb = NULL;
      asprintf(&jb,
        "{\"ok\":true,\"field_trials\":%s,"
        "\"note\":\"%s\"}",
        on ? "true" : "false",
        on ? "motion trials enabled — robot will RC-move when undocked"
           : "motion trials disabled");
      return jb ? jb : strdup("{\"ok\":true}");
    }
  }

  /* Explore: freer RC mess-around to build associations (explore.flag). */
  if (!strcmp(action, "explore") || !strcmp(action, "explore_mode")) {
    char *v = ng_json_get_string(json_body, "value");
    int on = v && (v[0] == '1' || !strcasecmp(v, "true") || !strcasecmp(v, "on"));
    if (v && (v[0] == '0' || !strcasecmp(v, "false") || !strcasecmp(v, "off"))) on = 0;
    if (!v) on = 1;
    free(v);
    {
      char path[700];
      ensure_dir();
      snprintf(path, sizeof path, "%s/braincube/explore.flag", ng_workdir());
      if (on) {
        ng_write_file(path, "1\n", 2);
        /* explore implies continuous + agent + field-ish */
        ng_bc_set_continuous(1);
        agent_service_flag_set(1);
        snprintf(path, sizeof path, "%s/braincube/field_trials.flag", ng_workdir());
        ng_write_file(path, "1\n", 2);
        ng_bc_log_supervision_oneliner("explore=on (RC free-roam association)");
      } else {
        unlink(path);
        ng_bc_log_supervision_oneliner("explore=off");
      }
    }
    free(action);
    {
      char *jb = NULL;
      asprintf(&jb,
        "{\"ok\":true,\"explore\":%s,\"field_trials\":%s,"
        "\"note\":\"%s\"}",
        on ? "true" : "false", on ? "true" : "false",
        on ? "explore ON — undock to mess around in RC and build associations"
           : "explore off");
      return jb ? jb : strdup("{\"ok\":true}");
    }
  }

  if (!strcmp(action, "train_status") || !strcmp(action, "train_report") ||
      !strcmp(action, "hows_training")) {
    char *rep = ng_bc_train_status_report();
    char *esc = NULL, *jb = NULL;
    /* return both text and json wrapper for tools */
    free(action);
    if (!rep) return strdup("{\"ok\":false}");
    /* embed as JSON string (escape quotes/newlines lightly) */
    {
      size_t i, o = 0, L = strlen(rep);
      esc = (char *)malloc(L * 2 + 4);
      if (esc) {
        for (i = 0; i < L; i++) {
          char c = rep[i];
          if (c == '"' || c == '\\') { esc[o++] = '\\'; esc[o++] = c; }
          else if (c == '\n') { esc[o++] = '\\'; esc[o++] = 'n'; }
          else if (c == '\r') continue;
          else esc[o++] = c;
        }
        esc[o] = 0;
      }
    }
    asprintf(&jb, "{\"ok\":true,\"plugin_version\":\"%s\",\"report\":\"%s\"}",
             NG_BC_PLUGIN_VERSION, esc ? esc : "");
    free(rep); free(esc);
    return jb ? jb : strdup("{\"ok\":true}");
  }

  /* Last N field trials + latest + field status (results of trying things). */
  if (!strcmp(action, "trials") || !strcmp(action, "results")) {
    char path[700], spath[700], lpath[700], fpath[700];
    char *body = NULL, *st = NULL, *lat = NULL;
    size_t blen = 0, slen = 0, llen = 0;
    char *jb = NULL;
    char trials_arr[12000];
    size_t o = 0;
    int nlines = 0, field_on = 0;
    const char *collected[24];
    int nc = 0;
    ensure_dir();
    snprintf(path, sizeof path, "%s/braincube/trials.jsonl", ng_workdir());
    snprintf(spath, sizeof spath, "%s/braincube/field_status.json", ng_workdir());
    snprintf(lpath, sizeof lpath, "%s/braincube/latest_trial.json", ng_workdir());
    snprintf(fpath, sizeof fpath, "%s/braincube/field_trials.flag", ng_workdir());
    field_on = (access(fpath, F_OK) == 0);
    body = ng_read_file(path, &blen);
    st = ng_read_file(spath, &slen);
    lat = ng_read_file(lpath, &llen);
    o += (size_t)snprintf(trials_arr + o, sizeof trials_arr - o, "[");
    if (body && blen > 0) {
      char *copy = (char *)malloc(blen + 1);
      char *p, *save;
      if (copy) {
        memcpy(copy, body, blen);
        copy[blen] = 0;
        for (p = strtok_r(copy, "\n", &save); p; p = strtok_r(NULL, "\n", &save)) {
          if (p[0] != '{') continue;
          if (nc < 24) collected[nc++] = p;
          else {
            int k;
            for (k = 0; k < 23; k++) collected[k] = collected[k + 1];
            collected[23] = p;
          }
        }
        {
          int i, start = nc > 12 ? nc - 12 : 0;
          for (i = start; i < nc; i++) {
            o += (size_t)snprintf(trials_arr + o, sizeof trials_arr - o, "%s%s",
                                  nlines ? "," : "", collected[i]);
            nlines++;
            if (o + 200 >= sizeof trials_arr) break;
          }
        }
        free(copy);
      }
    }
    o += (size_t)snprintf(trials_arr + o, sizeof trials_arr - o, "]");
    asprintf(&jb,
      "{\"ok\":true,\"field_trials\":%s,\"count\":%d,"
      "\"latest\":%s,"
      "\"field_status\":%s,"
      "\"trials\":%s,"
      "\"how_to_read\":\"Each trial is a motion the robot tried. "
      "clear=free path; bump=hit; skip/docked=no move. reward 1=good. "
      "Without field_trials only attention trains (no motion).\"}",
      field_on ? "true" : "false", nlines,
      (lat && llen > 2) ? lat : "null",
      (st && slen > 2) ? st : "null",
      trials_arr);
    free(body); free(st); free(lat);
    free(action);
    return jb ? jb : strdup("{\"ok\":true,\"trials\":[]}");
  }

  if (!strcmp(action, "selftest")) {
#if NANOBOT_HAS_BRAINCUBE
    int rc = lhlam_selftest();
    char *jb = NULL;
    asprintf(&jb, "{\"ok\":%s,\"selftest_rc\":%d,\"learning_evidence\":%s}",
      rc == 0 ? "true" : "false", rc, rc == 0 ? "true" : "false");
    free(action);
    return jb ? jb : strdup("{\"ok\":false}");
#else
    free(action);
    return strdup("{\"ok\":false,\"error\":\"braincube not linked\"}");
#endif
  }

  /* CubeChain demo API — lanes + coordinator on peer (Titan / Clanker direction). */
  if (!strcmp(action, "chain_status") || !strcmp(action, "cubechain")) {
#if NANOBOT_HAS_BRAINCUBE
    char *jb;
    pthread_mutex_lock(&g_mu);
    jb = chain_status_json_locked();
    pthread_mutex_unlock(&g_mu);
    free(action);
    return jb;
#else
    free(action);
    return strdup("{\"ok\":false,\"error\":\"braincube not linked\"}");
#endif
  }

  if (!strcmp(action, "chain_tick") || !strcmp(action, "chain_demo")) {
#if NANOBOT_HAS_BRAINCUBE
    uint8_t feat[16];
    size_t nf;
    int pick, want = -1;
    char *w = ng_json_get_string(json_body, "want");
    char *teacher = ng_json_get_string(json_body, "teacher");
    if (w) want = atoi(w);
    else if (teacher) want = atoi(teacher);
    free(w);
    free(teacher);
    pthread_mutex_lock(&g_mu);
    nf = sample_plane_digits(feat, sizeof feat);
    pick = chain_tick_locked(feat, nf, want);
    {
      char *jb = chain_status_json_locked();
      pthread_mutex_unlock(&g_mu);
      free(action);
      (void)pick;
      return jb;
    }
#else
    free(action);
    return strdup("{\"ok\":false,\"error\":\"braincube not linked\"}");
#endif
  }

  free(action);
  return strdup("{\"ok\":false,\"error\":\"unknown action\","
    "\"actions\":[\"status\",\"live\",\"sense\",\"learn_status\",\"training\","
    "\"train_status\",\"train_report\",\"hows_training\","
    "\"continuous\",\"self_teach\",\"supervise\",\"agent_teach\",\"teach\","
    "\"session_start\",\"session_stop\",\"train_start\",\"train_stop\","
    "\"explore\",\"field_trials\",\"trials\",\"results\","
    "\"export\",\"import\",\"reset\",\"brain_reset\",\"agent_service\",\"control\","
    "\"auto_adapt\",\"enable\",\"disable\",\"direct_io\",\"dry_run\",\"tick\","
    "\"decide\",\"sample\",\"feedback\",\"selftest\",\"chain_status\",\"chain_tick\"]}");
}
