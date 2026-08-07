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

/*
 * Dual-wire braincube plate tail (schema nanobot.braincube.v1).
 * Product bus remains SMX2; peer HTTP is lab/ops only.
 */
#define NG_BC_DUAL_WIRE                                                        \
  "\"product_wire\":\"smx2\",\"peer_http\":\"lab_ops_only\","                  \
  "\"peer_http_is_product_bus\":false,"                                        \
  "\"share\":\"state_matrix_only\",\"hold_flash\":1,"                          \
  "\"llm_is_commander\":false,\"python\":0"

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
/* Mutable labels: Clanker RC names by default; Commander plane renames in sample. */
static char g_lane_name_buf[NG_BC_SENSORS][24] = {
  "bump_L", "bump_C", "bump_R", "charge",
  "battery", "state", "error", "free_ok"
};
static const char *g_lane_name[NG_BC_SENSORS] = {
  g_lane_name_buf[0], g_lane_name_buf[1], g_lane_name_buf[2], g_lane_name_buf[3],
  g_lane_name_buf[4], g_lane_name_buf[5], g_lane_name_buf[6], g_lane_name_buf[7]
};
/* last world snapshot for live viz */
static int g_world_state = 3, g_world_charge = 0, g_world_battery = 0, g_world_error = 0;
static int g_world_bump[3];
static uint64_t g_live_seq;
/* Useful-learn gates: avoid docked static self-teach spam (empty UI feel). */
static uint8_t g_prev_feat[8];
static int g_prev_feat_ok;
static uint32_t g_useful_teaches;   /* teaches with real info change / clean motion */
static uint32_t g_skipped_teaches;  /* docked/static ticks skipped */
static int g_prev_state = -1;

/* --- First Cube's LAW / Crimson Cube (BlackCube Commander) ---
 * Endless reverse-Rubik game loop: search state-matrix paths so energy
 * travels I→O faster than the peer cube's plug impulse. I and O race.
 * Win = combine algocubes into Meta while keeping edge at CORE_N (small).
 * Energy MUST flow — NexusCore develops only when race is won.
 */
#define LAW_NAME "first_cube"
#define LAW_VERSION "1.0.0"
static uint32_t g_law_ticks;
static uint32_t g_law_races;       /* I/O race attempts */
static uint32_t g_law_wins;        /* impulse exited O first */
static uint32_t g_law_losses;      /* plug beat O (conflict / blocked) */
static uint32_t g_law_combines;    /* successful algocube joins */
static uint32_t g_law_paths;       /* pathfind solutions found */
static uint32_t g_law_path_fail;   /* no open path this tick */
static uint32_t g_law_energy;      /* cumulative flow score (NexusCore fuel) */
static int g_law_last_path_len;    /* last BFS path length */
static int g_law_last_i_ms;        /* simulated I arrival cost */
static int g_law_last_o_ms;        /* simulated O exit cost */
static int g_law_last_plug_ms;     /* peer plug impulse cost */
static int g_law_last_winner;      /* 1=I→O win, 0=plug win, -1=no race */
static int g_law_last_in_cell;     /* meta cell index I port */
static int g_law_last_out_cell;    /* meta cell index O port */
static int g_law_last_algo_a;      /* algocube A lane */
static int g_law_last_algo_b;      /* algocube B lane (combine target) */
static char g_law_status[96];
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

/* Roborock-ish state digits we care about (feat[5] = state % 10). */
static int state_is_cleaning(int st) {
  /* 5 cleaning, 6 returning, 7 manual/RC, 4? some firmwares use 17→7 */
  return st == 5 || st == 6 || st == 7 || st == 4;
}
static int state_is_docked_idle(int st, int charge) {
  /* charge=1 docked; states 8/15/3/2 often idle/charge related */
  if (!charge) return 0;
  if (st == 5 || st == 6 || st == 7) return 0; /* actively working / RC */
  return 1;
}

/* Feature vector changed enough to carry information. */
static int feat_changed(const uint8_t *feat, size_t nf) {
  size_t i;
  if (!g_prev_feat_ok || nf < 8) return 1;
  for (i = 0; i < 8; i++)
    if (feat[i] != g_prev_feat[i]) return 1;
  return 0;
}

/*
 * Sensor-derived teacher: exclusive single lane that *should* be salient.
 * Returns -1 to skip teach (no useful signal) — critical on dock.
 *
 * Useful development rules:
 *  1) Bump / error always teach (real hazard).
 *  2) While cleaning/returning/RC: free_ok when clear, charge when docked mid-run rare.
 *  3) Low battery only when not on dock.
 *  4) Docked + static sensors → NO teach (was inflating self_teaches with garbage).
 *  5) State lane only on state *transition*, not every 2Hz tick.
 */
static int self_teacher_want(const uint8_t *feat, size_t nf) {
  int st, ch, changed, cleaning;
  if (!feat || nf < 8) return -1;
  st = (int)(feat[5] % 10);
  ch = feat[3] >= 5 ? 1 : 0;
  changed = feat_changed(feat, nf);
  cleaning = state_is_cleaning(st);

  /* Hard signals first */
  if (feat[0] >= 5) return 0; /* bump_L */
  if (feat[1] >= 5) return 1; /* bump_C */
  if (feat[2] >= 5) return 2; /* bump_R */
  if (feat[6] >= 5) return 6; /* error */

  /* Docked idle static: do not self-train (biggest source of useless teaches). */
  if (state_is_docked_idle(st, ch) && !changed)
    return -1;

  /* Active work: clear path is the positive class we want the brain to prefer. */
  if (cleaning && feat[7] >= 5)
    return 7; /* free_ok */
  if (cleaning && ch)
    return 3; /* on dock while "working" — rare; notice charge */
  if (!ch && feat[4] <= 2)
    return 4; /* low battery undocked */

  /* State transition only — not default every tick */
  if (g_prev_state >= 0 && st != g_prev_state)
    return 5;

  /* Undocked free room, first samples or mild change → free_ok */
  if (!ch && feat[7] >= 5 && changed)
    return 7;

  /* Docked but something changed (e.g. charge edge, state dig) */
  if (ch && changed) {
    if (feat[3] >= 5) return 3;
    return 5;
  }

  return -1; /* skip: no useful teacher bit this tick */
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

/* Dual-wire train_status plate — machine fields only (no free-text essay). */
char *ng_bc_train_status_report(void) {
  char *snap = NULL, *jb = NULL, *pick_esc = NULL;
  size_t sl = 0;
  char path[700];
  int cont = g_continuous, self = g_self_teach, asvc = g_agent_service;
  int field_on = 0, explore_on = 0;
  int seq = 0, self_t = 0, agent_t = 0, agree = 0;
  char pick[32] = "none";
  ensure_dir();
  snprintf(path, sizeof path, "%s/braincube/live_snap.json", ng_workdir());
  snap = ng_read_file(path, &sl);
  snprintf(path, sizeof path, "%s/braincube/field_trials.flag", ng_workdir());
  field_on = (access(path, F_OK) == 0);
  snprintf(path, sizeof path, "%s/braincube/explore.flag", ng_workdir());
  explore_on = (access(path, F_OK) == 0);
  if (snap) {
    const char *p;
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
  pick_esc = ng_json_escape(pick[0] ? pick : "none");
  asprintf(&jb,
    "{\"schema\":\"nanobot.braincube.v1\",\"ok\":true,"
    "\"action\":\"train_status\",\"plugin_version\":\"%s\","
    "\"continuous\":%s,\"self_teach\":%s,\"agent_service\":%s,"
    "\"field_trials\":%s,\"explore\":%s,"
    "\"seq\":%d,\"self_teaches\":%d,\"agent_teaches\":%d,"
    "\"meta_agree\":%d,\"pick_name\":\"%s\","
    NG_BC_DUAL_WIRE "}",
    NG_BC_PLUGIN_VERSION,
    cont ? "true" : "false", self ? "true" : "false", asvc ? "true" : "false",
    field_on ? "true" : "false", explore_on ? "true" : "false",
    seq, self_t, agent_t, agree, pick_esc ? pick_esc : "none");
  free(snap);
  free(pick_esc);
  return jb ? jb
            : strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
                     "\"action\":\"train_status\",\"error\":\"oom\","
                     "\"python\":0}");
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
    char *src_esc = ng_json_escape(g_last_teach_src[0] ? g_last_teach_src : "none");
    char *note_esc = ng_json_escape(g_agent_note[0] ? g_agent_note : "");
    char *sid_esc = ng_json_escape(g_session_id[0] ? g_session_id : "");
    char *st_esc = ng_json_escape(g_law_status[0] ? g_law_status : "init");
    size_t L = strlen(jb);
    if (L > 2 && jb[L - 1] == '}') {
      jb[L - 1] = 0;
      /* Machine fields only — no free-text principle/game essays; inject-sanitize strings. */
      asprintf(&enriched,
        "%s,\"continuous\":%s,\"self_teach\":%s,\"learning\":true,"
        "\"teaches_ok\":%u,\"teaches_bad\":%u,\"self_teaches\":%u,\"agent_teaches\":%u,"
        "\"useful_teaches\":%u,\"skipped_teaches\":%u,"
        "\"last_teach_want\":%d,\"last_teach_src\":\"%s\","
        "\"agent_want\":%d,\"agent_note\":\"%s\","
        "\"agent_service\":%s,\"session_id\":\"%s\",\"session_age_s\":%ld,\"resets\":%u,"
        "\"law\":{\"name\":\"%s\",\"ver\":\"%s\",\"ticks\":%u,"
        "\"races\":%u,\"wins\":%u,\"losses\":%u,\"combines\":%u,"
        "\"paths\":%u,\"path_fail\":%u,\"energy\":%u,"
        "\"path_len\":%d,\"i_ms\":%d,\"o_ms\":%d,\"plug_ms\":%d,"
        "\"winner\":%d,\"algo_a\":%d,\"algo_b\":%d,"
        "\"i_cell\":%d,\"o_cell\":%d,"
        "\"status\":\"%s\"}}",
        jb,
        g_continuous ? "true" : "false",
        g_self_teach ? "true" : "false",
        g_teaches_ok, g_teaches_bad, g_self_teaches, g_agent_teaches,
        g_useful_teaches, g_skipped_teaches,
        g_last_teach_want, src_esc ? src_esc : "none",
        g_agent_want,
        note_esc ? note_esc : "",
        g_agent_service ? "true" : "false",
        sid_esc ? sid_esc : "",
        g_session_start ? (long)(time(NULL) - g_session_start) : 0L,
        g_resets,
        LAW_NAME, LAW_VERSION, g_law_ticks,
        g_law_races, g_law_wins, g_law_losses, g_law_combines,
        g_law_paths, g_law_path_fail, g_law_energy,
        g_law_last_path_len, g_law_last_i_ms, g_law_last_o_ms, g_law_last_plug_ms,
        g_law_last_winner, g_law_last_algo_a, g_law_last_algo_b,
        g_law_last_in_cell, g_law_last_out_cell,
        st_esc ? st_esc : "init");
      free(jb);
      jb = enriched;
    }
    free(src_esc);
    free(note_esc);
    free(sid_esc);
    free(st_esc);
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

/* BFS on meta cube lattice: open ports = cells with neuron OR high digit.
 * Keeps meta edge small (CORE_N) — reverse-Rubik: solve without growing volume. */
static int law_idx(int n, int x, int y, int z) {
  return (z * n + y) * n + x;
}
static void law_xyz(int n, int idx, int *x, int *y, int *z) {
  *x = idx % n;
  *y = (idx / n) % n;
  *z = idx / (n * n);
}
static int law_open_cell(const lhlam_cube *c, int idx) {
  unsigned n, nn;
  if (!c) return 0;
  n = c->n ? c->n : LHLAM_CORE_N;
  nn = n * n * n;
  if (idx < 0 || (unsigned)idx >= nn) return 0;
  if (c->neuron[idx]) return 1;
  if ((c->cells[idx] % 10) >= 5) return 1;
  return 0;
}

/* Path length from I-face cell to O-face cell on open ports; -1 if none.
 * Cost = hops + digit friction (chaos residual of reverse-Rubik search). */
static int law_pathfind(const lhlam_cube *c, int start, int goal, int *out_cost) {
  unsigned n, nn, i;
  int q[LHLAM_MAX_CELLS];
  int dist[LHLAM_MAX_CELLS];
  int qh = 0, qt = 0;
  int dx[6] = {1, -1, 0, 0, 0, 0};
  int dy[6] = {0, 0, 1, -1, 0, 0};
  int dz[6] = {0, 0, 0, 0, 1, -1};
  if (out_cost) *out_cost = -1;
  if (!c) return -1;
  n = c->n ? c->n : LHLAM_CORE_N;
  if (n > LHLAM_MAX_N) n = LHLAM_MAX_N;
  nn = n * n * n;
  if (start < 0 || goal < 0 || (unsigned)start >= nn || (unsigned)goal >= nn)
    return -1;
  for (i = 0; i < nn; i++) dist[i] = -1;
  dist[start] = 0;
  q[qt++] = start;
  while (qh < qt) {
    int cur = q[qh++];
    int x, y, z, d;
    if (cur == goal) {
      if (out_cost) *out_cost = dist[cur];
      return dist[cur];
    }
    law_xyz((int)n, cur, &x, &y, &z);
    for (d = 0; d < 6; d++) {
      int nx = x + dx[d], ny = y + dy[d], nz = z + dz[d], ni, step;
      if (nx < 0 || ny < 0 || nz < 0
          || nx >= (int)n || ny >= (int)n || nz >= (int)n) continue;
      ni = law_idx((int)n, nx, ny, nz);
      if (!law_open_cell(c, ni) && ni != goal) continue;
      if (dist[ni] >= 0) continue;
      step = 1 + (int)(c->cells[ni] % 3); /* friction */
      dist[ni] = dist[cur] + step;
      if (qt < (int)nn) q[qt++] = ni;
    }
  }
  return -1;
}

/*
 * First Cube's LAW tick — endless game loop step.
 * 1) Pick two algocubes (firing lanes or pick + highest sensor).
 * 2) Map I port (low-z face) and O port (high-z face) on Meta.
 * 3) Pathfind I→O through open matrix cells (best state travel).
 * 4) Race: I→O cost vs plug cube impulse cost (peer trying same port).
 * 5) If I→O wins: combine (feedback + compact) — energy flows, NexusCore +.
 * Meta cube stays CORE_N (never evolve larger for combine).
 */
static void law_tick_locked(const uint8_t *feat, size_t nf) {
  int n, i_cell, o_cell, path_cost, plug_cost, a, b, i, best_a, best_b;
  uint8_t race_in[12];
  size_t rn = 0;
  if (!g_chain_live || !g_coord) return;
  g_law_ticks++;
  n = g_coord->n ? g_coord->n : LHLAM_CORE_N;
  if (n > LHLAM_CORE_N) {
    /* Force small meta — prophecy: volume is not wisdom. */
    n = LHLAM_CORE_N;
  }
  /* I face: z=0 mid; O face: z=n-1 mid */
  i_cell = law_idx(n, n / 2, n / 2, 0);
  o_cell = law_idx(n, n / 2, n / 2, n - 1);
  g_law_last_in_cell = i_cell;
  g_law_last_out_cell = o_cell;

  /* Algocube pair: pick + strongest other fire / sensor */
  best_a = g_chain_pick >= 0 ? g_chain_pick : 0;
  best_b = (best_a + 1) % NG_BC_SENSORS;
  {
    int sc_b = -1;
    for (i = 0; i < NG_BC_SENSORS; i++) {
      int sc = g_lane_fire[i] * 3 + (i < (int)nf ? (int)(feat[i] % 10) : 0);
      if (i == best_a) continue;
      if (sc > sc_b) { sc_b = sc; best_b = i; }
    }
  }
  a = best_a; b = best_b;
  g_law_last_algo_a = a;
  g_law_last_algo_b = b;

  path_cost = -1;
  if (law_pathfind(g_coord, i_cell, o_cell, &path_cost) < 0) {
    g_law_path_fail++;
    g_law_last_path_len = -1;
    g_law_last_winner = -1;
    /* Machine token only — path/energy counts live in law numeric fields. */
    snprintf(g_law_status, sizeof g_law_status, "path_blocked");
    /* Still tick meta with race digits so lattice opens over time */
    race_in[rn++] = (uint8_t)(i_cell % 10);
    race_in[rn++] = (uint8_t)(o_cell % 10);
    race_in[rn++] = (uint8_t)a;
    race_in[rn++] = (uint8_t)b;
    lhlam_cube_tick(g_coord, race_in, rn);
    /* Maybe open neurons without growing n */
    lhlam_cube_maybe_evolve(g_coord);
    if (g_coord->n > LHLAM_CORE_N) {
      /* Hard clamp: export/import not available mid-tick — zero excess edge
       * by re-init seed chain keep (stats only). Prefer compact teach. */
      g_coord->n = LHLAM_CORE_N;
    }
    return;
  }
  g_law_paths++;
  g_law_last_path_len = path_cost;

  /* I→O impulse cost (lower = faster through open ports) */
  g_law_last_i_ms = path_cost;
  g_law_last_o_ms = path_cost + (int)(g_coord->cells[o_cell] % 4);
  /* Plug cube (B) races to claim O: fire strength + digit + conflict penalty */
  plug_cost = 2 + (g_lane_fire[b] ? 0 : 4)
    + (b < (int)nf ? (int)(9 - (feat[b] % 10)) : 5)
    + (g_lane_fire[a] && g_lane_fire[b] ? 2 : 0);
  g_law_last_plug_ms = plug_cost;
  g_law_races++;

  /* RACE: I→O must beat plug to O */
  if (g_law_last_o_ms < plug_cost) {
    /* WIN — energy flows; combine A+B into meta; NexusCore fuel++ */
    g_law_wins++;
    g_law_last_winner = 1;
    g_law_energy += (uint32_t)(1 + (plug_cost - g_law_last_o_ms));
    race_in[rn++] = (uint8_t)a;
    race_in[rn++] = (uint8_t)b;
    race_in[rn++] = 9; /* energy high */
    race_in[rn++] = (uint8_t)(path_cost % 10);
    lhlam_cube_tick(g_coord, race_in, rn);
    lhlam_cube_feedback(g_coord, race_in, rn, 1, 1);
    /* Bridge algocubes: exclusive fire teach toward A, silence B conflict */
    {
      uint8_t sin[8];
      size_t sn = 0;
      sin[sn++] = (uint8_t)(a < (int)nf ? feat[a] % 10 : 0);
      sin[sn++] = (uint8_t)a;
      lhlam_cube_feedback(&g_lane[a], sin, sn, 1, 1);
      sn = 0;
      sin[sn++] = (uint8_t)(b < (int)nf ? feat[b] % 10 : 0);
      sin[sn++] = (uint8_t)b;
      lhlam_cube_feedback(&g_lane[b], sin, sn, 0, g_lane_fire[b] ? 0 : 1);
    }
    g_law_combines++;
    /* Machine token only — A/B/path/plug/energy live in law numeric fields. */
    snprintf(g_law_status, sizeof g_law_status, "win");
  } else {
    /* LOSS — plug claimed O; meta still learns the blocked path */
    g_law_losses++;
    g_law_last_winner = 0;
    race_in[rn++] = (uint8_t)a;
    race_in[rn++] = (uint8_t)b;
    race_in[rn++] = 1;
    race_in[rn++] = (uint8_t)(plug_cost % 10);
    lhlam_cube_tick(g_coord, race_in, rn);
    lhlam_cube_feedback(g_coord, race_in, rn, 0, 0);
    /* Machine token only — A/B/path/plug/energy live in law numeric fields. */
    snprintf(g_law_status, sizeof g_law_status, "loss");
  }
  /* JSON-safe status (no quotes/backslash) — defensive if token ever expands. */
  {
    char *p;
    for (p = g_law_status; *p; p++) {
      if (*p == '"' || *p == '\\' || *p == '\n' || *p == ' ') *p = '_';
    }
  }
  /* Keep meta small after any evolution attempt */
  if (g_coord->n > LHLAM_CORE_N) g_coord->n = LHLAM_CORE_N;
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
      /* Exclusive fire teach: only teacher lane should fire */
      {
        uint8_t sin[8];
        size_t sn = 0;
        sin[sn++] = (uint8_t)(i < (int)nf ? feat[i] % 10 : 0);
        sin[sn++] = (uint8_t)i;
        for (k = 0; k < nf && sn < 6; k++) sin[sn++] = (uint8_t)(feat[k] % 10);
        lhlam_cube_feedback(&g_lane[i], sin, sn, want_fire,
                            (g_lane_fire[i] == want_fire) ? 1 : 0);
      }
    }
    if (ok) g_teaches_ok++; else g_teaches_bad++;
    g_last_teach_want = teach_want;
  }
  /* Endless Crimson Cube law — I/O race + pathfind + combine */
  law_tick_locked(feat, nf);
  lhlam_cube_stats(g_coord, g_last_stats, sizeof g_last_stats);
  return pick;
}

static char *chain_status_json_locked(void) {
  char *jb = NULL;
  char cs[160];
  if (!g_chain_live) chain_init_locked();
  lhlam_cube_stats(g_coord, cs, sizeof cs);
  /* Dual-wire chain_status — machine fields only (no free-text essays). */
  asprintf(&jb,
    "{\"schema\":\"nanobot.braincube.v1\",\"ok\":true,"
    "\"action\":\"chain_status\","
    "\"plugin\":\"cubechain\",\"available\":true,"
    "\"brain\":\"sensor_cubes+meta\",\"sensors\":%d,\"ticks\":%u,"
    "\"pick\":%d,\"pick_name\":\"%s\","
    "\"fire\":[%d,%d,%d,%d,%d,%d,%d,%d],"
    "\"names\":[\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"],"
    "\"agree\":%u,\"conflict\":%u,"
    "\"meta_is\":\"lhlam_cube\",\"stats\":\"%s\","
    NG_BC_DUAL_WIRE "}",
    NG_BC_SENSORS, g_chain_ticks, g_chain_pick,
    (g_chain_pick >= 0 && g_chain_pick < NG_BC_SENSORS) ? g_lane_name[g_chain_pick] : "?",
    g_lane_fire[0], g_lane_fire[1], g_lane_fire[2], g_lane_fire[3],
    g_lane_fire[4], g_lane_fire[5], g_lane_fire[6], g_lane_fire[7],
    g_lane_name[0], g_lane_name[1], g_lane_name[2], g_lane_name[3],
    g_lane_name[4], g_lane_name[5], g_lane_name[6], g_lane_name[7],
    g_chain_agree, g_chain_conflict, cs);
  return jb ? jb
            : strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
                     "\"action\":\"chain_status\",\"error\":\"oom\","
                     "\"python\":0}");
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
    /* Dual-wire live plate — machine fields only (no free-text declaration/principle). */
    asprintf(&jb,
      "{\"schema\":\"nanobot.braincube.v1\",\"ok\":true,"
      "\"action\":\"live\",\"live\":true,\"seq\":%u,\"hz\":4,"
      "\"brain\":\"sensor_cubes+meta\","
      "\"law\":{\"name\":\"%s\",\"ver\":\"%s\",\"ticks\":%u,"
      "\"races\":%u,\"wins\":%u,\"losses\":%u,\"combines\":%u,"
      "\"paths\":%u,\"path_fail\":%u,\"energy\":%u,"
      "\"path_len\":%d,\"winner\":%d,\"algo_a\":%d,\"algo_b\":%d,"
      "\"i_ms\":%d,\"o_ms\":%d,\"plug_ms\":%d,"
      "\"status\":\"%s\"},"
      "\"meta\":{\"id\":\"meta\",\"role\":\"meta\",\"name\":\"MetaCube\","
      "\"pick\":%d,\"pick_name\":\"%s\",\"activity\":%.3f,"
      "\"hits\":%u,\"misses\":%u,\"acc\":%.3f,\"gen\":%u,\"n\":%u,"
      "\"agree\":%u,\"conflict\":%u},"
      "\"sensors\":%s,"
      "\"structure\":{\"mode\":\"iso4\",\"cubes\":%s},"
      "\"world\":{\"state\":%d,\"charge\":%d,\"battery\":%d,\"error\":%d,"
      "\"bump\":[%d,%d,%d]},"
      "\"viz\":{\"layout\":\"radial+iso3d\",\"meta_center\":true,"
      "\"colors\":\"crimson_activity\"},"
      NG_BC_DUAL_WIRE "}",
      (unsigned)g_live_seq,
      LAW_NAME, LAW_VERSION, g_law_ticks,
      g_law_races, g_law_wins, g_law_losses, g_law_combines,
      g_law_paths, g_law_path_fail, g_law_energy,
      g_law_last_path_len, g_law_last_winner, g_law_last_algo_a, g_law_last_algo_b,
      g_law_last_i_ms, g_law_last_o_ms, g_law_last_plug_ms,
      g_law_status[0] ? g_law_status : "init",
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
  return jb ? jb
            : strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
                     "\"action\":\"live\",\"live\":false,\"error\":\"oom\","
                     "\"python\":0}");
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
/* Read first non-empty line from plane file under T2 or ST. */
static int plane_read_line(const char *name, char *buf, size_t cap) {
  static const char *roots[] = {
    "/data/misc/titan2", "/data/local/tmp", "/data/adb/titan2", NULL
  };
  int r;
  for (r = 0; roots[r]; r++) {
    char path[768];
    size_t fl = 0;
    char *body;
    snprintf(path, sizeof path, "%s/%s", roots[r], name);
    body = ng_read_file(path, &fl);
    if (!body || fl == 0) { free(body); continue; }
    size_t i = 0, o = 0;
    while (i < fl && (body[i] == ' ' || body[i] == '\t' || body[i] == '\n')) i++;
    while (i < fl && body[i] != '\n' && body[i] != '\r' && o + 1 < cap)
      buf[o++] = body[i++];
    buf[o] = 0;
    free(body);
    if (o > 0) return 1;
  }
  return 0;
}

/* Map Titan Commander plane tokens → digit 0..9 for CubeChain IN. */
static uint8_t plane_token_digit(const char *name, const char *body) {
  if (!body || !body[0]) return 0;
  if (!strcmp(name, "titan2_pad_mode")) {
    if (!strcmp(body, "off")) return 0;
    if (!strcmp(body, "trackpad") || !strcmp(body, "1")) return 7;
    if (!strcmp(body, "mouse") || !strcmp(body, "2")) return 9;
    return 3;
  }
  if (!strcmp(name, "titan2_usb_hid_session")
      || !strcmp(name, "titan2_usb_hid_on")
      || !strcmp(name, "titan2_usb_hid_grab")
      || !strcmp(name, "titan2_subdisplay_on")
      || !strcmp(name, "titan2_a11y_live")) {
    if (body[0] == '1' || !strcasecmp(body, "on") || !strcasecmp(body, "true"))
      return 9;
    return 0;
  }
  if (!strcmp(name, "titan2_sub_mode")) {
    if (!strcmp(body, "cube")) return 9;
    if (!strcmp(body, "apps")) return 7;
    if (!strcmp(body, "face")) return 5;
    return 0;
  }
  /* generic: first digit or hash */
  if (isdigit((unsigned char)body[0])) return (uint8_t)(body[0] - '0');
  {
    unsigned h = 2166136261u;
    size_t i;
    for (i = 0; body[i] && i < 64; i++) {
      h ^= (unsigned char)body[i];
      h *= 16777619u;
    }
    return (uint8_t)(h % 10);
  }
}

static size_t sample_plane_digits(uint8_t *out, size_t cap) {
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
  /* BlackCube Commander: fixed 8-lane SoT (all cubes share this bus). */
  {
    static const char *lanes[8] = {
      "titan2_pad_mode",           /* 0 pad_off / track / mouse */
      "titan2_usb_hid_session",    /* 1 HID session */
      "titan2_usb_hid_grab",       /* 2 exclusive grab */
      "titan2_subdisplay_on",      /* 3 rear panel power */
      "titan2_sub_mode",           /* 4 face|apps|cube */
      "titan2_a11y_live",          /* 5 a11y belt */
      "titan2_dt2w",               /* 6 DT2W optional */
      "titan2_subdisplay_bri"      /* 7 rear brightness */
    };
    static const char *cmd_names[8] = {
      "pad_mode", "hid_session", "hid_grab", "sub_on",
      "sub_mode", "a11y", "dt2w", "sub_bri"
    };
    char line[96];
    int i;
    for (i = 0; i < 8 && n < cap; i++) {
      if (plane_read_line(lanes[i], line, sizeof line))
        out[n++] = plane_token_digit(lanes[i], line);
      else
        out[n++] = 0;
      /* Retitle sensor lanes for Commander viz when direct_io */
      if (i < NG_BC_SENSORS)
        snprintf(g_lane_name_buf[i], sizeof g_lane_name_buf[i], "%s", cmd_names[i]);
    }
    /* publish law energy into virt for kernel cube (all cubes) */
    {
      char vpath[768], vbody[256];
      int vl;
      snprintf(vpath, sizeof vpath, "/data/local/tmp/cubebrain_viz/virtual.tsv");
      vl = snprintf(vbody, sizeof vbody,
        "# Crimson law (nanobot)\n"
        "virt_law_energy\t%u\n"
        "virt_law_wins\t%u\n"
        "virt_law_losses\t%u\n"
        "virt_law_combines\t%u\n"
        "virt_law_winner\t%d\n"
        "virt_nanobot_up\t9\n",
        g_law_energy, g_law_wins, g_law_losses, g_law_combines, g_law_last_winner);
      if (vl > 0) {
        mkdir("/data/local/tmp/cubebrain_viz", 0777);
        ng_write_file(vpath, vbody, (size_t)vl);
      }
    }
    g_plane_samples++;
    return n;
  }
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
  g_useful_teaches = g_skipped_teaches = 0;
  g_prev_feat_ok = 0;
  g_prev_state = -1;
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
      int st_now, ch_now, useful = 0;
      if (!g_chain_live) chain_init_locked();
      supervise_poll_locked();
      nf = sample_clanker_sensors(feat, sizeof feat);
      if (nf == 0) nf = sample_plane_digits(feat, sizeof feat);
      if (nf == 0) { feat[0] = 1; feat[1] = 0; nf = 2; }
      /* Pad to 8 sensor lanes so self-teacher always has a world. */
      for (i = (int)nf; i < NG_BC_SENSORS; i++)
        feat[i] = (uint8_t)(g_sensor_val[i] % 10);
      if (nf < (size_t)NG_BC_SENSORS) nf = (size_t)NG_BC_SENSORS;
      st_now = g_world_state;
      ch_now = g_world_charge;
      /* Agent supervision wins while TTL active; else self-teacher (may skip). */
      if (g_agent_want >= 0 && g_agent_want < NG_BC_SENSORS &&
          (g_agent_until == 0 || time(NULL) <= g_agent_until)) {
        teach = g_agent_want;
        snprintf(g_last_teach_src, sizeof g_last_teach_src, "agent");
        g_agent_teaches++;
        useful = 1;
      } else if (g_self_teach) {
        teach = self_teacher_want(feat, nf);
        if (teach >= 0) {
          snprintf(g_last_teach_src, sizeof g_last_teach_src, "self");
          g_self_teaches++;
          useful = 1;
        } else {
          snprintf(g_last_teach_src, sizeof g_last_teach_src, "skip");
          g_skipped_teaches++;
        }
      } else {
        teach = -1;
        snprintf(g_last_teach_src, sizeof g_last_teach_src, "none");
      }
      if (g_chain_live && nf > 0) {
        int d;
        /* Always tick for online prediction/viz; teach only if useful signal. */
        d = chain_tick_locked(feat, nf, teach);
        g_ticks++;
        g_last_decision = d;
        g_decides++;
        if (useful) g_useful_teaches++;
        /* remember feat/state for next info-gate */
        if (nf >= 8) {
          memcpy(g_prev_feat, feat, 8);
          g_prev_feat_ok = 1;
        }
        g_prev_state = st_now % 10;
        (void)ch_now;
        write_live_snap_locked();
        /* Persist more often when useful learning, less when skipping */
        if (useful || (g_chain_ticks % 16) == 0)
          chain_persist_locked();
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
  jb = strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
              "\"action\":\"live\",\"live\":false,\"available\":false,"
              "\"error\":\"braincube_unavailable\",\"python\":0}");
#endif
  return jb ? jb
            : strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
                     "\"action\":\"live\",\"error\":\"oom\",\"python\":0}");
}

char *ng_bc_status_json(void) {
  char *snap = NULL;
  char *jb = NULL;
  /* Dual-wire status — machine fields only (no free-text coaching notes). */
#if NANOBOT_HAS_BRAINCUBE
  snap = read_live_snap_if_fresh(8);
  if (snap) {
    asprintf(&jb,
      "{\"schema\":\"nanobot.braincube.v1\",\"ok\":true,\"action\":\"status\","
      "\"plugin\":\"braincube\",\"available\":true,"
      "\"brain\":\"sensor_cubes+meta\",\"sensors\":8,"
      "\"continuous\":true,\"learning\":true,\"snap\":true,"
      "\"product_wire\":\"smx2\",\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,\"llm_is_commander\":false,\"python\":0}");
    free(snap);
    if (jb) return jb;
  }
#endif
  asprintf(&jb,
    "{\"schema\":\"nanobot.braincube.v1\",\"ok\":true,\"action\":\"status\","
    "\"plugin\":\"braincube\",\"available\":true,"
    "\"brain\":\"sensor_cubes+meta\",\"sensors\":8,"
    "\"continuous\":%s,\"self_teach\":%s,\"snap\":false,"
    "\"product_wire\":\"smx2\",\"peer_http\":\"lab_ops_only\","
    "\"peer_http_is_product_bus\":false,\"share\":\"state_matrix_only\","
    "\"hold_flash\":1,\"llm_is_commander\":false,\"python\":0}",
    g_continuous ? "true" : "false",
    g_self_teach ? "true" : "false");
  return jb ? jb
            : strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
                     "\"action\":\"status\",\"error\":\"oom\",\"python\":0}");
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
  if (!strcmp(action, "law") || !strcmp(action, "crimson") || !strcmp(action, "first_cube")) {
    /* First Cube's LAW snapshot (I/O race + pathfind + NexusCore energy). */
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
    /* Dual-wire learn_status fail — machine error token only. */
    return strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
                  "\"action\":\"learn_status\",\"learning\":false,"
                  "\"error\":\"no_live_snap\",\"python\":0}");
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
      /* Dual-wire supervise fail — machine error token only. */
      return strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
                    "\"action\":\"supervise\",\"error\":\"bad_want\","
                    "\"python\":0}");
    }
    supervise_write(want, ttl, note);
    free(note);
    {
      char *lane_esc = ng_json_escape(g_lane_name[want] ? g_lane_name[want] : "");
      char *jb = NULL;
      /* Dual-wire supervise ack — no free-text note essay. */
      asprintf(&jb,
        "{\"schema\":\"nanobot.braincube.v1\",\"ok\":true,"
        "\"action\":\"supervise\",\"supervised\":true,"
        "\"want\":%d,\"want_name\":\"%s\",\"ttl_sec\":%d,"
        NG_BC_DUAL_WIRE "}",
        want, lane_esc ? lane_esc : "", ttl);
      free(lane_esc);
      free(action);
      return jb ? jb
                : strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
                         "\"action\":\"supervise\",\"error\":\"oom\","
                         "\"python\":0}");
    }
#else
    free(action);
    return strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
                  "\"action\":\"supervise\",\"error\":\"braincube_unavailable\","
                  "\"python\":0}");
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
        char *lane_esc = ng_json_escape(g_lane_name[want] ? g_lane_name[want] : "");
        char *jb = NULL;
        /* Dual-wire teach ack — machine fields only (no free-text essay). */
        asprintf(&jb,
          "{\"schema\":\"nanobot.braincube.v1\",\"ok\":true,"
          "\"action\":\"teach\",\"taught\":true,\"mode\":\"supervise\","
          "\"want\":%d,\"want_name\":\"%s\",\"ttl_sec\":%d,"
          NG_BC_DUAL_WIRE "}",
          want, lane_esc ? lane_esc : "", ttl);
        free(lane_esc);
        return jb ? jb
                  : strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
                           "\"action\":\"teach\",\"error\":\"oom\",\"python\":0}");
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
      /* Dual-wire reset ack — no free-text note essay. */
      asprintf(&jb,
               "{\"schema\":\"nanobot.braincube.v1\",\"ok\":true,"
               "\"action\":\"reset\",\"reset\":true,"
               "\"product_wire\":\"smx2\",\"peer_http\":\"lab_ops_only\","
               "\"peer_http_is_product_bus\":false,"
               "\"share\":\"state_matrix_only\",\"hold_flash\":1,"
               "\"llm_is_commander\":false,\"python\":0}");
      return jb ? jb
                : strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
                         "\"action\":\"reset\",\"error\":\"oom\",\"python\":0}");
    }
#else
    free(action);
    return strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
                  "\"action\":\"reset\",\"error\":\"braincube_unavailable\","
                  "\"python\":0}");
#endif
  }

  if (!strcmp(action, "export") || !strcmp(action, "chain_export")) {
#if NANOBOT_HAS_BRAINCUBE
    {
      char path[700];
      size_t blen = 0;
      char *raw;
      char *b64;
      char *sid_esc = NULL;
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
        /* Dual-wire export fail — machine error token only. */
        return strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
                      "\"action\":\"export\",\"error\":\"no_chain_state\","
                      "\"python\":0}");
      }
      b64 = b64_encode((const uint8_t *)raw, blen);
      free(raw);
      if (!b64) {
        free(action);
        return strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
                      "\"action\":\"export\",\"error\":\"b64_oom\","
                      "\"python\":0}");
      }
      sid_esc = ng_json_escape(g_session_id[0] ? g_session_id : "");
      /* Dual-wire export ack — no free-text note essay. */
      asprintf(&jb,
        "{\"schema\":\"nanobot.braincube.v1\",\"ok\":true,"
        "\"action\":\"export\",\"format\":\"chain_state_b64\","
        "\"bytes\":%zu,\"resets\":%u,\"session_id\":\"%s\",\"data\":\"%s\","
        NG_BC_DUAL_WIRE "}",
        blen, g_resets, sid_esc ? sid_esc : "", b64);
      free(b64);
      free(sid_esc);
      free(action);
      return jb ? jb
                : strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
                         "\"action\":\"export\",\"error\":\"oom\",\"python\":0}");
    }
#else
    free(action);
    return strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
                  "\"action\":\"export\",\"error\":\"braincube_unavailable\","
                  "\"python\":0}");
#endif
  }

  /*
   * SMX1 — State Matrix eXchange binary (cubes talk binary when sharing matrices).
   * Wire: magic SMX1 | ver | n | flags | pad | seq u32 | pick i8+pad3 |
   *        cells[n³] | [neuron n³] | [law energy/wins/losses/combines u32×4]
   * flags: bit0 neuron, bit1 law trailer
   * action=matrix_bin | exchange_bin | smx1  (export)
   * action=matrix_bin_import | smx1_import  data=base64
   */
  if (!strcmp(action, "matrix_bin") || !strcmp(action, "exchange_bin")
      || !strcmp(action, "smx1")) {
#if NANOBOT_HAS_BRAINCUBE
    {
      char *which = ng_json_get_string(json_body, "cube"); /* meta|lane name|index */
      const lhlam_cube *src = NULL;
      uint8_t *blob = NULL;
      size_t cap, o = 0;
      unsigned n, nn, i;
      uint32_t seq32;
      int8_t pick8;
      uint8_t flags = 0;
      char *b64, *jb = NULL;
      pthread_mutex_lock(&g_mu);
      if (!g_chain_live) chain_init_locked();
      src = g_coord;
      if (which && which[0]) {
        int li = lane_from_token(which);
        if (li >= 0 && li < NG_BC_SENSORS && g_lane)
          src = &g_lane[li];
        else if (!strcmp(which, "meta") || !strcmp(which, "coord"))
          src = g_coord;
      }
      free(which);
      if (!src || !g_chain_live) {
        pthread_mutex_unlock(&g_mu);
        free(action);
        return strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
                      "\"action\":\"matrix_bin\",\"error\":\"no_cube_lattice\","
                      "\"python\":0}");
      }
      n = src->n ? src->n : LHLAM_CORE_N;
      if (n > LHLAM_MAX_N) n = LHLAM_MAX_N;
      nn = n * n * n;
      flags = 0x01; /* always include neuron mask for exchange */
      if (src == g_coord) flags |= 0x02; /* law trailer on meta */
      cap = 4 + 4 + 4 + 4 + nn + nn + 16;
      blob = (uint8_t *)malloc(cap);
      if (!blob) {
        pthread_mutex_unlock(&g_mu);
        free(action);
        return strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
                      "\"action\":\"matrix_bin\",\"error\":\"oom\","
                      "\"python\":0}");
      }
      blob[o++] = 'S'; blob[o++] = 'M'; blob[o++] = 'X'; blob[o++] = '1';
      blob[o++] = 1; /* ver */
      blob[o++] = (uint8_t)n;
      blob[o++] = flags;
      blob[o++] = 0;
      seq32 = (uint32_t)g_live_seq;
      memcpy(blob + o, &seq32, 4); o += 4;
      pick8 = (int8_t)g_chain_pick;
      blob[o++] = (uint8_t)pick8;
      blob[o++] = 0; blob[o++] = 0; blob[o++] = 0;
      for (i = 0; i < nn; i++) blob[o++] = (uint8_t)(src->cells[i] % 10);
      for (i = 0; i < nn; i++) blob[o++] = src->neuron[i] ? 1 : 0;
      if (flags & 0x02) {
        uint32_t le = g_law_energy, lw = g_law_wins, ll = g_law_losses, lc = g_law_combines;
        memcpy(blob + o, &le, 4); o += 4;
        memcpy(blob + o, &lw, 4); o += 4;
        memcpy(blob + o, &ll, 4); o += 4;
        memcpy(blob + o, &lc, 4); o += 4;
      }
      pthread_mutex_unlock(&g_mu);
      b64 = b64_encode(blob, o);
      free(blob);
      if (!b64) {
        free(action);
        return strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
                      "\"action\":\"matrix_bin\",\"error\":\"b64_oom\","
                      "\"python\":0}");
      }
      /* Dual-wire SMX1 export — no free-text note essay. */
      asprintf(&jb,
        "{\"schema\":\"nanobot.braincube.v1\",\"ok\":true,"
        "\"action\":\"matrix_bin\",\"format\":\"SMX1\",\"wire\":\"binary\","
        "\"bytes\":%zu,\"n\":%u,\"flags\":%u,\"data\":\"%s\","
        NG_BC_DUAL_WIRE "}",
        o, n, (unsigned)flags, b64);
      free(b64);
      free(action);
      return jb ? jb
                : strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
                         "\"action\":\"matrix_bin\",\"error\":\"oom\","
                         "\"python\":0}");
    }
#else
    free(action);
    return strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
                  "\"action\":\"matrix_bin\","
                  "\"error\":\"braincube_unavailable\",\"python\":0}");
#endif
  }

  if (!strcmp(action, "matrix_bin_import") || !strcmp(action, "smx1_import")
      || !strcmp(action, "exchange_bin_import")) {
#if NANOBOT_HAS_BRAINCUBE
    {
      char *data = ng_json_get_string(json_body, "data");
      char *which = ng_json_get_string(json_body, "cube");
      size_t raw_n = 0;
      uint8_t *raw;
      unsigned n, nn, i, o = 0;
      uint8_t flags, ver;
      lhlam_cube *dst = NULL;
      if (!data || !data[0]) {
        free(data); free(which); free(action);
        return strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
                      "\"action\":\"matrix_bin_import\","
                      "\"error\":\"need_data\",\"python\":0}");
      }
      raw = b64_decode(data, &raw_n);
      free(data);
      if (!raw || raw_n < 16 || raw[0] != 'S' || raw[1] != 'M' || raw[2] != 'X' || raw[3] != '1') {
        free(raw); free(which); free(action);
        return strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
                      "\"action\":\"matrix_bin_import\","
                      "\"error\":\"not_smx1\",\"python\":0}");
      }
      ver = raw[4]; n = raw[5]; flags = raw[6];
      o = 12; /* skip magic+hdr+seq start: 4+4 = 8, then seq 4 → 12, pick 4 → 16 */
      o = 16;
      if (ver != 1 || n < 2 || n > LHLAM_MAX_N) {
        free(raw); free(which); free(action);
        return strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
                      "\"action\":\"matrix_bin_import\","
                      "\"error\":\"bad_smx1_header\",\"python\":0}");
      }
      nn = n * n * n;
      if (raw_n < 16 + nn) {
        free(raw); free(which); free(action);
        return strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
                      "\"action\":\"matrix_bin_import\","
                      "\"error\":\"smx1_truncated\",\"python\":0}");
      }
      pthread_mutex_lock(&g_mu);
      if (!g_chain_live) chain_init_locked();
      dst = g_coord;
      if (which && which[0]) {
        int li = lane_from_token(which);
        if (li >= 0 && li < NG_BC_SENSORS && g_lane) dst = &g_lane[li];
      }
      free(which);
      if (!dst) {
        pthread_mutex_unlock(&g_mu);
        free(raw); free(action);
        return strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
                      "\"action\":\"matrix_bin_import\","
                      "\"error\":\"no_dest_cube\",\"python\":0}");
      }
      dst->n = (uint8_t)n;
      for (i = 0; i < nn; i++) dst->cells[i] = raw[o + i] % 10;
      o += nn;
      if ((flags & 0x01) && raw_n >= o + nn) {
        for (i = 0; i < nn; i++) dst->neuron[i] = raw[o + i] ? 1 : 0;
        o += nn;
      }
      if ((flags & 0x02) && dst == g_coord && raw_n >= o + 16) {
        memcpy(&g_law_energy, raw + o, 4); o += 4;
        memcpy(&g_law_wins, raw + o, 4); o += 4;
        memcpy(&g_law_losses, raw + o, 4); o += 4;
        memcpy(&g_law_combines, raw + o, 4);
      }
      chain_persist_locked();
      write_live_snap_locked();
      pthread_mutex_unlock(&g_mu);
      free(raw);
      free(action);
      /* Dual-wire SMX1 import ack — no free-text note essay. */
      return strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":true,"
                    "\"action\":\"matrix_bin_import\",\"imported\":true,"
                    "\"format\":\"SMX1\"," NG_BC_DUAL_WIRE "}");
    }
#else
    free(action);
    return strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
                  "\"action\":\"matrix_bin_import\","
                  "\"error\":\"braincube_unavailable\",\"python\":0}");
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
        return strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
                      "\"action\":\"import\",\"error\":\"need_data\","
                      "\"python\":0}");
      }
      raw = b64_decode(data, &raw_n);
      free(data);
      if (!raw || raw_n < 16) {
        free(raw); free(action);
        return strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
                      "\"action\":\"import\",\"error\":\"bad_base64\","
                      "\"python\":0}");
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
      /* Dual-wire chain import ack — no free-text note essay. */
      return strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":true,"
                    "\"action\":\"import\",\"imported\":true,"
                    NG_BC_DUAL_WIRE "}");
    }
#else
    free(action);
    return strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
                  "\"action\":\"import\","
                  "\"error\":\"braincube_unavailable\",\"python\":0}");
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
        char *sid_esc = ng_json_escape(g_session_id[0] ? g_session_id : "");
        char *jb = NULL;
        /* Dual-wire session_start — machine flags only. */
        asprintf(&jb,
          "{\"schema\":\"nanobot.braincube.v1\",\"ok\":true,"
          "\"action\":\"session_start\",\"session_start\":true,"
          "\"session_id\":\"%s\",\"continuous\":true,\"self_teach\":true,"
          "\"agent_service\":true," NG_BC_DUAL_WIRE "}",
          sid_esc ? sid_esc : "");
        free(sid_esc);
        return jb ? jb
                  : strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
                           "\"action\":\"session_start\",\"error\":\"oom\","
                           "\"python\":0}");
      }
    }
#else
    free(action);
    return strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
                  "\"action\":\"session_start\","
                  "\"error\":\"braincube_unavailable\",\"python\":0}");
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
    /* Dual-wire session_stop — machine flags only. */
    return strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":true,"
                  "\"action\":\"session_stop\",\"session_stop\":true,"
                  "\"agent_service\":false," NG_BC_DUAL_WIRE "}");
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
      /* Dual-wire agent_service ack — machine flag only (no free-text). */
      asprintf(&jb,
               "{\"schema\":\"nanobot.braincube.v1\",\"ok\":true,"
               "\"action\":\"agent_service\",\"agent_service\":%s,"
               NG_BC_DUAL_WIRE "}",
               on ? "true" : "false");
      return jb ? jb
                : strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
                         "\"action\":\"agent_service\",\"error\":\"oom\","
                         "\"python\":0}");
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
      char *sid_esc = ng_json_escape(g_session_id[0] ? g_session_id : "");
      char *jb = NULL;
      /* Dual-wire control plate — machine fields only (no free-text note essay). */
      asprintf(&jb,
        "{\"schema\":\"nanobot.braincube.v1\",\"ok\":true,"
        "\"action\":\"control\",\"learning\":false,\"snap\":false,"
        "\"continuous\":%s,\"self_teach\":%s,"
        "\"agent_service\":%s,\"session_id\":\"%s\",\"resets\":%u,"
        NG_BC_DUAL_WIRE "}",
        g_continuous ? "true" : "false", g_self_teach ? "true" : "false",
        g_agent_service ? "true" : "false",
        sid_esc ? sid_esc : "", g_resets);
      free(sid_esc);
      return jb ? jb
                : strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
                         "\"action\":\"control\",\"error\":\"oom\",\"python\":0}");
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
      /* Dual-wire field_trials ack — machine flag only (no free-text note essay). */
      asprintf(&jb,
        "{\"schema\":\"nanobot.braincube.v1\",\"ok\":true,"
        "\"action\":\"field_trials\",\"field_trials\":%s,"
        NG_BC_DUAL_WIRE "}",
        on ? "true" : "false");
      return jb ? jb
                : strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
                         "\"action\":\"field_trials\",\"error\":\"oom\","
                         "\"python\":0}");
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
      int field_on = 0;
      {
        char fpath[700];
        snprintf(fpath, sizeof fpath, "%s/braincube/field_trials.flag",
                 ng_workdir());
        field_on = (access(fpath, F_OK) == 0);
      }
      /* Dual-wire explore ack — machine flags only (no free-text note essay). */
      asprintf(&jb,
        "{\"schema\":\"nanobot.braincube.v1\",\"ok\":true,"
        "\"action\":\"explore\",\"explore\":%s,\"field_trials\":%s,"
        "\"continuous\":%s,\"agent_service\":%s,"
        NG_BC_DUAL_WIRE "}",
        on ? "true" : "false", field_on ? "true" : "false",
        g_continuous ? "true" : "false",
        g_agent_service ? "true" : "false");
      return jb ? jb
                : strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
                         "\"action\":\"explore\",\"error\":\"oom\",\"python\":0}");
    }
  }

  if (!strcmp(action, "train_status") || !strcmp(action, "train_report") ||
      !strcmp(action, "hows_training")) {
    free(action);
    /* Shared dual-wire plate builder (same as ng_bc_train_status_report). */
    return ng_bc_train_status_report();
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
    /* Dual-wire trials plate — machine fields only (no how_to_read essay). */
    asprintf(&jb,
      "{\"schema\":\"nanobot.braincube.v1\",\"ok\":true,"
      "\"action\":\"trials\",\"field_trials\":%s,\"count\":%d,"
      "\"latest\":%s,"
      "\"field_status\":%s,"
      "\"trials\":%s,"
      NG_BC_DUAL_WIRE "}",
      field_on ? "true" : "false", nlines,
      (lat && llen > 2) ? lat : "null",
      (st && slen > 2) ? st : "null",
      trials_arr);
    free(body); free(st); free(lat);
    free(action);
    return jb ? jb
              : strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":true,"
                       "\"action\":\"trials\",\"trials\":[],\"count\":0,"
                       "\"python\":0}");
  }

  if (!strcmp(action, "selftest")) {
#if NANOBOT_HAS_BRAINCUBE
    int rc = lhlam_selftest();
    char *jb = NULL;
    /* Dual-wire selftest — machine rc only. */
    asprintf(&jb,
      "{\"schema\":\"nanobot.braincube.v1\",\"ok\":%s,"
      "\"action\":\"selftest\",\"selftest_rc\":%d,"
      "\"learning_evidence\":%s," NG_BC_DUAL_WIRE "}",
      rc == 0 ? "true" : "false", rc, rc == 0 ? "true" : "false");
    free(action);
    return jb ? jb
              : strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
                       "\"action\":\"selftest\",\"error\":\"oom\",\"python\":0}");
#else
    free(action);
    return strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
                  "\"action\":\"selftest\","
                  "\"error\":\"braincube_unavailable\",\"python\":0}");
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
    return strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
                  "\"action\":\"chain_status\","
                  "\"error\":\"braincube_unavailable\",\"python\":0}");
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
    return strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
                  "\"action\":\"chain_tick\","
                  "\"error\":\"braincube_unavailable\",\"python\":0}");
#endif
  }

  free(action);
  /* Dual-wire unknown action — machine error token + action catalog. */
  return strdup("{\"schema\":\"nanobot.braincube.v1\",\"ok\":false,"
    "\"action\":\"unknown\",\"error\":\"unknown_action\","
    "\"actions\":[\"status\",\"live\",\"sense\",\"learn_status\",\"training\","
    "\"train_status\",\"train_report\",\"hows_training\","
    "\"continuous\",\"self_teach\",\"supervise\",\"agent_teach\",\"teach\","
    "\"session_start\",\"session_stop\",\"train_start\",\"train_stop\","
    "\"explore\",\"field_trials\",\"trials\",\"results\","
    "\"export\",\"import\",\"matrix_bin\",\"exchange_bin\",\"smx1\","
    "\"matrix_bin_import\",\"smx1_import\","
    "\"reset\",\"brain_reset\",\"agent_service\",\"control\","
    "\"auto_adapt\",\"enable\",\"disable\",\"direct_io\",\"dry_run\",\"tick\","
    "\"decide\",\"sample\",\"feedback\",\"selftest\",\"chain_status\","
    "\"chain_tick\"],\"python\":0}");
}
