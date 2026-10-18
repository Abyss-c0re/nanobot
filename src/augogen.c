#include "augogen.h"
#include "motive.h"
#include "util.h"
#include "memory.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#if NANOBOT_HAS_BRAINCUBE
#include <braincube/braincube.h>
#endif

#define SUG_CAP 240
#define ID_CAP 40
#define ROLE_CAP 16

typedef struct {
  char id[ID_CAP];
  char suggestion[SUG_CAP];
  char status[24]; /* empty|pending|approved|rejected|applied */
  unsigned long generation;
  int guide_vote;   /* -1 unknown, 0 no, 1 yes */
  int oversee_vote;
  int mesh_vote;
  int confirmed;
  int pair_ok;
  int executed;
  char job_id[40];
  int score_kept;
  int motive_rank;
  int motive_lex;
  int motive_ham;
  char plate_name[16];
} ng_augogen;

#if NANOBOT_HAS_BRAINCUBE
static void load_cube(lhlam_cube *c, const char *who);
static void save_cube(const lhlam_cube *c, const char *who);
#endif

static int truthy_off(const char *s) {
  if (!s || !s[0]) return 0;
  return s[0] == '0' || s[0] == 'n' || s[0] == 'N' || s[0] == 'f' ||
         s[0] == 'F' || !strcmp(s, "off") || !strcmp(s, "OFF");
}

/* Default ON (full auto). Vote-only: NANOBOT_AUGOGEN_AUTO=0 or AUGOGEN_AUTO=0. */
static int augogen_auto_enabled(void) {
  const char *e = getenv("NANOBOT_AUGOGEN_AUTO");
  if (e && e[0]) return !truthy_off(e);
  {
    char *s = ng_settings_get("AUGOGEN_AUTO");
    int on;
    if (!s) return 1;
    on = !truthy_off(s);
    free(s);
    return on;
  }
}

static int suggestion_forbidden(const char *s) {
  char low[SUG_CAP];
  size_t i;
  if (!s || !s[0]) return 0;
  for (i = 0; s[i] && i + 1 < sizeof low; i++) {
    char c = s[i];
    low[i] = (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
  }
  low[i] = 0;
  if (strstr(low, "hold_flash=0") || strstr(low, "hold_flash 0")) return 1;
  if (strstr(low, "wipe")) return 1;
  if (strstr(low, "super flash")) return 1;
  if (strstr(low, "stop the cycle") || strstr(low, "stop the_cycle")) return 1;
  if (strstr(low, "flash") && !strstr(low, "hold_flash")) return 1;
  return 0;
}

static int pair_confirm_suggestion(ng_augogen *p) {
#if NANOBOT_HAS_BRAINCUBE
  lhlam_cube guide, oversee;
  int pair;
  load_cube(&guide, "guide");
  load_cube(&oversee, "oversee");
  pair = lhlam_cube_pair_confirm_cstr(&guide, &oversee, p->suggestion);
  save_cube(&guide, "guide");
  save_cube(&oversee, "oversee");
  if (pair) {
    p->guide_vote = 1;
    p->oversee_vote = 1;
    p->pair_ok = 1;
    return 1;
  }
#else
  (void)p;
#endif
  return 0;
}

/* Done auto jobs past this cap drop. A queued or running job stays. */
enum { AUTO_JOB_KEEP = 48, AUTO_JOB_SCAN = 256 };

static int auto_job_stem(const char *stem, size_t n) {
  size_t i;
  int digit = 0;
  if (n < 8 || n >= 40) return 0;
  if (strncmp(stem, "agrun-", 6) != 0) return 0;
  for (i = 6; i < n; i++) {
    unsigned char c = (unsigned char)stem[i];
    if (c == '-') continue;
    if (!isdigit(c)) return 0;
    digit = 1;
  }
  return digit;
}

static void reap_done_auto_jobs(const char *jdir) {
  DIR *d;
  struct dirent *de;
  char ids[AUTO_JOB_SCAN][40];
  int n = 0;
  int i;
  if (!jdir || !jdir[0]) return;
  d = opendir(jdir);
  if (!d) return;
  while ((de = readdir(d)) != NULL && n < AUTO_JOB_SCAN) {
    const char *name = de->d_name;
    size_t len = strlen(name);
    size_t idlen;
    char mpath[700];
    char *meta;
    char *st;
    int live;
    if (len < 6 || strcmp(name + len - 5, ".json") != 0) continue;
    idlen = len - 5;
    if (!auto_job_stem(name, idlen)) continue;
    if (snprintf(mpath, sizeof mpath, "%s/%s", jdir, name) >= (int)sizeof mpath)
      continue;
    meta = ng_read_file(mpath, NULL);
    if (!meta) continue;
    st = ng_json_get_string(meta, "status");
    live = (st && (!strcmp(st, "queued") || !strcmp(st, "running")));
    free(st);
    free(meta);
    if (live) continue;
    memset(ids[n], 0, sizeof ids[n]);
    memcpy(ids[n], name, idlen);
    n++;
  }
  closedir(d);
  if (n <= AUTO_JOB_KEEP) return;
  for (i = 1; i < n; i++) {
    char tmp[40];
    int j = i;
    memcpy(tmp, ids[i], sizeof tmp);
    while (j > 0 && strcmp(ids[j - 1], tmp) > 0) {
      memcpy(ids[j], ids[j - 1], sizeof ids[j]);
      j--;
    }
    memcpy(ids[j], tmp, sizeof tmp);
  }
  {
    int drop = n - AUTO_JOB_KEEP;
    for (i = 0; i < drop; i++) {
      char p[700];
      if (snprintf(p, sizeof p, "%s/%s.json", jdir, ids[i]) < (int)sizeof p)
        unlink(p);
      if (snprintf(p, sizeof p, "%s/%s.in", jdir, ids[i]) < (int)sizeof p)
        unlink(p);
      if (snprintf(p, sizeof p, "%s/%s.out", jdir, ids[i]) < (int)sizeof p)
        unlink(p);
    }
  }
}

/* Fork a prompt job (same shape as POST /peer/v1/jobs). Never blocks HTTP.
 * A second prompt in the same second keeps its own file. The first stays.
 * A done auto job past the keep cap drops. A queued job stays.
 * A failed fork marks the reserved file error. It does not stay queued. */
static int enqueue_prompt(ng_agent_cfg *agent, const char *prompt, char *id_out,
                          size_t id_n) {
  char jdir[640], mpath[700], rpath[700], pp[700], meta[768];
  char id[40];
  pid_t w;
  int mn = 0;
  int attempt;
  int reserved = 0;
  long sec;
  int pid;
  if (!prompt || !prompt[0]) return 0;
  snprintf(jdir, sizeof jdir, "%s/jobs", ng_workdir());
  mkdir(jdir, 0755);
  reap_done_auto_jobs(jdir);
  sec = (long)time(NULL);
  pid = (int)(getpid() & 0xFFFF);
  for (attempt = 0; attempt < 16; attempt++) {
    int kn, fd, spins;
    if (attempt == 0)
      kn = snprintf(id, sizeof id, "agrun-%ld-%d", sec, pid);
    else
      kn = snprintf(id, sizeof id, "agrun-%ld-%d-%d", sec, pid, attempt);
    if (kn < 0 || (size_t)kn >= sizeof id) return 0;
    if (snprintf(mpath, sizeof mpath, "%s/%s.json", jdir, id) >= (int)sizeof mpath)
      return 0;
    if (snprintf(pp, sizeof pp, "%s/%s.in", jdir, id) >= (int)sizeof pp) return 0;
    mn = snprintf(meta, sizeof meta,
                  "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,"
                  "\"action\":\"job\",\"id\":\"%s\",\"status\":\"queued\","
                  "\"kind\":\"prompt\",\"via\":\"augogen_auto\"}\n",
                  id);
    if (mn < 0 || (size_t)mn >= sizeof meta) return 0;
    fd = -1;
    for (spins = 0; spins < 4; spins++) {
      fd = open(mpath, O_CREAT | O_EXCL | O_WRONLY, 0666);
      if (fd >= 0 || errno != EINTR) break;
    }
    if (fd < 0) {
      if (errno == EEXIST) continue;
      return 0;
    }
    {
      size_t off = 0;
      while (off < (size_t)mn) {
        ssize_t wrote = write(fd, meta + off, (size_t)mn - off);
        if (wrote < 0) {
          if (errno == EINTR) continue;
          close(fd);
          unlink(mpath);
          return 0;
        }
        off += (size_t)wrote;
      }
    }
    if (close(fd) != 0) {
      unlink(mpath);
      return 0;
    }
    reserved = 1;
    break;
  }
  if (!reserved) return 0;
  if (ng_write_file(pp, prompt, strlen(prompt)) != 0) {
    unlink(mpath);
    unlink(pp);
    return 0;
  }
  snprintf(rpath, sizeof rpath, "%s/%s.out", jdir, id);
  w = fork();
  if (w == 0) {
    char *reply;
    char *esc;
    char *jb = NULL;
    char runm[512];
    int rn = snprintf(runm, sizeof runm,
                      "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,"
                      "\"action\":\"job\",\"id\":\"%s\",\"status\":\"running\","
                      "\"kind\":\"prompt\",\"via\":\"augogen_auto\"}\n",
                      id);
    ng_write_file(mpath, runm, (size_t)rn);
    reply = agent ? ng_agent_run(agent, prompt) : strdup("");
    esc = ng_json_escape(reply ? reply : "");
    asprintf(&jb,
             "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,"
             "\"action\":\"job\",\"id\":\"%s\",\"status\":\"done\","
             "\"kind\":\"prompt\",\"via\":\"augogen_auto\",\"reply\":\"%s\"}\n",
             id, esc ? esc : "");
    if (jb) {
      ng_write_file(mpath, jb, strlen(jb));
      free(jb);
    }
    free(esc);
    free(reply);
    _exit(0);
  }
  if (w < 0) {
    char errm[512];
    int en = snprintf(errm, sizeof errm,
                      "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":false,"
                      "\"action\":\"job\",\"id\":\"%s\",\"status\":\"error\","
                      "\"kind\":\"prompt\",\"via\":\"augogen_auto\","
                      "\"error\":\"fork_failed\"}\n",
                      id);
    if (en > 0 && (size_t)en < sizeof errm)
      ng_write_file(mpath, errm, (size_t)en);
    if (id_out && id_n) snprintf(id_out, id_n, "%s", id);
    return 0;
  }
  if (id_out && id_n) snprintf(id_out, id_n, "%s", id);
  (void)rpath;
  return 1;
}

static void try_auto_run(ng_augogen *p, ng_agent_cfg *agent) {
  if (!p->confirmed || !p->suggestion[0]) return;
  if (suggestion_forbidden(p->suggestion) || ng_motive_blocks(p->suggestion)) {
    snprintf(p->status, sizeof p->status, "rejected");
    p->confirmed = 0;
    p->executed = 0;
    return;
  }
  snprintf(p->status, sizeof p->status, "applied");
  if (enqueue_prompt(agent, p->suggestion, p->job_id, sizeof p->job_id))
    p->executed = 1;
  else
    p->executed = 0;
}

static void augogen_dir(char *buf, size_t n) {
  snprintf(buf, n, "%s/augogen", ng_workdir());
}

/* A symlink named augogen is not the plate directory. */
static int augogen_dir_is_link(void) {
  char d[640];
  struct stat st;
  augogen_dir(d, sizeof d);
  if (lstat(d, &st) != 0) return 0;
  return S_ISLNK(st.st_mode) ? 1 : 0;
}

/* replace=0 leaves a symlink unread. replace=1 unlinks that link and
 * makes a directory. The target stays. A final symlink is ENOTDIR.
 * A regular file is not the plate directory. replace=1 unlinks that
 * file only when it holds no bytes, then makes a directory. A file
 * that still holds bytes stays. A fifo is not the plate directory.
 * The read does not open it. replace=1 unlinks that fifo and makes a
 * directory. A waiting writer is not paired. */
static int open_plate_dir(int replace) {
  char d[640];
  struct stat st;
  int fd;
  int err;
  augogen_dir(d, sizeof d);
  for (;;) {
    /* Opening a fifo would pair a writer. Classify it before open. */
    if (lstat(d, &st) == 0 && S_ISFIFO(st.st_mode)) {
      if (!replace) return -1;
      if (unlink(d) != 0) return -1;
      if (mkdir(d, 0700) != 0 && errno != EEXIST) return -1;
      replace = 0;
      continue;
    }
    fd = open(d, O_RDONLY | O_NOFOLLOW | O_DIRECTORY);
    if (fd >= 0) {
      if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode)) {
        close(fd);
        return -1;
      }
      return fd;
    }
    err = errno;
    if (replace && (err == ELOOP || err == ENOTDIR) &&
        lstat(d, &st) == 0 && S_ISLNK(st.st_mode)) {
      if (unlink(d) != 0) return -1;
      if (mkdir(d, 0700) != 0 && errno != EEXIST) return -1;
      replace = 0;
      continue;
    }
    /* A regular file is not the directory. An empty one is removed.
     * A file that still holds bytes stays, and this write does not
     * replace it. O_NOFOLLOW leaves a link on the other branch. */
    if (replace && err == ENOTDIR) {
      int rfd = open(d, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
      if (rfd >= 0) {
        int reg = (fstat(rfd, &st) == 0 && S_ISREG(st.st_mode));
        int empty = reg && st.st_size == 0;
        close(rfd);
        if (reg && !empty) return -1;
        if (empty) {
          if (unlink(d) != 0) return -1;
          if (mkdir(d, 0700) != 0 && errno != EEXIST) return -1;
          replace = 0;
          continue;
        }
      }
    }
    if (err == ENOENT && replace) {
      if (mkdir(d, 0700) != 0 && errno != EEXIST) return -1;
      replace = 0;
      continue;
    }
    return -1;
  }
}

#if NANOBOT_HAS_BRAINCUBE
static void cube_path(char *buf, size_t n, const char *who) {
  snprintf(buf, n, "%s/augogen/%s.bin", ng_workdir(), who);
}

/* A symlink is not the cube. O_NOFOLLOW leaves the target unread.
 * A directory at this name is not the cube either. */
static char *read_cube_file(const char *path, size_t *out_len) {
  int fd;
  struct stat st;
  char *buf;
  size_t n = 0;
  if (out_len) *out_len = 0;
  if (!path || !path[0]) return NULL;
  fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) return NULL;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
    close(fd);
    return NULL;
  }
  buf = malloc((size_t)st.st_size + 1);
  if (!buf) {
    close(fd);
    return NULL;
  }
  while (n < (size_t)st.st_size) {
    ssize_t r = read(fd, buf + n, (size_t)st.st_size - n);
    if (r < 0) {
      if (errno == EINTR) continue;
      free(buf);
      close(fd);
      return NULL;
    }
    if (r == 0) break;
    n += (size_t)r;
  }
  close(fd);
  buf[n] = 0;
  if (out_len) *out_len = n;
  return buf;
}

/* rename replaces a symlink. It does not follow the link. An empty
 * directory at this name is not the cube and is removed. A directory
 * that still holds a name stays, and this write does not enter it. */
static int write_cube_file(const char *path, const void *data, size_t len) {
  char tmp[700];
  struct stat st;
  int fd;
  int nw;
  size_t off = 0;
  if (!path || !path[0] || !data) return -1;
  nw = snprintf(tmp, sizeof tmp, "%s.tmp-%d", path, (int)getpid());
  if (nw < 0 || (size_t)nw >= sizeof tmp) return -1;
  if (lstat(tmp, &st) == 0) {
    if (S_ISDIR(st.st_mode)) return -1;
    if (unlink(tmp) != 0) return -1;
  }
  fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  if (fd < 0) return -1;
  while (off < len) {
    ssize_t w = write(fd, (const char *)data + off, len - off);
    if (w < 0) {
      if (errno == EINTR) continue;
      close(fd);
      unlink(tmp);
      return -1;
    }
    if (w == 0) {
      close(fd);
      unlink(tmp);
      return -1;
    }
    off += (size_t)w;
  }
  if (close(fd) != 0) {
    unlink(tmp);
    return -1;
  }
  /* A directory is not the cube. rmdir removes only an empty one.
   * A symlink stays on the rename path, which replaces the link. */
  if (lstat(path, &st) == 0 && !S_ISLNK(st.st_mode) && S_ISDIR(st.st_mode)) {
    if (rmdir(path) != 0) {
      unlink(tmp);
      return -1;
    }
  }
  if (rename(tmp, path) != 0) {
    unlink(tmp);
    return -1;
  }
  return 0;
}

static void load_cube(lhlam_cube *c, const char *who) {
  char path[640];
  size_t len = 0;
  char *raw;
  uint8_t seed[32];
  if (augogen_dir_is_link()) {
    memset(seed, who[0] == 'g' ? 0x17 : 0x0E, sizeof seed);
    seed[0] = (uint8_t)(who[0] == 'g' ? 3 : 9);
    lhlam_cube_init(c, seed);
    return;
  }
  cube_path(path, sizeof path, who);
  raw = read_cube_file(path, &len);
  if (raw && len > 8 && lhlam_cube_import(c, (const uint8_t *)raw, len) == 0) {
    free(raw);
    return;
  }
  free(raw);
  memset(seed, who[0] == 'g' ? 0x17 : 0x0E, sizeof seed);
  seed[0] = (uint8_t)(who[0] == 'g' ? 3 : 9);
  lhlam_cube_init(c, seed);
}

static void save_cube(const lhlam_cube *c, const char *who) {
  uint8_t bin[8192];
  size_t need = lhlam_cube_export(c, bin, sizeof bin);
  char path[640];
  if (augogen_dir_is_link()) return;
  if (need == 0 || need > sizeof bin) return;
  cube_path(path, sizeof path, who);
  write_cube_file(path, bin, need);
}
#endif

static void plate_clear(ng_augogen *p) {
  memset(p, 0, sizeof *p);
  snprintf(p->status, sizeof p->status, "empty");
  p->guide_vote = -1;
  p->oversee_vote = -1;
  p->mesh_vote = -1;
  p->executed = 0;
  p->job_id[0] = 0;
}

static char *escape_or_empty(const char *s) {
  char *e = ng_json_escape(s ? s : "");
  return e ? e : strdup("");
}

static char *plate_json(const ng_augogen *p) {
  char *sug = escape_or_empty(p->suggestion);
  char *id = escape_or_empty(p->id);
  char *st = escape_or_empty(p->status);
  char *jid = escape_or_empty(p->job_id);
  char *ver = escape_or_empty(ng_cli_version());
  char *pesc = NULL;
  char *score = NULL;
  char *out = NULL;
  int auto_on = augogen_auto_enabled();
  const char *np = (p->confirmed && p->suggestion[0]) ? (sug ? sug : "") : NULL;
  /* A quote in the status, the plate name, or the cli version stays escaped. */
  if (p->score_kept && p->plate_name[0]) {
    pesc = escape_or_empty(p->plate_name);
    if (asprintf(&score,
                 "\"plate\":\"%s\",\"rank\":%d,\"lexical\":%d,\"hamming\":%d,",
                 pesc ? pesc : "", p->motive_rank, p->motive_lex,
                 p->motive_ham) < 0)
      score = NULL;
  } else if (p->score_kept) {
    if (asprintf(&score, "\"rank\":%d,\"lexical\":%d,\"hamming\":%d,",
                 p->motive_rank, p->motive_lex, p->motive_ham) < 0)
      score = NULL;
  }
  asprintf(&out,
           "{\"schema\":\"" NG_AUGOGEN_SCHEMA "\",\"ok\":true,"
           "\"action\":\"%s\",\"id\":\"%s\",\"generation\":%lu,"
           "\"suggestion\":\"%s\",\"status\":\"%s\","
           "\"guide_vote\":%d,\"oversee_vote\":%d,\"mesh_vote\":%d,"
           "\"pair_ok\":%s,\"confirmed\":%s,\"auto_execute\":%s,"
           "\"executed\":%s,\"job_id\":\"%s\","
           "\"next_prompt\":%s%s%s,"
           "\"api\":\"x.ai/suggestPrompt\","
           "\"cli_compat\":\"%s\","
           "\"hold_flash\":1,\"share\":\"state_matrix_only\","
           "%s\"python\":0}",
           st ? st : "",
           id ? id : "",
           p->generation,
           sug ? sug : "",
           st ? st : "empty",
           p->guide_vote, p->oversee_vote, p->mesh_vote,
           p->pair_ok ? "true" : "false",
           p->confirmed ? "true" : "false",
           auto_on ? "true" : "false",
           p->executed ? "true" : "false",
           jid ? jid : "",
           np ? "\"" : "null", np ? np : "", np ? "\"" : "",
           ver ? ver : "", score ? score : "");
  free(sug);
  free(id);
  free(st);
  free(jid);
  free(ver);
  free(pesc);
  free(score);
  return out ? out
             : strdup("{\"schema\":\"" NG_AUGOGEN_SCHEMA "\",\"ok\":false,"
                      "\"error\":\"oom\",\"python\":0}");
}

/* The response names the plate. A stored plate already carries rank. */
static char *with_report(char *js, const char *report, int rank, int lexical,
                         int hamming, int lex_w, int stored, const char *plate) {
  char *esc;
  char *pesc;
  char *cut;
  char *out = NULL;
  int have_rank;
  int have_plate;
  if (!js || !report || !report[0]) return js;
  esc = ng_json_escape(report);
  if (!esc) return js;
  pesc = ng_json_escape(plate ? plate : "");
  if (!pesc) {
    free(esc);
    return js;
  }
  cut = strstr(js, "\"python\":0}");
  if (!cut) {
    free(esc);
    free(pesc);
    return js;
  }
  have_rank = strstr(js, "\"rank\":") != NULL;
  have_plate = strstr(js, "\"plate\":") != NULL;
  if (have_rank && have_plate) {
    if (asprintf(&out,
                 "%.*s\"report\":\"%s\",\"lex_w\":%d,\"stored\":%s,\"python\":0}",
                 (int)(cut - js), js, esc, lex_w, stored ? "true" : "false") < 0)
      out = NULL;
  } else if (have_rank) {
    if (asprintf(&out,
                 "%.*s\"report\":\"%s\",\"plate\":\"%s\",\"lex_w\":%d,\"stored\":%s,\"python\":0}",
                 (int)(cut - js), js, esc, pesc, lex_w, stored ? "true" : "false") < 0)
      out = NULL;
  } else if (asprintf(&out,
                      "%.*s\"report\":\"%s\",\"plate\":\"%s\",\"rank\":%d,\"lexical\":%d,"
                      "\"hamming\":%d,\"lex_w\":%d,\"stored\":%s,\"python\":0}",
                      (int)(cut - js), js, esc, pesc, rank, lexical, hamming, lex_w,
                      stored ? "true" : "false") < 0)
    out = NULL;
  free(esc);
  free(pesc);
  if (!out) return js;
  free(js);
  return out;
}

/* A symlink is not the plate. O_NOFOLLOW leaves the target unread.
 * The directory fd is the real augogen directory, not a link to one.
 * A fifo at this name is not the plate. Opening it would pair a writer. */
static char *read_plate_at(int dfd, size_t *out_len) {
  int fd;
  struct stat st;
  char *buf;
  size_t n = 0;
  if (out_len) *out_len = 0;
  if (dfd < 0) return NULL;
  /* Classify a fifo before open. fstatat does not pair a writer. */
  if (fstatat(dfd, "pending.json", &st, AT_SYMLINK_NOFOLLOW) == 0 &&
      S_ISFIFO(st.st_mode))
    return NULL;
  fd = openat(dfd, "pending.json", O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) return NULL;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
    close(fd);
    return NULL;
  }
  buf = malloc((size_t)st.st_size + 1);
  if (!buf) {
    close(fd);
    return NULL;
  }
  while (n < (size_t)st.st_size) {
    ssize_t r = read(fd, buf + n, (size_t)st.st_size - n);
    if (r < 0) {
      if (errno == EINTR) continue;
      free(buf);
      close(fd);
      return NULL;
    }
    if (r == 0) break;
    n += (size_t)r;
  }
  close(fd);
  buf[n] = 0;
  if (out_len) *out_len = n;
  return buf;
}

static void load_plate(ng_augogen *p) {
  size_t len = 0;
  char *raw;
  char *s;
  int dfd;
  plate_clear(p);
  dfd = open_plate_dir(0);
  if (dfd < 0) return;
  raw = read_plate_at(dfd, &len);
  close(dfd);
  if (!raw || !raw[0]) {
    free(raw);
    return;
  }
  s = ng_json_get_string(raw, "id");
  if (s) {
    snprintf(p->id, sizeof p->id, "%s", s);
    free(s);
  }
  s = ng_json_get_string(raw, "suggestion");
  if (s) {
    snprintf(p->suggestion, sizeof p->suggestion, "%s", s);
    free(s);
  }
  s = ng_json_get_string(raw, "status");
  if (s) {
    snprintf(p->status, sizeof p->status, "%s", s);
    free(s);
  }
  s = ng_json_get_string(raw, "generation");
  if (s) {
    p->generation = strtoul(s, NULL, 10);
    free(s);
  } else if (strstr(raw, "\"generation\":")) {
    const char *g = strstr(raw, "\"generation\":");
    if (g) p->generation = strtoul(g + 13, NULL, 10);
  }
  if (strstr(raw, "\"guide_vote\":1")) p->guide_vote = 1;
  else if (strstr(raw, "\"guide_vote\":0")) p->guide_vote = 0;
  if (strstr(raw, "\"oversee_vote\":1")) p->oversee_vote = 1;
  else if (strstr(raw, "\"oversee_vote\":0")) p->oversee_vote = 0;
  if (strstr(raw, "\"mesh_vote\":1")) p->mesh_vote = 1;
  else if (strstr(raw, "\"mesh_vote\":0")) p->mesh_vote = 0;
  if (strstr(raw, "\"executed\":true")) p->executed = 1;
  s = ng_json_get_string(raw, "job_id");
  if (s) {
    snprintf(p->job_id, sizeof p->job_id, "%s", s);
    free(s);
  }
  if (strstr(raw, "\"rank\":")) {
    const char *r = strstr(raw, "\"rank\":");
    const char *lx = strstr(raw, "\"lexical\":");
    const char *hm = strstr(raw, "\"hamming\":");
    p->score_kept = 1;
    if (r) p->motive_rank = (int)strtol(r + 7, NULL, 10);
    if (lx) p->motive_lex = (int)strtol(lx + 10, NULL, 10);
    if (hm) p->motive_ham = (int)strtol(hm + 10, NULL, 10);
    s = ng_json_get_string(raw, "plate");
    if (s) {
      snprintf(p->plate_name, sizeof p->plate_name, "%s", s);
      free(s);
    }
  }
  p->pair_ok = (p->guide_vote == 1 && p->oversee_vote == 1);
  p->confirmed = p->pair_ok && (p->mesh_vote != 0);
  if (p->confirmed && strcmp(p->status, "applied") != 0)
    snprintf(p->status, sizeof p->status, "approved");
  free(raw);
}

/* A symlink at the plate path is not the file it names. renameat replaces
 * that link. A symlink named augogen is replaced with a directory. Either
 * target stays. A directory at the plate path is not the plate. An empty
 * one is removed before the rename. A directory that still holds a name
 * stays, and the write does not enter it. A fifo named augogen is not
 * the plate directory. The write replaces that fifo and does not open it.
 * A fifo at the plate path is not the plate. rename replaces it and does
 * not open it. A waiting writer is not paired. */
static int save_plate(const ng_augogen *p) {
  char tmp[64];
  char *js;
  int fd;
  int dfd;
  size_t len, off = 0;
  js = plate_json(p);
  if (!js) return -1;
  len = strlen(js);
  dfd = open_plate_dir(1);
  if (dfd < 0) {
    free(js);
    return -1;
  }
  if (snprintf(tmp, sizeof tmp, "pending.json.tmp-%d", (int)getpid()) >= (int)sizeof tmp) {
    close(dfd);
    free(js);
    return -1;
  }
  if (unlinkat(dfd, tmp, 0) != 0 && errno != ENOENT) {
    close(dfd);
    free(js);
    return -1;
  }
  fd = openat(dfd, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  if (fd < 0) {
    close(dfd);
    free(js);
    return -1;
  }
  while (off < len) {
    ssize_t w = write(fd, js + off, len - off);
    if (w < 0) {
      if (errno == EINTR) continue;
      close(fd);
      unlinkat(dfd, tmp, 0);
      close(dfd);
      free(js);
      return -1;
    }
    if (w == 0) {
      close(fd);
      unlinkat(dfd, tmp, 0);
      close(dfd);
      free(js);
      return -1;
    }
    off += (size_t)w;
  }
  if (fchmod(fd, 0600) != 0 || close(fd) != 0) {
    unlinkat(dfd, tmp, 0);
    close(dfd);
    free(js);
    return -1;
  }
  free(js);
  /* A directory is not the plate. AT_SYMLINK_NOFOLLOW keeps a link on the
   * rename path. rmdir removes only an empty directory. */
  {
    struct stat st;
    if (fstatat(dfd, "pending.json", &st, AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISDIR(st.st_mode)) {
      if (unlinkat(dfd, "pending.json", AT_REMOVEDIR) != 0) {
        unlinkat(dfd, tmp, 0);
        close(dfd);
        return -1;
      }
    }
  }
  if (renameat(dfd, tmp, dfd, "pending.json") != 0) {
    unlinkat(dfd, tmp, 0);
    close(dfd);
    return -1;
  }
  close(dfd);
  return 0;
}

static void recompute(ng_augogen *p) {
  p->pair_ok = (p->guide_vote == 1 && p->oversee_vote == 1);
  /* Mesh may abstain (-1). A 0 is a veto. Pair approve + no veto = confirmed. */
  p->confirmed = p->pair_ok && (p->mesh_vote != 0) && p->suggestion[0];
  if (p->confirmed) {
    if (strcmp(p->status, "applied") != 0)
      snprintf(p->status, sizeof p->status, "approved");
  } else if (p->guide_vote == 0 || p->oversee_vote == 0 || p->mesh_vote == 0) {
    snprintf(p->status, sizeof p->status, "rejected");
    p->confirmed = 0;
  } else if (p->suggestion[0]) {
    snprintf(p->status, sizeof p->status, "pending");
  } else {
    snprintf(p->status, sizeof p->status, "empty");
  }
}

/* 1 for an ASCII quote, 3 for a typographic quote, 0 otherwise. */
static int edge_quote(const char *p) {
  unsigned char a, b, c;
  if (!p || !p[0]) return 0;
  a = (unsigned char)p[0];
  if (a == '"' || a == '\'' || a == '`') return 1;
  if (!p[1] || !p[2]) return 0;
  b = (unsigned char)p[1];
  c = (unsigned char)p[2];
  if (a == 0xE2 && b == 0x80 && (c == 0x9C || c == 0x9D || c == 0x98 || c == 0x99))
    return 3;
  return 0;
}

/* Byte length of one well-formed UTF-8 sequence, or 0.
 * A lead plus its continuation bytes stay. A torn lead does not. */
static int utf8_seq(const char *p, size_t avail) {
  unsigned char c;
  int need, i;
  if (!p || avail == 0) return 0;
  c = (unsigned char)p[0];
  if (c < 0x80) return 1;
  if ((c & 0xE0) == 0xC0) need = 2;
  else if ((c & 0xF0) == 0xE0) need = 3;
  else if ((c & 0xF8) == 0xF0) need = 4;
  else return 0;
  if ((size_t)need > avail) return 0;
  for (i = 1; i < need; i++) {
    if (((unsigned char)p[i] & 0xC0) != 0x80) return 0;
  }
  return need;
}

/* Byte length of one unicode space, or 0. ASCII space is not included.
 * Quote bytes E2 80 98/99/9C/9D stay out of this set. An em dash stays out. */
static int utf8_space(const char *p) {
  unsigned char a, b, c;
  if (!p || !p[0]) return 0;
  a = (unsigned char)p[0];
  if (a == 0xC2 && p[1]) {
    b = (unsigned char)p[1];
    if (b == 0x85 || b == 0xA0) return 2; /* U+0085, U+00A0 */
  }
  if (!p[1] || !p[2]) return 0;
  b = (unsigned char)p[1];
  c = (unsigned char)p[2];
  if (a == 0xE1 && b == 0x9A && c == 0x80) return 3; /* U+1680 */
  if (a == 0xE2 && b == 0x80 &&
      ((c >= 0x80 && c <= 0x8A) || c == 0xA8 || c == 0xA9 || c == 0xAF))
    return 3; /* U+2000–U+200A, U+2028, U+2029, U+202F */
  if (a == 0xE2 && b == 0x81 && c == 0x9F) return 3; /* U+205F */
  if (a == 0xE3 && b == 0x80 && c == 0x80) return 3; /* U+3000 */
  return 0;
}

/* 1 for a separator from U+001C through U+001F. A backspace is outside. */
static int field_sep(unsigned char c) {
  return c >= 0x1C && c <= 0x1F;
}

/* 1 kept, 0 when there is no line, -1 when the line is rejected.
 * A rejected line leaves the buffer empty. */
static int sanitize_suggestion(const char *raw, char *out, size_t cap) {
  char line[SUG_CAP];
  size_t i = 0;
  const char *p;
  char lowered[SUG_CAP];
  static const char *meta[] = {
      "none", "n/a", "no suggestion", "nothing", "(silence)", "silence", "null",
      NULL};
  if (!raw || !out || cap < 2) return 0;
  while (*raw == ' ' || *raw == '\t' || *raw == '\n' || *raw == '\r') raw++;
  while (*raw && *raw != '\n' && *raw != '\r' && i + 1 < sizeof line)
    line[i++] = *raw++;
  line[i] = 0;
  /* A torn byte drops. A quote behind it still drops. The words stay. */
  {
    size_t r = 0, w = 0;
    while (r < i) {
      int n = utf8_seq(line + r, i - r);
      if (n <= 0) {
        r++;
        continue;
      }
      if (w != r) memmove(line + w, line + r, (size_t)n);
      w += (size_t)n;
      r += (size_t)n;
    }
    i = w;
    line[i] = 0;
  }
  /* Trim quotes and edge whitespace. A quote behind a unicode space still drops.
   * The words stay. An em dash stays. */
  p = line;
  for (;;) {
    int n = edge_quote(p);
    if (n) {
      p += n;
      continue;
    }
    if (*p == ' ' || *p == '\t' || *p == '\v' || *p == '\f' ||
        field_sep((unsigned char)*p)) {
      p++;
      continue;
    }
    n = utf8_space(p);
    if (n) {
      p += n;
      continue;
    }
    break;
  }
  i = strlen(p);
  for (;;) {
    int n;
    while (i > 0 && (p[i - 1] == '"' || p[i - 1] == '\'' || p[i - 1] == '`' ||
                     p[i - 1] == ' ' || p[i - 1] == '\t' || p[i - 1] == '\v' ||
                     p[i - 1] == '\f' || p[i - 1] == '.' ||
                     field_sep((unsigned char)p[i - 1])))
      i--;
    if (i >= 3 && (n = edge_quote(p + i - 3)) == 3) {
      i -= 3;
      continue;
    }
    if (i >= 3 && utf8_space(p + i - 3) == 3) {
      i -= 3;
      continue;
    }
    if (i >= 2 && utf8_space(p + i - 2) == 2) {
      i -= 2;
      continue;
    }
    break;
  }
  if (i >= cap) i = cap - 1;
  /* A unicode space inside the line is a space. A tab, a vertical tab,
   * or a form feed inside the line is a space. A separator from U+001C
   * through U+001F inside the line is a space. A backspace stays.
   * The words stay. An em dash stays. A doubled space stays doubled. */
  {
    char tmp[SUG_CAP];
    size_t o = 0, k = 0;
    while (k < i && o + 1 < cap && o + 1 < sizeof tmp) {
      int n = utf8_space(p + k);
      unsigned char c;
      if (n > 0 && k + (size_t)n <= i) {
        tmp[o++] = ' ';
        k += (size_t)n;
        continue;
      }
      c = (unsigned char)p[k];
      if (c == '\t' || c == '\v' || c == '\f' || field_sep(c)) {
        tmp[o++] = ' ';
        k++;
        continue;
      }
      tmp[o++] = p[k++];
    }
    memcpy(out, tmp, o);
    out[o] = 0;
  }
  if (!out[0]) return 0;
  for (i = 0; out[i] && i + 1 < sizeof lowered; i++) {
    char c = out[i];
    lowered[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
  }
  lowered[i] = 0;
  for (i = 0; meta[i]; i++) {
    if (strcmp(lowered, meta[i]) == 0 ||
        (strncmp(lowered, meta[i], strlen(meta[i])) == 0 &&
         lowered[strlen(meta[i])] == '.')) {
      out[0] = 0;
      return -1;
    }
  }
  /* 2–12 words, matching grok-build eval bound */
  {
    int words = 0, inw = 0;
    for (i = 0; out[i]; i++) {
      if (out[i] != ' ' && out[i] != '\t') {
        if (!inw) {
          words++;
          inw = 1;
        }
      } else {
        inw = 0;
      }
    }
    if (words < 2 || words > 12) {
      out[0] = 0;
      return -1;
    }
  }
  return 1;
}

static void new_id(char *out, size_t n) {
  snprintf(out, n, "ag-%ld-%d", (long)time(NULL), (int)(getpid() & 0xFFFF));
}

/* Copy an env knob, or the settings value when the env is empty. */
static void knob_copy(char *dst, size_t cap, const char *envkey, const char *setkey) {
  const char *e = getenv(envkey);
  char *owned = NULL;
  size_t n;
  if (!dst || cap < 2) return;
  dst[0] = 0;
  if (!e || !e[0]) {
    owned = ng_settings_get(setkey);
    e = owned;
  }
  if (e && e[0]) snprintf(dst, cap, "%s", e);
  free(owned);
  n = strlen(dst);
  while (n && (dst[n - 1] == '\n' || dst[n - 1] == '\r' || dst[n - 1] == ' '))
    dst[--n] = 0;
}

/* IPv4 literal only, so a nudge never waits on DNS. */
static int peer_ipv4(const char *spec, char *host, size_t hcap, int *port) {
  char buf[80];
  char *colon, *slash;
  const char *p;
  size_t n;
  if (!spec || !host || hcap < 16 || !port) return 0;
  p = spec;
  while (*p == ' ') p++;
  if (!strncmp(p, "https://", 8)) return 0;
  if (!strncmp(p, "http://", 7)) p += 7;
  n = strlen(p);
  if (n == 0 || n >= sizeof buf) return 0;
  memcpy(buf, p, n + 1);
  slash = strchr(buf, '/');
  if (slash) *slash = 0;
  colon = strrchr(buf, ':');
  if (!colon || colon == buf || !colon[1]) return 0;
  *colon = 0;
  if (strlen(buf) >= hcap) return 0;
  for (p = buf; *p; p++) {
    if (*p != '.' && (*p < '0' || *p > '9')) return 0;
  }
  *port = atoi(colon + 1);
  if (*port <= 0 || *port > 65535) return 0;
  memcpy(host, buf, strlen(buf) + 1);
  return 1;
}

static int token_safe(const char *t) {
  size_t n;
  if (!t || !t[0]) return 0;
  n = strlen(t);
  if (n > 80) return 0;
  for (; *t; t++) {
    unsigned char c = (unsigned char)*t;
    if (c <= 32 || c >= 127) return 0;
  }
  return 1;
}

/* Owner-only regular file. A group or other bit skips it. */
static int token_from_file(char *dst, size_t cap) {
  char path[256];
  char buf[96];
  struct stat st;
  FILE *f;
  size_t n;
  char *p;
  if (!dst || cap < 2) return 0;
  dst[0] = 0;
  knob_copy(path, sizeof path, "NANOBOT_MOTIVE_PEER_TOKEN_FILE", "MOTIVE_PEER_TOKEN_FILE");
  if (path[0] != '/') return 0;
  if (lstat(path, &st) != 0) return 0;
  if (!S_ISREG(st.st_mode)) return 0;
  if (st.st_mode & (S_IRWXG | S_IRWXO)) return 0;
  if (st.st_size <= 0 || st.st_size > 80) return 0;
  f = fopen(path, "r");
  if (!f) return 0;
  n = fread(buf, 1, sizeof buf - 1, f);
  fclose(f);
  buf[n] = 0;
  p = buf;
  if (!strncmp(p, "token=", 6)) p += 6;
  n = strlen(p);
  while (n && (p[n - 1] == '\n' || p[n - 1] == '\r' || p[n - 1] == ' ')) p[--n] = 0;
  if (!token_safe(p)) return 0;
  snprintf(dst, cap, "%s", p);
  return dst[0] != 0;
}

static int connect_budget(int fd, const struct sockaddr_in *sa) {
  int flags, err = 0;
  socklen_t el = sizeof err;
  struct pollfd pfd;
  flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return 0;
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return 0;
  if (connect(fd, (const struct sockaddr *)sa, sizeof *sa) == 0) return 1;
  if (errno != EINPROGRESS) return 0;
  pfd.fd = fd;
  pfd.events = POLLOUT;
  pfd.revents = 0;
  if (poll(&pfd, 1, 200) <= 0) return 0;
  if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err) return 0;
  return 1;
}

static int write_budget(int fd, const char *buf, size_t n) {
  size_t off = 0;
  while (off < n) {
    struct pollfd pfd;
    ssize_t w;
    pfd.fd = fd;
    pfd.events = POLLOUT;
    pfd.revents = 0;
    if (poll(&pfd, 1, 200) <= 0) return 0;
    w = write(fd, buf + off, n - off);
    if (w < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
      return 0;
    }
    if (w == 0) return 0;
    off += (size_t)w;
  }
  return 1;
}

/* Missing stays agreeable. A bad or different integer does not. */
static int quoted_eq(const char *s, int want) {
  char *end = NULL;
  long v;
  if (!s || !s[0]) return 1;
  v = strtol(s, &end, 10);
  if (!end || end == s || *end) return 0;
  return v == (long)want;
}

/* One configured peer, one POST, no retry. The body carries hop=1.
 * lex_w 0-255 is quoted on that POST. A negative value omits the field.
 * plate is the ranked plate name. lexical and hamming are the rank parts.
 * concept is the ranked concept line. The peer stores only when a quoted
 * value agrees. */
static void motive_nudge(const char *line, int lex_w, const char *plate,
                         int lexical, int hamming, const char *concept) {
  char spec[96], token[96], host[16];
  char body[384], req[768];
  char *esc = NULL, *pesc = NULL, *cesc = NULL;
  int port = 0, fd = -1, bl, rl;
  struct sockaddr_in sa;
  knob_copy(spec, sizeof spec, "NANOBOT_MOTIVE_PEER", "MOTIVE_PEER");
  knob_copy(token, sizeof token, "NANOBOT_MOTIVE_PEER_TOKEN", "MOTIVE_PEER_TOKEN");
  if (!token_safe(token)) token[0] = 0;
  if (!token[0]) token_from_file(token, sizeof token);
  if (!spec[0] || !token_safe(token)) return;
  if (!peer_ipv4(spec, host, sizeof host, &port)) return;
  esc = ng_json_escape(line ? line : "");
  if (!esc) return;
  pesc = ng_json_escape(plate && plate[0] ? plate : "");
  if (!pesc) {
    free(esc);
    return;
  }
  cesc = ng_json_escape(concept && concept[0] ? concept : "");
  if (!cesc) {
    free(esc);
    free(pesc);
    return;
  }
  if (lex_w >= 0 && lex_w <= 255 && pesc[0])
    bl = snprintf(body, sizeof body,
                  "{\"action\":\"motive\",\"text\":\"%s\",\"enqueue\":\"1\","
                  "\"hop\":\"1\",\"lex_w\":\"%d\",\"plate\":\"%s\","
                  "\"lexical\":\"%d\",\"hamming\":\"%d\",\"concept\":\"%s\"}",
                  esc, lex_w, pesc, lexical, hamming, cesc);
  else if (lex_w >= 0 && lex_w <= 255)
    bl = snprintf(body, sizeof body,
                  "{\"action\":\"motive\",\"text\":\"%s\",\"enqueue\":\"1\","
                  "\"hop\":\"1\",\"lex_w\":\"%d\","
                  "\"lexical\":\"%d\",\"hamming\":\"%d\",\"concept\":\"%s\"}",
                  esc, lex_w, lexical, hamming, cesc);
  else if (pesc[0])
    bl = snprintf(body, sizeof body,
                  "{\"action\":\"motive\",\"text\":\"%s\",\"enqueue\":\"1\","
                  "\"hop\":\"1\",\"plate\":\"%s\","
                  "\"lexical\":\"%d\",\"hamming\":\"%d\",\"concept\":\"%s\"}",
                  esc, pesc, lexical, hamming, cesc);
  else
    bl = snprintf(body, sizeof body,
                  "{\"action\":\"motive\",\"text\":\"%s\",\"enqueue\":\"1\","
                  "\"hop\":\"1\",\"lexical\":\"%d\",\"hamming\":\"%d\","
                  "\"concept\":\"%s\"}",
                  esc, lexical, hamming, cesc);
  free(esc);
  free(pesc);
  free(cesc);
  if (bl < 0 || (size_t)bl >= sizeof body) return;
  rl = snprintf(req, sizeof req,
                "POST /peer/v1/augogen HTTP/1.0\r\n"
                "Host: %s:%d\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: %d\r\n"
                "X-Nanobot-Peer-Token: %s\r\n"
                "Connection: close\r\n\r\n"
                "%s",
                host, port, bl, token, body);
  if (rl < 0 || (size_t)rl >= sizeof req) return;
  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return;
  memset(&sa, 0, sizeof sa);
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, host, &sa.sin_addr) != 1 ||
      !connect_budget(fd, &sa) || !write_budget(fd, req, (size_t)rl)) {
    close(fd);
    return;
  }
  shutdown(fd, SHUT_WR);
  {
    char dump[64];
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, 200) > 0) {
      ssize_t n = read(fd, dump, sizeof dump);
      (void)n;
    }
  }
  close(fd);
}

char *ng_augogen_pending_json(void) {
  ng_augogen p;
  load_plate(&p);
  return plate_json(&p);
}

char *ng_augogen_handle(ng_agent_cfg *agent, const char *json_body) {
  ng_augogen p;
  char *action;
  if (!json_body) json_body = "{}";
  load_plate(&p);
  action = ng_json_get_string(json_body, "action");
  if (!action) action = strdup("pending");

  if (!strcmp(action, "pending") || !strcmp(action, "status") ||
      !strcmp(action, "get")) {
    free(action);
    return plate_json(&p);
  }

  if (!strcmp(action, "reset") || !strcmp(action, "clear")) {
    plate_clear(&p);
    save_plate(&p);
    free(action);
    return plate_json(&p);
  }

  if (!strcmp(action, "reject")) {
    p.confirmed = 0;
    p.pair_ok = 0;
    snprintf(p.status, sizeof p.status, "rejected");
    save_plate(&p);
    free(action);
    return plate_json(&p);
  }

  if (!strcmp(action, "vote")) {
    char *role = ng_json_get_string(json_body, "role");
    char *vs = ng_json_get_string(json_body, "vote");
    int v = 0;
    if (!p.suggestion[0]) {
      free(role);
      free(vs);
      free(action);
      return strdup("{\"schema\":\"" NG_AUGOGEN_SCHEMA "\",\"ok\":false,"
                    "\"action\":\"vote\",\"error\":\"no_pending\","
                    "\"auto_execute\":false,\"python\":0}");
    }
    if (vs && vs[0]) v = (vs[0] == '1' || vs[0] == 't' || vs[0] == 'T' ||
                          vs[0] == 'y' || vs[0] == 'Y')
                             ? 1
                             : 0;
    if (!role) role = strdup("mesh");
    if (!strcmp(role, "guide") || !strcmp(role, "construct") ||
        !strcmp(role, "center"))
      p.guide_vote = v;
    else if (!strcmp(role, "oversee") || !strcmp(role, "deconstruct") ||
             !strcmp(role, "observer"))
      p.oversee_vote = v;
    else
      p.mesh_vote = v;
#if NANOBOT_HAS_BRAINCUBE
    {
      lhlam_cube guide, oversee;
      int pair;
      load_cube(&guide, "guide");
      load_cube(&oversee, "oversee");
      pair = lhlam_cube_pair_confirm_cstr(&guide, &oversee, p.suggestion);
      p.pair_ok = pair || (p.guide_vote == 1 && p.oversee_vote == 1);
      save_cube(&guide, "guide");
      save_cube(&oversee, "oversee");
    }
#endif
    recompute(&p);
    if (augogen_auto_enabled() && p.confirmed && !p.executed)
      try_auto_run(&p, agent);
    save_plate(&p);
    free(role);
    free(vs);
    free(action);
    return plate_json(&p);
  }

  if (!strcmp(action, "apply")) {
    if (!p.confirmed) {
      free(action);
      return strdup("{\"schema\":\"" NG_AUGOGEN_SCHEMA "\",\"ok\":false,"
                    "\"action\":\"apply\",\"error\":\"not_confirmed\","
                    "\"auto_execute\":false,\"python\":0}");
    }
    if (augogen_auto_enabled() && !p.executed)
      try_auto_run(&p, agent);
    else
      snprintf(p.status, sizeof p.status, "applied");
    save_plate(&p);
    free(action);
    return plate_json(&p);
  }

  if (!strcmp(action, "mode")) {
    char *vs = ng_json_get_string(json_body, "auto");
    if (!vs) vs = ng_json_get_string(json_body, "value");
    if (vs && vs[0]) {
      ng_settings_set("AUGOGEN_AUTO", truthy_off(vs) ? "0" : "1");
      setenv("NANOBOT_AUGOGEN_AUTO", truthy_off(vs) ? "0" : "1", 1);
    }
    free(vs);
    free(action);
    return plate_json(&p);
  }

  if (!strcmp(action, "motive") || !strcmp(action, "rank")) {
    char *text = ng_json_get_string(json_body, "text");
    char *enq = ng_json_get_string(json_body, "enqueue");
    char *hopv = ng_json_get_string(json_body, "hop");
    char *lwv = ng_json_get_string(json_body, "lex_w");
    char *platev = ng_json_get_string(json_body, "plate");
    char *lexn = ng_json_get_string(json_body, "lexical");
    char *hamn = ng_json_get_string(json_body, "hamming");
    char *conceptv = ng_json_get_string(json_body, "concept");
    ng_motive m;
    char *js;
    int asked = 0, hop = 0, over = -1, plate_ok = 1, score_ok = 1, concept_ok = 1;
    if (!text) text = ng_json_get_string(json_body, "act");
    if (!text) text = strdup("");
    if (enq && (enq[0] == '1' || enq[0] == 't' || enq[0] == 'T' || enq[0] == 'y' ||
                enq[0] == 'Y'))
      asked = 1;
    if (hopv && (hopv[0] == '1' || hopv[0] == 't' || hopv[0] == 'T' ||
                 hopv[0] == 'y' || hopv[0] == 'Y'))
      hop = 1;
    /* A quoted lex_w applies to this call. A bad value leaves the env weight. */
    if (lwv && lwv[0]) {
      char *end = NULL;
      long v = strtol(lwv, &end, 10);
      if (end && end != lwv && !*end && v >= 0 && v <= 255) over = (int)v;
    }
    ng_motive_rank_w(text ? text : "", &m, over);
    /* A quoted plate, lexical, hamming, or concept that disagrees is not stored. */
    if (platev && platev[0] && strcmp(platev, ng_motive_plate_name(m.plate)) != 0)
      plate_ok = 0;
    if (!quoted_eq(lexn, m.lexical) || !quoted_eq(hamn, m.hamming)) score_ok = 0;
    if (conceptv && conceptv[0] && strcmp(conceptv, m.concept) != 0) concept_ok = 0;
    /* Report every plate. Store a line only when the plate is forward. */
    if (asked && plate_ok && score_ok && concept_ok && m.forward && m.line[0] &&
        !suggestion_forbidden(m.line) && !ng_motive_blocks(m.line)) {
      unsigned long gen = p.generation + 1;
      plate_clear(&p);
      p.generation = gen;
      new_id(p.id, sizeof p.id);
      snprintf(p.suggestion, sizeof p.suggestion, "%s", m.line);
      snprintf(p.status, sizeof p.status, "pending");
      p.score_kept = 1;
      p.motive_rank = m.rank;
      p.motive_lex = m.lexical;
      p.motive_ham = m.hamming;
      snprintf(p.plate_name, sizeof p.plate_name, "%s", ng_motive_plate_name(m.plate));
      if (augogen_auto_enabled()) {
        if (!pair_confirm_suggestion(&p)) {
          p.guide_vote = 1;
          p.oversee_vote = 1;
          p.pair_ok = 1;
        }
        recompute(&p);
        if (p.confirmed && agent) try_auto_run(&p, agent);
      }
      save_plate(&p);
      /* A hop already arrived from one peer. Do not POST it onward.
       * over is the quoted lex_w, or -1 when this call keeps the env weight.
       * The body is the stored line. Quote that line's own rank so the
       * peer's check of the body agrees. The sender plate keeps the act. */
      if (!hop) {
        ng_motive posted;
        ng_motive_rank_w(m.line, &posted, over);
        motive_nudge(m.line, over, ng_motive_plate_name(posted.plate),
                     posted.lexical, posted.hamming, posted.concept);
      }
    }
    js = ng_motive_format(&m);
    free(text);
    free(enq);
    free(hopv);
    free(lwv);
    free(platev);
    free(lexn);
    free(hamn);
    free(conceptv);
    free(action);
    return js ? js
              : strdup("{\"schema\":\"" NG_MOTIVE_SCHEMA "\",\"ok\":false,"
                       "\"error\":\"oom\",\"python\":0}");
  }

  if (!strcmp(action, "generate") || !strcmp(action, "suggest") ||
      !strcmp(action, "suggestPrompt") || !strcmp(action, "auto")) {
    char *given = ng_json_get_string(json_body, "suggestion");
    char *transcript = ng_json_get_string(json_body, "transcript");
    char *enq = ng_json_get_string(json_body, "enqueue");
    char cleaned[SUG_CAP];
    char reported[SUG_CAP];
    int report_rank = 0, report_lex = 0, report_ham = 0, report_lex_w = 64;
    int report_plate = NG_PLATE_OBSERVE;
    int asked = 0;
    unsigned long gen = p.generation + 1;
    int host_reject = 0;
    cleaned[0] = 0;
    reported[0] = 0;
    if (enq && (enq[0] == '1' || enq[0] == 't' || enq[0] == 'T' || enq[0] == 'y' ||
                enq[0] == 'Y'))
      asked = 1;
    if (given && given[0]) {
      int sr = sanitize_suggestion(given, cleaned, sizeof cleaned);
      /* A rejected host line is not silence and does not take its place. */
      if (sr < 0) {
        host_reject = 1;
        cleaned[0] = 0;
      }
    }
    if (!cleaned[0] && agent) {
      char *raw = ng_agent_suggest_prompt(agent, transcript, ng_workdir());
      if (raw) {
        if (sanitize_suggestion(raw, cleaned, sizeof cleaned) != 1) cleaned[0] = 0;
        free(raw);
      }
    }
    /* No model line: report a forward concept.
       Store it only when enqueue asks. Silence reports the peer-prompt concept.
       The act stays off the plate. */
    if (!cleaned[0] && !host_reject) {
      ng_motive ranked;
      ng_motive_rank(transcript ? transcript : "", &ranked);
      if (ranked.forward && ranked.line[0] && !suggestion_forbidden(ranked.line) &&
          !ng_motive_blocks(ranked.line)) {
        snprintf(reported, sizeof reported, "%s", ranked.line);
        report_rank = ranked.rank;
        report_lex = ranked.lexical;
        report_ham = ranked.hamming;
        report_lex_w = ranked.lex_w;
        report_plate = ranked.plate;
        if (asked) snprintf(cleaned, sizeof cleaned, "%s", ranked.line);
      }
    }
    free(given);
    free(transcript);
    free(enq);
    plate_clear(&p);
    p.generation = gen;
    new_id(p.id, sizeof p.id);
    if (cleaned[0]) {
      snprintf(p.suggestion, sizeof p.suggestion, "%s", cleaned);
      if (suggestion_forbidden(cleaned) || ng_motive_blocks(cleaned)) {
        snprintf(p.status, sizeof p.status, "rejected");
        p.confirmed = 0;
      } else {
        snprintf(p.status, sizeof p.status, "pending");
        if (asked && reported[0]) {
          p.score_kept = 1;
          p.motive_rank = report_rank;
          p.motive_lex = report_lex;
          p.motive_ham = report_ham;
          snprintf(p.plate_name, sizeof p.plate_name, "%s",
                   ng_motive_plate_name(report_plate));
        }
        if (augogen_auto_enabled()) {
          /* Full auto: try braincube pair; if it withholds, operator-authorized
           * auto-pair (guide+oversee=1). Mesh 0 still vetoes. */
          if (!pair_confirm_suggestion(&p)) {
            p.guide_vote = 1;
            p.oversee_vote = 1;
            p.pair_ok = 1;
          }
          recompute(&p);
          if (p.confirmed) try_auto_run(&p, agent);
        }
      }
    } else {
      snprintf(p.status, sizeof p.status, "empty");
    }
    save_plate(&p);
    free(action);
    return with_report(plate_json(&p), reported, report_rank, report_lex,
                       report_ham, report_lex_w, asked && reported[0],
                       ng_motive_plate_name(report_plate));
  }

  {
    char *esc = ng_json_escape(action);
    char *err = NULL;
    asprintf(&err,
             "{\"schema\":\"" NG_AUGOGEN_SCHEMA "\",\"ok\":false,"
             "\"action\":\"error\",\"error\":\"unknown_action\","
             "\"got\":\"%s\",\"auto_execute\":false,\"python\":0}",
             esc ? esc : "");
    free(esc);
    free(action);
    return err ? err
               : strdup("{\"schema\":\"" NG_AUGOGEN_SCHEMA "\",\"ok\":false,"
                        "\"error\":\"unknown_action\",\"python\":0}");
  }
}
