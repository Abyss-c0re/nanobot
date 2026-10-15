#include "augogen.h"
#include "motive.h"
#include "util.h"
#include "memory.h"
#include <arpa/inet.h>
#include <ctype.h>
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

/* Fork a prompt job (same shape as POST /peer/v1/jobs). Never blocks HTTP. */
static int enqueue_prompt(ng_agent_cfg *agent, const char *prompt, char *id_out,
                          size_t id_n) {
  char jdir[640], mpath[700], rpath[700], pp[700], meta[768];
  char id[40];
  pid_t w;
  int mn;
  if (!prompt || !prompt[0]) return 0;
  snprintf(jdir, sizeof jdir, "%s/jobs", ng_workdir());
  mkdir(jdir, 0755);
  snprintf(id, sizeof id, "agrun-%ld-%d", (long)time(NULL), (int)(getpid() & 0xFFFF));
  snprintf(mpath, sizeof mpath, "%s/%s.json", jdir, id);
  snprintf(rpath, sizeof rpath, "%s/%s.out", jdir, id);
  snprintf(pp, sizeof pp, "%s/%s.in", jdir, id);
  mn = snprintf(meta, sizeof meta,
                "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,"
                "\"action\":\"job\",\"id\":\"%s\",\"status\":\"queued\","
                "\"kind\":\"prompt\",\"via\":\"augogen_auto\"}\n",
                id);
  ng_write_file(mpath, meta, (size_t)mn);
  ng_write_file(pp, prompt, strlen(prompt));
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
  if (w < 0) return 0;
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

static void plate_path(char *buf, size_t n) {
  snprintf(buf, n, "%s/augogen/pending.json", ng_workdir());
}

#if NANOBOT_HAS_BRAINCUBE
static void cube_path(char *buf, size_t n, const char *who) {
  snprintf(buf, n, "%s/augogen/%s.bin", ng_workdir(), who);
}

static void load_cube(lhlam_cube *c, const char *who) {
  char path[640];
  size_t len = 0;
  char *raw;
  uint8_t seed[32];
  cube_path(path, sizeof path, who);
  raw = ng_read_file(path, &len);
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
  if (need == 0 || need > sizeof bin) return;
  cube_path(path, sizeof path, who);
  ng_write_file(path, (const char *)bin, need);
}
#endif

static void ensure_dir(void) {
  char d[640];
  augogen_dir(d, sizeof d);
  mkdir(d, 0700);
}

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
  char *out = NULL;
  char score[128];
  int auto_on = augogen_auto_enabled();
  const char *np = (p->confirmed && p->suggestion[0]) ? (sug ? sug : "") : NULL;
  score[0] = 0;
  if (p->score_kept && p->plate_name[0])
    snprintf(score, sizeof score,
             "\"plate\":\"%s\",\"rank\":%d,\"lexical\":%d,\"hamming\":%d,",
             p->plate_name, p->motive_rank, p->motive_lex, p->motive_ham);
  else if (p->score_kept)
    snprintf(score, sizeof score, "\"rank\":%d,\"lexical\":%d,\"hamming\":%d,",
             p->motive_rank, p->motive_lex, p->motive_ham);
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
           p->status,
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
           ng_cli_version(), score);
  free(sug);
  free(id);
  free(st);
  free(jid);
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

static void load_plate(ng_augogen *p) {
  char path[640];
  size_t len = 0;
  char *raw;
  char *s;
  plate_clear(p);
  plate_path(path, sizeof path);
  raw = ng_read_file(path, &len);
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

static int save_plate(const ng_augogen *p) {
  char path[640];
  char *js;
  int rc;
  ensure_dir();
  plate_path(path, sizeof path);
  js = plate_json(p);
  rc = ng_write_file(path, js, strlen(js));
  free(js);
  return rc;
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

/* grok-build prompt_suggest::sanitize_suggestion */
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
  /* trim quotes */
  p = line;
  while (*p == '"' || *p == '\'' || *p == '`' || (unsigned char)*p == 0xE2)
    p++;
  i = strlen(p);
  while (i > 0 && (p[i - 1] == '"' || p[i - 1] == '\'' || p[i - 1] == '`' ||
                   p[i - 1] == ' ' || p[i - 1] == '.'))
    i--;
  if (i >= cap) i = cap - 1;
  memcpy(out, p, i);
  out[i] = 0;
  if (!out[0]) return 0;
  for (i = 0; out[i] && i + 1 < sizeof lowered; i++) {
    char c = out[i];
    lowered[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
  }
  lowered[i] = 0;
  for (i = 0; meta[i]; i++) {
    if (strcmp(lowered, meta[i]) == 0) return 0;
    if (strncmp(lowered, meta[i], strlen(meta[i])) == 0 &&
        lowered[strlen(meta[i])] == '.')
      return 0;
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
    if (words < 2 || words > 12) return 0;
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
    int stored = 0;
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
      if (save_plate(&p) == 0) stored = 1;
      /* A hop already arrived from one peer. Do not POST it onward.
       * over is the quoted lex_w, or -1 when this call keeps the env weight. */
      if (!hop)
        motive_nudge(m.line, over, ng_motive_plate_name(m.plate), m.lexical,
                     m.hamming, m.concept);
    }
    js = ng_motive_format(&m);
    if (js) {
      char *cut = strstr(js, "\"python\":0}");
      char *tagged = NULL;
      if (cut && asprintf(&tagged, "%.*s\"stored\":%s,\"python\":0}",
                          (int)(cut - js), js, stored ? "true" : "false") >= 0) {
        free(js);
        js = tagged;
      }
    }
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
    cleaned[0] = 0;
    reported[0] = 0;
    if (enq && (enq[0] == '1' || enq[0] == 't' || enq[0] == 'T' || enq[0] == 'y' ||
                enq[0] == 'Y'))
      asked = 1;
    if (given && sanitize_suggestion(given, cleaned, sizeof cleaned)) {
      /* host-supplied line (CubalC / mesh) still needs pair vote */
    } else if (agent) {
      char *raw = ng_agent_suggest_prompt(agent, transcript, ng_workdir());
      if (raw) {
        sanitize_suggestion(raw, cleaned, sizeof cleaned);
        free(raw);
      }
    }
    /* No model line: report a forward concept.
       Store it only when enqueue asks. Silence reports the peer-prompt concept.
       The act stays off the plate. */
    if (!cleaned[0]) {
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
