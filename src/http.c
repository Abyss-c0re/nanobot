#include "http.h"
#include "mcp_remote.h"
#include "auth.h"
#include "shell.h"
#include "shell_gate.h"
#include "util.h"
#include "hub_local.h"
#include "agent.h"
#include "subagent.h"
#include "ng_sched.h"
#include "braincube_plugin.h"
#include <nanobot/crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <strings.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>

/* Set once listen succeeds — health plate (parent listener, not fork workers). */
static time_t g_serve_started = 0;
static pid_t g_serve_pid = 0;

/*
 * Dual-wire lab/ops peer HTTP plate tail (schema nanobot.peer_http.v1).
 * Product bus remains SMX2; this HTTP surface is lab/ops only.
 * Defined early so jobs_gc/orphan helpers can embed the same plate.
 */
#define NG_PEER_HTTP_DUAL_WIRE                                                 \
  "\"product_wire\":\"smx2\",\"peer_http\":\"lab_ops_only\","                  \
  "\"peer_http_is_product_bus\":false,"                                        \
  "\"share\":\"state_matrix_only\",\"hold_flash\":1,"                          \
  "\"llm_is_commander\":false,\"python\":0"

/* Cap finished job artifacts under $HOME/jobs (mesh leaves many done/*.json).
 * Never unlink queued/running. Ids are time-prefixed so lexicographic order
 * is chronological. */
enum { NG_JOBS_KEEP = 48, NG_JOBS_SCAN = 256 };

/* Count $HOME/jobs/*.json for health/info (mesh focus probes). */
static int jobs_meta_count(void) {
  char jdir[640];
  snprintf(jdir, sizeof jdir, "%s/jobs", ng_workdir());
  DIR *d = opendir(jdir);
  if (!d) return 0;
  int n = 0;
  struct dirent *de;
  while ((de = readdir(d)) != NULL) {
    size_t len = strlen(de->d_name);
    if (len > 5 && strcmp(de->d_name + len - 5, ".json") == 0) n++;
  }
  closedir(d);
  return n;
}

static void jobs_gc(const char *jdir) {
  if (!jdir || !jdir[0]) return;
  DIR *d = opendir(jdir);
  if (!d) return;
  char ids[NG_JOBS_SCAN][32];
  int n = 0;
  struct dirent *de;
  while ((de = readdir(d)) != NULL && n < NG_JOBS_SCAN) {
    const char *name = de->d_name;
    size_t len = strlen(name);
    if (len < 6 || strcmp(name + len - 5, ".json") != 0) continue;
    size_t idlen = len - 5;
    if (idlen >= sizeof ids[0]) continue;
    int ok = 1;
    for (size_t i = 0; i < idlen; i++) {
      if (!isdigit((unsigned char)name[i])) {
        ok = 0;
        break;
      }
    }
    if (!ok) continue;
    char mpath[700];
    snprintf(mpath, sizeof mpath, "%s/%s", jdir, name);
    char *meta = ng_read_file(mpath, NULL);
    if (!meta) continue;
    char *st = ng_json_get_string(meta, "status");
    int live = (st && (!strcmp(st, "queued") || !strcmp(st, "running")));
    free(st);
    free(meta);
    if (live) continue;
    memcpy(ids[n], name, idlen);
    ids[n][idlen] = 0;
    n++;
  }
  closedir(d);
  if (n <= NG_JOBS_KEEP) return;
  /* oldest first */
  for (int i = 1; i < n; i++) {
    char tmp[32];
    memcpy(tmp, ids[i], sizeof tmp);
    int j = i;
    while (j > 0 && strcmp(ids[j - 1], tmp) > 0) {
      memcpy(ids[j], ids[j - 1], sizeof ids[j]);
      j--;
    }
    memcpy(ids[j], tmp, sizeof tmp);
  }
  int drop = n - NG_JOBS_KEEP;
  for (int i = 0; i < drop; i++) {
    char p[700];
    snprintf(p, sizeof p, "%s/%s.json", jdir, ids[i]);
    unlink(p);
    snprintf(p, sizeof p, "%s/%s.in", jdir, ids[i]);
    unlink(p);
    snprintf(p, sizeof p, "%s/%s.out", jdir, ids[i]);
    unlink(p);
  }
}

/* Residual: cool_restart / SIGTERM kills job worker children; metas stay
 * status=running|queued forever (jobs_gc never drops live). Mesh poll spins.
 * On listen only — mark orphans error so GC and dual-wire poll can finish. */
static void jobs_mark_orphans(const char *jdir) {
  if (!jdir || !jdir[0]) return;
  DIR *d = opendir(jdir);
  if (!d) return;
  struct dirent *de;
  int marked = 0;
  while ((de = readdir(d)) != NULL) {
    const char *name = de->d_name;
    size_t len = strlen(name);
    if (len < 6 || strcmp(name + len - 5, ".json") != 0) continue;
    size_t idlen = len - 5;
    if (idlen == 0 || idlen >= 32) continue;
    int digits = 1;
    for (size_t i = 0; i < idlen; i++) {
      if (!isdigit((unsigned char)name[i])) {
        digits = 0;
        break;
      }
    }
    if (!digits) continue;
    char mpath[700];
    snprintf(mpath, sizeof mpath, "%s/%s", jdir, name);
    char *meta = ng_read_file(mpath, NULL);
    if (!meta) continue;
    char *st = ng_json_get_string(meta, "status");
    int live = (st && (!strcmp(st, "queued") || !strcmp(st, "running")));
    char *kind = ng_json_get_string(meta, "kind");
    free(st);
    free(meta);
    if (!live) {
      free(kind);
      continue;
    }
    char id[32];
    memcpy(id, name, idlen);
    id[idlen] = 0;
    /* kind is machine token only (prompt|shell|watcher); never free-text. */
    const char *k = "unknown";
    if (kind && (!strcmp(kind, "prompt") || !strcmp(kind, "shell") ||
                 !strcmp(kind, "watcher")))
      k = kind;
    char *jb = NULL;
    asprintf(&jb,
      "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":false,\"action\":\"job\","
      "\"id\":\"%s\",\"status\":\"error\",\"kind\":\"%s\","
      "\"error\":\"orphan_restart\","
      NG_PEER_HTTP_DUAL_WIRE "}\n",
      id, k);
    if (jb) {
      ng_write_file(mpath, jb, strlen(jb));
      free(jb);
      marked++;
      ng_hub_event("job.orphan", "id", id, "kind", k);
    }
    free(kind);
  }
  closedir(d);
  if (marked > 0)
    ng_log("jobs: marked %d orphan running/queued after restart", marked);
}

static void send_all(int fd, const char *data, size_t n) {
  while (n) {
    ssize_t w = write(fd, data, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      return;
    }
    data += w; n -= (size_t)w;
  }
}

/* SSE chat stream helper — top-level (not nested) for portable C compilers.
 * Normal chunks → dual-wire nanobot.peer_http.v1 chat_delta plates.
 * Chunks starting with 0x1e (RS) → dual-wire chat_agent plates (tool/thinking).
 * Product bus remains SMX2; this HTTP stream is lab/ops only.
 * In-process host callbacks still see raw 0x1e JSON; only peer HTTP is plated. */
typedef struct { int fd; } chat_sse_ud;
static void chat_sse_delta(void *p, const char *chunk, size_t n) {
  chat_sse_ud *u = (chat_sse_ud *)p;
  if (!chunk || !n || !u || u->fd < 0) return;
  /* Structured agent event (tool / thinking) — dual-wire, keep type/phase leaves. */
  if ((unsigned char)chunk[0] == 0x1e && n > 1) {
    const char *body = chunk + 1;
    size_t bl = n - 1;
    while (bl > 0 &&
           (body[bl - 1] == '\n' || body[bl - 1] == '\r' || body[bl - 1] == ' ' ||
            body[bl - 1] == '\t'))
      bl--;
    char *line = NULL;
    if (bl >= 2 && body[0] == '{' && body[bl - 1] == '}') {
      /* Merge honesty into object: schema/action + original leaves + dual-wire. */
      if (asprintf(&line,
                   "event: agent\n"
                   "data: {\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,"
                   "\"action\":\"chat_agent\",%.*s,"
                   NG_PEER_HTTP_DUAL_WIRE "}\n\n",
                   (int)(bl - 2), body + 1) > 0 &&
          line) {
        send_all(u->fd, line, strlen(line));
        free(line);
      }
    } else {
      /* Non-object agent body — escape as payload string (fail-closed honesty). */
      char *tmp = (char *)malloc(bl + 1);
      char *esc = NULL;
      if (tmp) {
        memcpy(tmp, body, bl);
        tmp[bl] = 0;
        esc = ng_json_escape(tmp);
        free(tmp);
      }
      if (asprintf(&line,
                   "event: agent\n"
                   "data: {\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,"
                   "\"action\":\"chat_agent\",\"payload\":\"%s\","
                   NG_PEER_HTTP_DUAL_WIRE "}\n\n",
                   esc ? esc : "") > 0 &&
          line) {
        send_all(u->fd, line, strlen(line));
        free(line);
      }
      free(esc);
    }
    return;
  }
  char *tmp = (char *)malloc(n + 1);
  if (!tmp) return;
  memcpy(tmp, chunk, n);
  tmp[n] = 0;
  char *esc = ng_json_escape(tmp);
  free(tmp);
  if (!esc) return;
  char *line = NULL;
  if (asprintf(&line,
               "event: delta\n"
               "data: {\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,"
               "\"action\":\"chat_delta\",\"delta\":\"%s\","
               NG_PEER_HTTP_DUAL_WIRE "}\n\n",
               esc) > 0 &&
      line) {
    send_all(u->fd, line, strlen(line));
    free(line);
  }
  free(esc);
}

static void http_response(int fd, int code, const char *ctype, const char *body, size_t blen) {
  char hdr[256];
  const char *reason = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 404 ? "Not Found" : "Error";
  int n = snprintf(hdr, sizeof hdr,
    "HTTP/1.1 %d %s\r\n"
    "Content-Type: %s\r\n"
    "Content-Length: %zu\r\n"
    "Connection: close\r\n"
    "Access-Control-Allow-Origin: *\r\n"
    "Cache-Control: no-store\r\n"
    "\r\n",
    code, reason, ctype, blen);
  send_all(fd, hdr, (size_t)n);
  if (body && blen) send_all(fd, body, blen);
}

/* Prefer this for string literals — never hardcode Content-Length. */
static void http_json(int fd, int code, const char *body) {
  http_response(fd, code, "application/json", body, body ? strlen(body) : 0);
}
static void http_text(int fd, int code, const char *body) {
  http_response(fd, code, "text/plain", body, body ? strlen(body) : 0);
}

/* Residual: error plates had schema/ok/error dual-wire but no action leaf —
 * mesh could not classify fail responses like health/info/models. */
static void http_peer_err(int fd, int code, const char *error) {
  char body[400];
  const char *e = (error && error[0]) ? error : "peer_failed";
  snprintf(body, sizeof body,
           "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":false,"
           "\"action\":\"error\",\"error\":\"%s\","
           NG_PEER_HTTP_DUAL_WIRE "}",
           e);
  http_json(fd, code, body);
}

/* Same plate with one boolean flag leaf (need_peer_token / need_login). */
static void http_peer_err_flag(int fd, int code, const char *error,
                               const char *flag_key) {
  char body[440];
  const char *e = (error && error[0]) ? error : "peer_failed";
  const char *f = (flag_key && flag_key[0]) ? flag_key : "flag";
  snprintf(body, sizeof body,
           "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":false,"
           "\"action\":\"error\",\"error\":\"%s\","
           "\"%s\":true," NG_PEER_HTTP_DUAL_WIRE "}",
           e, f);
  http_json(fd, code, body);
}

static int client_is_loopback(int cfd) {
  struct sockaddr_storage ss;
  socklen_t sl = sizeof ss;
  memset(&ss, 0, sizeof ss);
  if (getpeername(cfd, (struct sockaddr *)&ss, &sl) != 0) return 0;
  if (ss.ss_family == AF_INET) {
    struct sockaddr_in *in = (struct sockaddr_in *)&ss;
    return in->sin_addr.s_addr == htonl(INADDR_LOOPBACK);
  }
  return 0;
}

/* Load expected peer token (malloc'd) or NULL if none configured.
 * Accepts KEY=val lines (token=…) or a single raw token line (legacy). */
static char *peer_token_expected(void) {
  char token_path[640];
  snprintf(token_path, sizeof token_path, "%s/peer_token", ng_workdir());
  char *need = ng_slurp_env_file(token_path, "token");
  if (!need) {
    size_t blen = 0;
    char *raw = ng_read_file(token_path, &blen);
    if (raw && raw[0]) {
      /* first non-comment line, trim */
      char *p = raw;
      while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
      if (*p != '#') {
        size_t n = strcspn(p, "\r\n");
        while (n && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
        if (n > 0) {
          need = malloc(n + 1);
          if (need) { memcpy(need, p, n); need[n] = 0; }
        }
      }
    }
    free(raw);
  }
  if (!need) need = ng_getenv_dup("NANOBOT_PEER_TOKEN");
  return need;
}

/*
 * Authorize sensitive HTTP routes.
 * Returns 1 if OK, 0 if 401 already sent (caller must free req and return).
 * allow_loopback: /api/* from 127.0.0.1 may skip token (local CLI only).
 */
static int require_peer_auth(int cfd, const char *req, int allow_loopback) {
  if (allow_loopback && client_is_loopback(cfd)) return 1;
  char *need = peer_token_expected();
  if (!need || !need[0]) {
    free(need);
    /* Fail closed on mutating routes if token missing */
    http_peer_err_flag(cfd, 503, "peer_token_not_configured", "need_peer_token");
    return 0;
  }
  int ok = 0;
  const char *h = strstr(req, "X-Nanobot-Peer-Token:");
  if (!h) h = strstr(req, "x-nanobot-peer-token:");
  if (h) {
    h = strchr(h, ':');
    if (h) {
      h++;
      while (*h == ' ' || *h == '\t') h++;
      size_t nl = strcspn(h, "\r\n");
      if (nb_ct_eq(h, nl, need, strlen(need))) ok = 1;
    }
  }
  /* Residual: mesh/HTTP clients often send only Authorization: Bearer. */
  if (!ok) {
    const char *line = NULL;
    if (!strncmp(req, "Authorization:", 14) || !strncmp(req, "authorization:", 14))
      line = req;
    else {
      const char *p = strstr(req, "\r\nAuthorization:");
      if (!p) p = strstr(req, "\r\nauthorization:");
      if (p) line = p + 2;
    }
    if (line) {
      const char *v = strchr(line, ':');
      if (v) {
        v++;
        while (*v == ' ' || *v == '\t') v++;
        if (!strncmp(v, "Bearer ", 7) || !strncmp(v, "bearer ", 7)) {
          v += 7;
          while (*v == ' ' || *v == '\t') v++;
          size_t nl = strcspn(v, "\r\n");
          if (nb_ct_eq(v, nl, need, strlen(need))) ok = 1;
        }
      }
    }
  }
  if (!ok) {
    char *body0 = strstr(req, "\r\n\r\n");
    body0 = body0 ? body0 + 4 : "";
    char *bt = ng_json_get_string(body0, "peer_token");
    if (bt && nb_ct_eq(bt, strlen(bt), need, strlen(need))) ok = 1;
    free(bt);
  }
  free(need);
  if (!ok) {
    http_peer_err_flag(cfd, 401, "peer_token_invalid", "need_peer_token");
    return 0;
  }
  return 1;
}

/* Safe static path under www_root: only relative path with no .. or weird bytes. */
static int static_path_ok(const char *rel) {
  if (!rel || rel[0] != '/') return 0;
  if (strstr(rel, "..")) return 0;
  for (const char *p = rel; *p; p++) {
    unsigned char c = (unsigned char)*p;
    if (c < 0x20 || c == 0x7f) return 0;
    if (!(isalnum(c) || c == '/' || c == '.' || c == '-' || c == '_' || c == '~'))
      return 0;
  }
  return 1;
}

/* Vision chat POSTs can be several MB (base64 JPEG). Cap is not product-specific. */
#define NG_HTTP_REQ_MAX (8 * 1024 * 1024)

static char *read_request(int fd, size_t *out_len) {
  size_t cap = 8192, len = 0;
  char *buf = malloc(cap);
  if (!buf) return NULL;
  /* Optional deadline so a stuck peer cannot pin a worker forever. */
  struct timeval tv = { .tv_sec = 120, .tv_usec = 0 };
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  while (1) {
    if (len + 4096 > cap) {
      size_t ncap = cap * 2;
      if (ncap > NG_HTTP_REQ_MAX) ncap = NG_HTTP_REQ_MAX;
      if (ncap <= cap) break; /* hit max */
      char *nbuf = realloc(buf, ncap);
      if (!nbuf) break;
      buf = nbuf;
      cap = ncap;
    }
    ssize_t r = read(fd, buf + len, cap - len - 1);
    if (r < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (r == 0) break;
    len += (size_t)r;
    buf[len] = 0;
    char *hdrend = strstr(buf, "\r\n\r\n");
    if (hdrend) {
      size_t hlen = (size_t)(hdrend + 4 - buf);
      /* Methods with no body: finish as soon as headers complete.
       * Waiting for EOF here pins workers under HTTP/1.1 keep-alive. */
      int no_body = (strncmp(buf, "GET ", 4) == 0 ||
                     strncmp(buf, "HEAD ", 5) == 0 ||
                     strncmp(buf, "OPTIONS ", 8) == 0);
      size_t cl = 0;
      int have_cl = 0;
      const char *clh = strcasestr(buf, "Content-Length:");
      if (clh) {
        cl = (size_t)strtoul(clh + 15, NULL, 10);
        have_cl = 1;
        if (cl > NG_HTTP_REQ_MAX - hlen) {
          /* Reject oversize before allocating forever */
          break;
        }
      }
      if (no_body || (have_cl && cl == 0))
        break;
      if (have_cl) {
        if (len >= hlen + cl) break;
      } else {
        /* POST/PUT without Content-Length: stop at headers (rare; body empty). */
        break;
      }
    }
  }
  buf[len] = 0;
  if (out_len) *out_len = len;
  return buf;
}

static const char *path_of(const char *req) {
  /* GET /path HTTP/1.1 */
  const char *p = strchr(req, ' ');
  if (!p) return "/";
  p++;
  return p;
}

static void handle_client(int cfd, ng_http_cfg *cfg) {
  ng_agent_cfg *agent = cfg->agent;
  ng_session *session = cfg->session;
  size_t rlen = 0;
  char *req = read_request(cfd, &rlen);
  if (!req) { close(cfd); return; }

  int is_get = strncmp(req, "GET ", 4) == 0 || strncmp(req, "HEAD ", 5) == 0;
  int is_post = strncmp(req, "POST ", 5) == 0;
  int is_put = strncmp(req, "PUT ", 4) == 0;
  int is_opts = strncmp(req, "OPTIONS ", 8) == 0;
  const char *pathstart = path_of(req);
  char path[256];
  size_t i = 0;
  while (pathstart[i] && pathstart[i] != ' ' && pathstart[i] != '?' && i + 1 < sizeof path) {
    path[i] = pathstart[i];
    i++;
  }
  path[i] = 0;

  if (is_opts) {
    http_text(cfd, 200, "");
    free(req); close(cfd); return;
  }

  /* Advance pending device login on any request.
   * Forked HTTP workers do not share memory: login_pending lives on disk
   * (device_login secret). Always call poll_login so it can load_pending
   * first — otherwise only the worker that ran /api/auth/start ever polls
   * the token and the app sees "link opened, never signed in". */
  if (session) {
    int pr = ng_session_poll_login(session);
    if (pr == 1) ng_log("auth: browser approved session");
  }

  /*
   * Optional static files from cfg->www_root (--www DIR / NANOBOT_WWW).
   * Without www_root: JSON/peer API only.
   */
  if (is_get && cfg->www_root && cfg->www_root[0]) {
    const char *rel = path;
    if (!strcmp(path, "/") || !strcmp(path, "/index.html"))
      rel = "/index.html";
    /* only serve plain static under www; API paths fall through */
    int is_static = 1;
    if (!strncmp(path, "/api/", 5) || !strncmp(path, "/peer/", 6) ||
        !strcmp(path, "/activate") ||
        !strcmp(path, "/openapi.yaml") || !strcmp(path, "/openapi.json") ||
        !strcmp(path, "/openapi") || !strcmp(path, "/favicon.ico") ||
        !strcmp(path, "/favicon"))
      is_static = 0;
    if (is_static && static_path_ok(rel)) {
      char fpath[768];
      snprintf(fpath, sizeof fpath, "%s%s", cfg->www_root, rel);
      size_t blen = 0;
      char *body = ng_read_file(fpath, &blen);
      if (body) {
        const char *ct = "application/octet-stream";
        if (strstr(rel, ".html")) ct = "text/html; charset=utf-8";
        else if (strstr(rel, ".js")) ct = "application/javascript; charset=utf-8";
        else if (strstr(rel, ".css")) ct = "text/css; charset=utf-8";
        else if (strstr(rel, ".json")) ct = "application/json";
        else if (strstr(rel, ".svg")) ct = "image/svg+xml";
        else if (strstr(rel, ".png")) ct = "image/png";
        else if (strstr(rel, ".ico")) ct = "image/x-icon";
        http_response(cfd, 200, ct, body, blen);
        free(body);
        free(req); close(cfd); return;
      }
      if (!strcmp(rel, "/index.html")) {
        /* missing assets: fall through to JSON notice */
      } else {
        http_peer_err(cfd, 404, "not_found");
        free(req); close(cfd); return;
      }
    }
  }

  if (is_get && (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)) {
    const char *dash = getenv("NANOBOT_WRAPPER_URL");
    if (!dash || !dash[0]) dash = getenv("NANOBOT_DASH_URL");
    if (dash && dash[0] && strncmp(dash, "http", 4) == 0) {
      char hdr[768];
      int n = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 302 Found\r\nLocation: %s\r\nContent-Length: 0\r\n"
        "Connection: close\r\nCache-Control: no-store\r\n\r\n", dash);
      if (n > 0 && n < (int)sizeof hdr) {
        send_all(cfd, hdr, (size_t)n);
        free(req); close(cfd); return;
      }
    }
    /* Dual-wire root plate — machine endpoints only (no free-text hint essay).
     * Residual: omitted health/ready probes while /peer/v1/info listed them. */
    const char *body =
      "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,\"action\":\"root\","
      "\"service\":\"nanobot\",\"role\":\"cli-api\","
      "\"endpoints\":["
      "\"/peer/v1/health\",\"/health\",\"/api/health\","
      "\"/ready\",\"/peer/v1/ready\",\"/api/ready\","
      "\"/peer/v1/info\",\"/peer/v1/prompt\",\"/peer/v1/shell\",\"/peer/v1/jobs\","
      "\"/peer/v1/task\",\"/peer/v1/models\",\"/api/chat\",\"/api/auth\",\"/api/task\","
      "\"/api/settings\",\"/api/models\",\"/api/braincube\",\"/api/subagents\"],"
      "\"product_wire\":\"smx2\",\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,\"llm_is_commander\":false,\"python\":0}";
    http_response(cfd, 200, "application/json", body, strlen(body));
    free(req); close(cfd); return;
  }

  /* Residual: mesh probes hit bare /api|/api/v1|/peer/v1 namespace roots and
   * got not_found while / already lists dual-wire endpoints. */
  if (is_get && (strcmp(path, "/api") == 0 || strcmp(path, "/api/") == 0 ||
                 strcmp(path, "/api/v1") == 0 || strcmp(path, "/api/v1/") == 0 ||
                 strcmp(path, "/peer/v1") == 0 || strcmp(path, "/peer/v1/") == 0)) {
    const char *ns =
      (strcmp(path, "/peer/v1") == 0 || strcmp(path, "/peer/v1/") == 0) ? "peer/v1" :
      (strcmp(path, "/api/v1") == 0 || strcmp(path, "/api/v1/") == 0) ? "api/v1" : "api";
    char body[1400];
    int n = snprintf(body, sizeof body,
      "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,\"action\":\"index\","
      "\"service\":\"nanobot-peer\",\"namespace\":\"%s\","
      "\"endpoints\":["
      "\"/peer/v1/health\",\"/health\",\"/api/health\","
      "\"/ready\",\"/peer/v1/ready\",\"/api/ready\","
      "\"/ping\",\"/api/ping\",\"/peer/v1/ping\","
      "\"/version\",\"/api/version\",\"/peer/v1/version\","
      "\"/peer/v1/info\",\"/api/info\",\"/hello\","
      "\"/peer/v1/prompt\",\"/api/prompt\",\"/peer/v1/shell\",\"/api/shell\","
      "\"/peer/v1/jobs\",\"/api/jobs\",\"/peer/v1/control\",\"/api/control\","
      "\"/peer/v1/task\",\"/api/task\",\"/peer/v1/models\",\"/api/models\","
      "\"/api/settings\",\"/settings\",\"/api/backend\",\"/peer/v1/backend\","
      "\"/api/auth\",\"/api/chat\",\"/api/braincube\",\"/api/subagents\","
      "\"/api/log\",\"/peer/v1/log\",\"/api/mcp/servers\""
      "],"
      NG_PEER_HTTP_DUAL_WIRE "}",
      ns);
    http_response(cfd, 200, "application/json", body, (size_t)n);
    free(req); close(cfd); return;
  }

  /* Residual: trailing slash 404 on /activate while bare path worked. */
  if (is_get && (strcmp(path, "/activate") == 0 || strcmp(path, "/activate/") == 0)) {
    const char *u = NULL;
    if (session) {
      if (!session->login_pending && !ng_session_valid(session))
        ng_session_start_device_login(session);
      u = session->verification_uri_complete ? session->verification_uri_complete
          : session->verification_uri;
    }
    if (u && u[0]) {
      char hdr[1024];
      int n = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 302 Found\r\nLocation: %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", u);
      send_all(cfd, hdr, (size_t)n);
    } else {
      http_peer_err(cfd, 503, "login_not_ready");
    }
    free(req); close(cfd); return;
  }

  /* Residual: trailing slash 404 on /api/auth while bare path worked (mesh probes).
   * Residual: /whoami|/api/whoami|/peer/v1/whoami not_found — identity probes. */
  if (is_get && (strcmp(path, "/api/auth") == 0 || strcmp(path, "/api/auth/") == 0 ||
                 strcmp(path, "/api/status") == 0 || strcmp(path, "/api/status/") == 0 ||
                 strcmp(path, "/peer/v1/auth") == 0 || strcmp(path, "/peer/v1/auth/") == 0 ||
                 strcmp(path, "/peer/v1/status") == 0 ||
                 strcmp(path, "/peer/v1/status/") == 0 ||
                 strcmp(path, "/whoami") == 0 || strcmp(path, "/whoami/") == 0 ||
                 strcmp(path, "/api/whoami") == 0 || strcmp(path, "/api/whoami/") == 0 ||
                 strcmp(path, "/peer/v1/whoami") == 0 ||
                 strcmp(path, "/peer/v1/whoami/") == 0)) {
    int need_browser = agent && ng_agent_needs_browser_session(agent);
    /* Soft-expired access_token still counts as signed-in after a successful refresh.
     * Skip ensure while device-login is pending — refresh cannot help and spam-logs
     * "need Connect once" on every app poll. */
    if (need_browser && session && !ng_session_valid(session)
        && !session->login_pending)
      (void)ng_session_ensure(session);
    int signed_in = need_browser ? (session && ng_session_valid(session)) : 1;
    const char *backend = agent ? ng_agent_backend_kind(agent) : "unknown";
    const char *auth_mode = need_browser ? "browser_device_code" : "local_openai_compatible";
    char *vu = NULL, *vuc = NULL, *uc = NULL;
    char *base_esc = NULL;
    if (session && need_browser) {
      if (session->verification_uri) vu = ng_json_escape(session->verification_uri);
      if (session->verification_uri_complete) vuc = ng_json_escape(session->verification_uri_complete);
      if (session->user_code) uc = ng_json_escape(session->user_code);
    }
    if (agent && agent->base_url) base_esc = ng_json_escape(agent->base_url);
    char *model_esc = ng_json_escape(agent && agent->model ? agent->model : "");
    char *wd_esc = ng_json_escape(ng_workdir());
    char *ver_esc = ng_json_escape(NG_VERSION);
    char *be_esc = ng_json_escape(backend);
    char body[3072];
    /* needs_browser = backend TYPE uses browser OAuth (always true for Grok).
     * login_required = user must complete Connect (not signed in). Clients must
     * use login_required / signed_in — not needs_browser — or they thrash Connect.
     * Dual-wire nanobot.auth.v1 — machine fields only (no free-text essays). */
    int login_required = need_browser && !signed_in;
    int is_whoami = (strcmp(path, "/whoami") == 0 || strcmp(path, "/whoami/") == 0 ||
                     strcmp(path, "/api/whoami") == 0 || strcmp(path, "/api/whoami/") == 0 ||
                     strcmp(path, "/peer/v1/whoami") == 0 ||
                     strcmp(path, "/peer/v1/whoami/") == 0);
    const char *act = is_whoami ? "whoami" : "status";
    int n = snprintf(body, sizeof body,
      "{\"schema\":\"nanobot.auth.v1\",\"ok\":true,\"action\":\"%s\","
      "\"version\":\"%s\",\"model\":\"%s\",\"signed_in\":%s,"
      "\"login_pending\":%s,\"login_required\":%s,\"user_code\":\"%s\","
      "\"verification_uri\":\"%s\",\"verification_uri_complete\":\"%s\","
      "\"workdir\":\"%s\",\"auth\":\"%s\",\"backend\":\"%s\","
      "\"base_url\":\"%s\",\"needs_browser\":%s,\"transport\":\"http\","
      NG_PEER_HTTP_DUAL_WIRE "}",
      act,
      ver_esc ? ver_esc : "",
      model_esc ? model_esc : "",
      signed_in ? "true" : "false",
      (need_browser && session && session->login_pending) ? "true" : "false",
      login_required ? "true" : "false",
      uc ? uc : "",
      vu ? vu : "",
      vuc ? vuc : "",
      wd_esc ? wd_esc : "",
      auth_mode,
      be_esc ? be_esc : "",
      base_esc ? base_esc : "",
      need_browser ? "true" : "false");
    http_response(cfd, 200, "application/json", body, (size_t)n);
    free(vu); free(vuc); free(uc); free(base_esc);
    free(model_esc); free(wd_esc); free(ver_esc); free(be_esc);
    free(req); close(cfd); return;
  }

  if (is_post && strcmp(path, "/api/auth/start") == 0) {
    if (!require_peer_auth(cfd, req, 1)) { free(req); close(cfd); return; }
    if (!session || !agent) {
      http_peer_err(cfd, 500, "no_session");
      free(req); close(cfd); return;
    }
    /* Connect Grok: ensure Grok backend, then device-code login link. */
    char *body0 = strstr(req, "\r\n\r\n");
    body0 = body0 ? body0 + 4 : "";
    int force = 0;
    if (strstr(body0, "\"force\"") && strstr(body0, "true")) force = 1;
    if (!ng_agent_is_grok_backend(agent)) {
      ng_agent_set_grok_backend(agent, NULL);
      ng_agent_save_env(agent);
      ng_log("settings: switched backend to grok for Connect");
    }
    if (force || !ng_session_valid(session)) {
      if (force) {
        ng_session_clear(session); /* also clears device_login secret file */
      } else {
        /* Resume pending device login from secure file (fork-safe). */
        ng_session_load_pending(session);
      }
      if (!session->login_pending || force) {
        if (ng_session_start_device_login(session) != 0) {
          http_peer_err(cfd, 500, "device_login_failed");
          free(req); close(cfd); return;
        }
      }
    }
    if (ng_session_valid(session)) {
      /* Dual-wire — machine action token only (no free-text "already connected"). */
      http_json(cfd, 200,
        "{\"schema\":\"nanobot.auth.v1\",\"ok\":true,\"action\":\"already_signed_in\","
        "\"signed_in\":true,\"login_pending\":false,\"backend\":\"grok\","
        "\"transport\":\"http\","
        "\"product_wire\":\"smx2\",\"peer_http\":\"lab_ops_only\","
        "\"peer_http_is_product_bus\":false,\"share\":\"state_matrix_only\","
        "\"hold_flash\":1,\"llm_is_commander\":false,\"python\":0}");
      free(req); close(cfd); return;
    }
    char *vuc = session->verification_uri_complete
      ? ng_json_escape(session->verification_uri_complete)
      : ng_json_escape(session->verification_uri ? session->verification_uri : "");
    char *vu = session->verification_uri ? ng_json_escape(session->verification_uri) : NULL;
    char *uc = ng_json_escape(session->user_code ? session->user_code : "");
    char *body = NULL;
    /* Dual-wire device-login start — no free-text coaching message. */
    asprintf(&body,
      "{\"schema\":\"nanobot.auth.v1\",\"ok\":true,\"action\":\"device_login_pending\","
      "\"signed_in\":false,\"login_pending\":true,"
      "\"backend\":\"grok\",\"needs_browser\":true,"
      "\"user_code\":\"%s\","
      "\"verification_uri\":\"%s\","
      "\"verification_uri_complete\":\"%s\","
      "\"activate_path\":\"/activate\",\"transport\":\"http\","
      NG_PEER_HTTP_DUAL_WIRE "}",
      uc ? uc : "",
      vu ? vu : "",
      vuc ? vuc : "");
    http_response(cfd, 200, "application/json", body ? body : "{}", body ? strlen(body) : 2);
    free(body); free(vuc); free(vu); free(uc);
    free(req); close(cfd); return;
  }

  /* List models: GET {base}/models (OpenAI-compatible; Grok session or local).
   * Residual: trailing slash 404 after info/jobs/control gained slash aliases. */
  if (is_get && (strcmp(path, "/api/models") == 0 || strcmp(path, "/peer/v1/models") == 0 ||
                 strcmp(path, "/api/models/") == 0 || strcmp(path, "/peer/v1/models/") == 0)) {
    if (!agent) {
      http_peer_err(cfd, 500, "no_agent");
      free(req); close(cfd); return;
    }
    if (session && ng_agent_needs_browser_session(agent) && ng_session_valid(session))
      ng_session_ensure(session);
    char *raw = ng_agent_fetch_models_json(agent);
    char *ids = ng_agent_models_ids_json(raw);
    char *cur = ng_json_escape(agent->model ? agent->model : "");
    char *base_e = ng_json_escape(agent->base_url ? agent->base_url : "");
    char *out = NULL;
    int nonempty = (ids && ids[0] == '[' && strcmp(ids, "[]") != 0);
    int ok = nonempty;
    if (!ok && raw && raw[0] == '{') {
      /* upstream returned object but no ids parsed — still ok with empty list */
      ok = 1;
    }
    /* Machine token only when list empty — never surface upstream free-text. */
    const char *hint_tok = NULL;
    if (!nonempty && raw && raw[0]) {
      if (strstr(raw, "Unauthenticated") || strstr(raw, "expired") ||
          strstr(raw, "auth"))
        hint_tok = "auth_failed";
      else if (strstr(raw, "Invalid") || strstr(raw, "error"))
        hint_tok = "upstream_error";
    }
    /* Residual: models plate omitted action leaf while health/info/jobs/task
     * all set action — mesh dual-wire could not identify the response class. */
    if (ok) {
      if (hint_tok) {
        asprintf(&out,
          "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,\"action\":\"models\","
          "\"base_url\":\"%s\",\"model\":\"%s\",\"models\":%s,\"error\":\"%s\","
          NG_PEER_HTTP_DUAL_WIRE "}",
          base_e ? base_e : "", cur ? cur : "",
          ids && ids[0] == '[' ? ids : "[]", hint_tok);
      } else {
        asprintf(&out,
          "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,\"action\":\"models\","
          "\"base_url\":\"%s\",\"model\":\"%s\",\"models\":%s,"
          NG_PEER_HTTP_DUAL_WIRE "}",
          base_e ? base_e : "", cur ? cur : "",
          ids && ids[0] == '[' ? ids : "[]");
      }
    } else {
      asprintf(&out,
        "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":false,\"action\":\"models\","
        "\"base_url\":\"%s\",\"model\":\"%s\",\"models\":[],\"error\":\"%s\","
        NG_PEER_HTTP_DUAL_WIRE "}",
        base_e ? base_e : "", cur ? cur : "",
        hint_tok ? hint_tok : "models_fetch_failed");
    }
    http_response(cfd, 200, "application/json", out ? out : "{}", out ? strlen(out) : 2);
    free(out); free(raw); free(ids); free(cur); free(base_e);
    free(req); close(cfd); return;
  }

  /* Settings: select backend (grok | local) + optional base/model.
   * Residual: root discovery listed /api/settings but only POST/PUT existed → GET not_found.
   * Residual: bare /settings 404 while /api + /peer/v1 settings plates already work. */
  if (is_get && (strcmp(path, "/api/settings") == 0 || strcmp(path, "/api/settings/") == 0 ||
                 strcmp(path, "/peer/v1/settings") == 0 ||
                 strcmp(path, "/peer/v1/settings/") == 0 ||
                 strcmp(path, "/settings") == 0 || strcmp(path, "/settings/") == 0)) {
    if (!require_peer_auth(cfd, req, 1)) { free(req); close(cfd); return; }
    if (!agent) {
      http_peer_err(cfd, 500, "no_agent");
      free(req); close(cfd); return;
    }
    if (session && ng_agent_needs_browser_session(agent) && !ng_session_valid(session)
        && !session->login_pending)
      (void)ng_session_ensure(session);
    char *be = ng_json_escape(agent->base_url ? agent->base_url : "");
    char *me = ng_json_escape(agent->model ? agent->model : "");
    char *bk = ng_json_escape(ng_agent_backend_kind(agent));
    char *sp = ng_json_escape(ng_settings_path());
    char *out = NULL;
    asprintf(&out,
      "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,\"action\":\"settings\","
      "\"backend\":\"%s\",\"base_url\":\"%s\",\"model\":\"%s\","
      "\"needs_browser\":%s,\"signed_in\":%s,"
      "\"subagents\":%s,\"subagents_max\":%d,\"llm_serial\":%s,"
      "\"settings\":\"%s\","
      NG_PEER_HTTP_DUAL_WIRE "}",
      bk ? bk : "",
      be ? be : "",
      me ? me : "",
      ng_agent_needs_browser_session(agent) ? "true" : "false",
      (ng_agent_needs_browser_session(agent) && session && ng_session_valid(session))
        ? "true" : (ng_agent_needs_browser_session(agent) ? "false" : "true"),
      ng_subagent_enabled() ? "true" : "false",
      ng_subagent_max(),
      ng_llm_sched_enabled() ? "true" : "false",
      sp ? sp : "");
    http_response(cfd, 200, "application/json", out ? out : "{}", out ? strlen(out) : 2);
    free(out); free(be); free(me); free(bk); free(sp);
    free(req); close(cfd); return;
  }

  if ((is_post || is_put) && (strcmp(path, "/api/settings") == 0 ||
                              strcmp(path, "/api/settings/") == 0 ||
                              strcmp(path, "/peer/v1/settings") == 0 ||
                              strcmp(path, "/peer/v1/settings/") == 0 ||
                              strcmp(path, "/settings") == 0 ||
                              strcmp(path, "/settings/") == 0)) {
    if (!require_peer_auth(cfd, req, 1)) { free(req); close(cfd); return; }
    if (!agent) {
      http_peer_err(cfd, 500, "no_agent");
      free(req); close(cfd); return;
    }
    char *body = strstr(req, "\r\n\r\n");
    body = body ? body + 4 : "";
    char *backend = ng_json_get_string(body, "backend");
    char *base = ng_json_get_string(body, "base_url");
    if (!base) base = ng_json_get_string(body, "base");
    char *model = ng_json_get_string(body, "model");
    /* Optional light policy keys (subagents / serial LLM) */
    char *sa = ng_json_get_string(body, "subagents");
    if (!sa) sa = ng_json_get_string(body, "SUBAGENTS");
    char *samax = ng_json_get_string(body, "subagents_max");
    if (!samax) samax = ng_json_get_string(body, "SUBAGENTS_MAX");
    char *serial = ng_json_get_string(body, "llm_serial");
    if (!serial) serial = ng_json_get_string(body, "LLM_SERIAL");
    char *maxctx = ng_json_get_string(body, "max_ctx_chars");
    if (!maxctx) maxctx = ng_json_get_string(body, "MAX_CTX_CHARS");
    int policy_only = 0;
    if (sa) { ng_settings_set("SUBAGENTS", sa); policy_only = 1; }
    if (samax) { ng_settings_set("SUBAGENTS_MAX", samax); policy_only = 1; }
    if (serial) { ng_settings_set("LLM_SERIAL", serial); policy_only = 1; }
    if (maxctx) { ng_settings_set("MAX_CTX_CHARS", maxctx); policy_only = 1; }
    free(sa); free(samax); free(serial); free(maxctx);
    if (!backend && !base && !(model && model[0])) {
      if (policy_only) {
        if (agent) ng_agent_apply_provider_policy(agent);
        free(backend); free(base); free(model);
        http_json(cfd, 200,
          "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,"
          "\"action\":\"settings_policy\","
          NG_PEER_HTTP_DUAL_WIRE "}");
        free(req); close(cfd); return;
      }
      free(backend); free(base); free(model);
      http_peer_err(cfd, 400, "need_backend_or_policy");
      free(req); close(cfd); return;
    }
    if (backend && (strcmp(backend, "grok") == 0 || strcmp(backend, "cloud") == 0)) {
      ng_agent_set_grok_backend(agent, model);
    } else if (backend && (strcmp(backend, "local") == 0 || strcmp(backend, "llama") == 0 ||
                           strcmp(backend, "offline") == 0 ||
                           strcmp(backend, "openai_compatible") == 0)) {
      ng_agent_set_local_backend(agent, base, model);
    } else if (base && base[0]) {
      /* Infer from URL */
      if (strstr(base, "grok.com") || strstr(base, "x.ai"))
        ng_agent_set_grok_backend(agent, model);
      else
        ng_agent_set_local_backend(agent, base, model);
    } else if (model && model[0]) {
      /* same backend, switch model only (after --models / UI pick) */
      ng_agent_select_model(agent, model);
    }
    ng_agent_save_env(agent);
    ng_agent_apply_provider_policy(agent);
    ng_log("settings: backend=%s base=%s model=%s",
           ng_agent_backend_kind(agent),
           agent->base_url ? agent->base_url : "?",
           agent->model ? agent->model : "?");
    char *be = ng_json_escape(agent->base_url ? agent->base_url : "");
    char *me = ng_json_escape(agent->model ? agent->model : "");
    char *bk = ng_json_escape(ng_agent_backend_kind(agent));
    char *out = NULL;
    asprintf(&out,
      "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,\"action\":\"settings\","
      "\"backend\":\"%s\",\"base_url\":\"%s\",\"model\":\"%s\","
      "\"needs_browser\":%s,\"signed_in\":%s,"
      "\"subagents\":%s,\"subagents_max\":%d,\"llm_serial\":%s,"
      NG_PEER_HTTP_DUAL_WIRE "}",
      bk ? bk : "",
      be ? be : "",
      me ? me : "",
      ng_agent_needs_browser_session(agent) ? "true" : "false",
      (ng_agent_needs_browser_session(agent) && session && ng_session_valid(session))
        ? "true" : (ng_agent_needs_browser_session(agent) ? "false" : "true"),
      ng_subagent_enabled() ? "true" : "false",
      ng_subagent_max(),
      ng_llm_sched_enabled() ? "true" : "false");
    http_response(cfd, 200, "application/json", out ? out : "{}", out ? strlen(out) : 2);
    free(out); free(be); free(me); free(bk);
    free(backend); free(base); free(model);
    free(req); close(cfd); return;
  }


  /* Residual: POST-only /api/backend → GET not_found; no slash or /peer/v1 aliases. */
  if (is_get && (strcmp(path, "/api/backend") == 0 || strcmp(path, "/api/backend/") == 0 ||
                 strcmp(path, "/peer/v1/backend") == 0 ||
                 strcmp(path, "/peer/v1/backend/") == 0)) {
    if (!require_peer_auth(cfd, req, 1)) { free(req); close(cfd); return; }
    if (!agent) {
      http_peer_err(cfd, 500, "no_agent");
      free(req); close(cfd); return;
    }
    const char *kind = ng_agent_backend_kind(agent);
    char *be = ng_json_escape(agent->base_url ? agent->base_url : "");
    char *me = ng_json_escape(agent->model ? agent->model : "");
    char *ke = ng_json_escape(kind);
    char body[900];
    int n = snprintf(body, sizeof body,
      "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,\"action\":\"backend\","
      "\"backend\":\"%s\",\"base_url\":\"%s\",\"model\":\"%s\",\"needs_browser\":%s,"
      NG_PEER_HTTP_DUAL_WIRE "}",
      ke ? ke : "", be ? be : "", me ? me : "",
      ng_agent_needs_browser_session(agent) ? "true" : "false");
    http_response(cfd, 200, "application/json", body, (size_t)n);
    free(be); free(me); free(ke);
    free(req); close(cfd); return;
  }

  if (is_post && (strcmp(path, "/api/backend") == 0 || strcmp(path, "/api/backend/") == 0 ||
                  strcmp(path, "/peer/v1/backend") == 0 ||
                  strcmp(path, "/peer/v1/backend/") == 0)) {
    if (!require_peer_auth(cfd, req, 1)) { free(req); close(cfd); return; }
    /* alias: switch backend; same as /api/settings */
    if (!agent) {
      http_peer_err(cfd, 500, "no_agent");
      free(req); close(cfd); return;
    }
    char *jsonp = strstr(req, "\r\n\r\n");
    const char *json = jsonp ? jsonp + 4 : "{}";
    char *bk = ng_json_get_string(json, "backend");
    char *base = ng_json_get_string(json, "base_url");
    char *model = ng_json_get_string(json, "model");
    if (bk && (strcmp(bk, "openai_compatible") == 0 || strcmp(bk, "llama") == 0 ||
               strcmp(bk, "local") == 0 || strcmp(bk, "offline") == 0)) {
      ng_agent_set_local_backend(agent, base ? base : NG_DEFAULT_LOCAL_BASE,
                                 model ? model : NG_DEFAULT_LOCAL_MODEL);
    } else {
      ng_agent_set_grok_backend(agent, model);
    }
    free(bk); free(base); free(model);
    ng_agent_save_env(agent);
    const char *kind = ng_agent_backend_kind(agent);
    char *be = ng_json_escape(agent->base_url ? agent->base_url : "");
    char *me = ng_json_escape(agent->model ? agent->model : "");
    char *ke = ng_json_escape(kind);
    char body[900];
    int n = snprintf(body, sizeof body,
      "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,\"action\":\"backend\","
      "\"backend\":\"%s\",\"base_url\":\"%s\",\"model\":\"%s\",\"needs_browser\":%s,"
      NG_PEER_HTTP_DUAL_WIRE "}",
      ke ? ke : "", be ? be : "", me ? me : "",
      ng_agent_needs_browser_session(agent) ? "true" : "false");
    http_response(cfd, 200, "application/json", body, (size_t)n);
    free(be); free(me); free(ke);
    free(req); close(cfd); return;
  }

  /* Residual: trailing slash + /peer/v1/log 404 while bare /api/log worked. */
  if (is_get && (strcmp(path, "/api/log") == 0 || strcmp(path, "/api/log/") == 0 ||
                 strcmp(path, "/peer/v1/log") == 0 || strcmp(path, "/peer/v1/log/") == 0)) {
    char *tail = ng_read_log_tail(16 * 1024);
    char *esc = ng_json_escape(tail ? tail : "");
    char *body = NULL;
    asprintf(&body,
             "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,\"action\":\"log\","
             "\"log\":\"%s\"," NG_PEER_HTTP_DUAL_WIRE "}",
             esc ? esc : "");
    http_response(cfd, 200, "application/json", body ? body : "{}", body ? strlen(body) : 2);
    free(tail); free(esc); free(body);
    free(req); close(cfd); return;
  }

  /* Active multi-step task board (task_plan / task_done tools).
   * Residual: trailing slash 404 after info/jobs/control gained slash aliases. */
  if (is_get && (strcmp(path, "/api/task") == 0 || strcmp(path, "/peer/v1/task") == 0 ||
                 strcmp(path, "/api/task/") == 0 || strcmp(path, "/peer/v1/task/") == 0)) {
    if (!require_peer_auth(cfd, req, 1)) { free(req); close(cfd); return; }
    char tpath[700];
    snprintf(tpath, sizeof tpath, "%s/tasks/active.json", ng_workdir());
    char *raw = ng_read_file(tpath, NULL);
    if (!raw || !raw[0]) {
      free(raw);
      http_json(cfd, 200,
        "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,\"action\":\"task\","
        "\"open\":false,\"task\":null," NG_PEER_HTTP_DUAL_WIRE "}");
    } else {
      char *body = NULL;
      asprintf(&body,
               "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,\"action\":\"task\","
               "\"open\":true,\"task\":%s," NG_PEER_HTTP_DUAL_WIRE "}",
               raw);
      http_response(cfd, 200, "application/json", body ? body : "{}", body ? strlen(body) : 2);
      free(body);
      free(raw);
    }
    free(req); close(cfd); return;
  }

  /* ---- Outbound MCP servers (agent connects TO remote MCPs) ----
   * Residual: trailing slash 404 on /api|/peer/v1/mcp/servers while bare worked. */
  if (is_get && (strcmp(path, "/api/mcp/servers") == 0 ||
                 strcmp(path, "/api/mcp/servers/") == 0 ||
                 strcmp(path, "/peer/v1/mcp/servers") == 0 ||
                 strcmp(path, "/peer/v1/mcp/servers/") == 0)) {
    if (!require_peer_auth(cfd, req, 1)) { free(req); close(cfd); return; }
    char *body = ng_mcp_servers_list_json();
    http_response(cfd, 200, "application/json", body ? body : "{}", body ? strlen(body) : 2);
    free(body);
    free(req); close(cfd); return;
  }
  if (is_post && (strcmp(path, "/api/mcp/servers") == 0 ||
                  strcmp(path, "/api/mcp/servers/") == 0 ||
                  strcmp(path, "/peer/v1/mcp/servers") == 0 ||
                  strcmp(path, "/peer/v1/mcp/servers/") == 0)) {
    if (!require_peer_auth(cfd, req, 1)) { free(req); close(cfd); return; }
    char *body = strstr(req, "\r\n\r\n");
    body = body ? body + 4 : "";
    int rc = ng_mcp_servers_save_raw(body);
    if (rc != 0) {
      http_peer_err(cfd, 400, "invalid_mcp_servers");
    } else {
      char *list = ng_mcp_servers_list_json();
      http_response(cfd, 200, "application/json", list ? list : "{\"ok\":true}",
                    list ? strlen(list) : 11);
      free(list);
    }
    free(req); close(cfd); return;
  }
  /* Residual: trailing slash 404 on mcp/probe after servers gained slash. */
  if (is_post && (strcmp(path, "/api/mcp/probe") == 0 ||
                  strcmp(path, "/api/mcp/probe/") == 0 ||
                  strcmp(path, "/peer/v1/mcp/probe") == 0 ||
                  strcmp(path, "/peer/v1/mcp/probe/") == 0)) {
    if (!require_peer_auth(cfd, req, 1)) { free(req); close(cfd); return; }
    char *body = strstr(req, "\r\n\r\n");
    body = body ? body + 4 : "";
    char *id = ng_json_get_string(body, "id");
    char *url = ng_json_get_string(body, "url");
    char *auth = ng_json_get_string(body, "auth");
    char *out = ng_mcp_server_probe(id, url, auth);
    free(id); free(url); free(auth);
    http_response(cfd, 200, "application/json", out ? out : "{}", out ? strlen(out) : 2);
    free(out);
    free(req); close(cfd); return;
  }

  if (is_post && strcmp(path, "/api/chat") == 0) {
    /* LAN: require peer token; localhost may skip (local tools). */
    if (!require_peer_auth(cfd, req, 1)) { free(req); close(cfd); return; }
    char *body = strstr(req, "\r\n\r\n");
    body = body ? body + 4 : "";
    char *prompt = ng_json_get_string(body, "prompt");
    if (!prompt) prompt = ng_json_get_string(body, "q");
    if (!prompt) prompt = ng_json_get_string(body, "message");
    char *image_b64 = ng_json_get_string(body, "image_base64");
    if (!image_b64) image_b64 = ng_json_get_string(body, "image");
    char *image_mime = ng_json_get_string(body, "image_mime");
    if (!image_mime) image_mime = ng_json_get_string(body, "mime");
    /* Optional multi-image array: "images":[{base64,mime},...] — extract raw slice */
    char *images_json = NULL;
    {
      const char *ik = strstr(body, "\"images\"");
      if (ik) {
        const char *lb = strchr(ik, '[');
        if (lb) {
          int depth = 0;
          const char *p = lb;
          for (; *p; p++) {
            if (*p == '[') depth++;
            else if (*p == ']') {
              depth--;
              if (depth == 0) {
                size_t n = (size_t)(p - lb + 1);
                if (n < 4 * 1024 * 1024) {
                  images_json = malloc(n + 1);
                  if (images_json) {
                    memcpy(images_json, lb, n);
                    images_json[n] = 0;
                  }
                }
                break;
              }
            } else if (*p == '"') {
              p++;
              while (*p && *p != '"') {
                if (*p == '\\' && p[1]) p += 2;
                else p++;
              }
            }
          }
        }
      }
    }
    int has_img = (image_b64 && image_b64[0])
               || (images_json && strstr(images_json, "base64"));
    /* Residual: whitespace-only prompt skipped [0] check and burned agent turns
     * (peer /peer/v1/prompt already trims → missing_prompt). */
    if (prompt) {
      const char *p = prompt;
      while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
      if (!*p) {
        free(prompt);
        prompt = NULL;
      }
    }
    if (!prompt && !has_img) {
      free(prompt); free(image_b64); free(image_mime); free(images_json);
      http_peer_err(cfd, 400, "missing_prompt_or_image");
      free(req); close(cfd); return;
    }
    if (!prompt) prompt = strdup("");
    /* Outer shell always: @! shell. Local/llama backend: no browser session. */
    int shell_only = (prompt[0] == '@' && prompt[1] == '!' && !has_img);
    int need_browser = agent && ng_agent_needs_browser_session(agent);
    /* Residual: soft-expired access failed login gate without ensure (info/prompt fixed). */
    if (!shell_only && need_browser && session && !ng_session_valid(session)
        && !session->login_pending)
      (void)ng_session_ensure(session);
    if (!shell_only && need_browser && (!session || !ng_session_valid(session))) {
      if (session && session->login_pending) ng_session_poll_login(session);
      if (!session || !ng_session_valid(session)) {
        free(prompt); free(image_b64); free(image_mime); free(images_json);
        http_peer_err_flag(cfd, 401, "need_login", "need_login");
        free(req); close(cfd); return;
      }
    }
    /* Real-time typing: stream=true → dual-wire SSE chat_delta plates. */
    int want_stream = (strstr(body, "\"stream\":true") != NULL)
                   || (strstr(body, "\"stream\": true") != NULL);
    if (want_stream && !shell_only) {
      char hdr[384];
      int hn = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "X-Grokium-Product-Wire: smx2\r\n"
        "X-Grokium-Peer-HTTP: lab_ops_only\r\n"
        "X-Grokium-Share: state_matrix_only\r\n"
        "\r\n");
      send_all(cfd, hdr, (size_t)hn);
      /* Dual-wire stream open — no free-text SSE comment. */
      {
        static const char open_evt[] =
          "event: open\n"
          "data: {\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,"
          "\"action\":\"chat_stream_open\","
          NG_PEER_HTTP_DUAL_WIRE "}\n\n";
        send_all(cfd, open_evt, sizeof open_evt - 1);
      }
      chat_sse_ud ud = { .fd = cfd };
      char *reply = ng_agent_run_attachments(agent, prompt, image_b64, image_mime,
                                             images_json, 1, chat_sse_delta, &ud);
      char *esc = ng_json_escape(reply ? reply : "");
      char *fin = NULL;
      if (asprintf(&fin,
                   "event: done\n"
                   "data: {\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,"
                   "\"action\":\"chat\",\"done\":true,\"reply\":\"%s\","
                   NG_PEER_HTTP_DUAL_WIRE "}\n\n",
                   esc ? esc : "") > 0 && fin) {
        send_all(cfd, fin, strlen(fin));
        free(fin);
      }
      free(prompt); free(image_b64); free(image_mime); free(images_json);
      free(reply); free(esc);
      free(req); close(cfd); return;
    }
    char *reply = ng_agent_run_attachments(agent, prompt, image_b64, image_mime,
                                           images_json, 0, NULL, NULL);
    char *esc = ng_json_escape(reply ? reply : "");
    char *jb = NULL;
    asprintf(&jb,
             "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,\"action\":\"chat\","
             "\"reply\":\"%s\"," NG_PEER_HTTP_DUAL_WIRE "}",
             esc ? esc : "");
    http_response(cfd, 200, "application/json", jb ? jb : "{}", jb ? strlen(jb) : 2);
    free(prompt); free(image_b64); free(image_mime); free(images_json);
    free(reply); free(esc); free(jb);
    free(req); close(cfd); return;
  }


  /* Residual: trailing slash + /api/resources 404 while /peer/v1/resources worked.
   * Residual: /metrics|/api/metrics|/peer/v1/metrics not_found — same host plate,
   * action=metrics for mesh Prometheus-ish probes. */
  if (is_get && (strcmp(path, "/api/v1/resources") == 0 ||
                 strcmp(path, "/api/v1/resources/") == 0 ||
                 strcmp(path, "/api/resources") == 0 ||
                 strcmp(path, "/api/resources/") == 0 ||
                 strcmp(path, "/peer/v1/resources") == 0 ||
                 strcmp(path, "/peer/v1/resources/") == 0 ||
                 strcmp(path, "/metrics") == 0 || strcmp(path, "/metrics/") == 0 ||
                 strcmp(path, "/api/metrics") == 0 || strcmp(path, "/api/metrics/") == 0 ||
                 strcmp(path, "/peer/v1/metrics") == 0 ||
                 strcmp(path, "/peer/v1/metrics/") == 0)) {
    char *body = ng_resources_json();
    int is_metrics = (strcmp(path, "/metrics") == 0 || strcmp(path, "/metrics/") == 0 ||
                      strcmp(path, "/api/metrics") == 0 ||
                      strcmp(path, "/api/metrics/") == 0 ||
                      strcmp(path, "/peer/v1/metrics") == 0 ||
                      strcmp(path, "/peer/v1/metrics/") == 0);
    if (is_metrics && body) {
      /* resources=20 chars action leaf; metrics=18 — shrink in place */
      char *p = strstr(body, "\"action\":\"resources\"");
      if (p) {
        memcpy(p, "\"action\":\"metrics\"", 18);
        memmove(p + 18, p + 20, strlen(p + 20) + 1);
      }
    }
    http_response(cfd, 200, "application/json", body ? body : "{}", body ? strlen(body) : 2);
    free(body);
    free(req); close(cfd); return;
  }

  /* ---- Peer bus for other agents / sessions (lab/ops; product bus = SMX2) ---- */
  /* /health + /ready + /ping: mesh focus loops probe these; same plate, distinct action.
   * Residual: /ready reused action=health → probes could not tell paths apart.
   * Residual: /peer/v1/ready was 404 while bare /ready worked — peer-namespace alias.
   * Residual: /api/health|/api/ready 404 — API-namespace clients (OpenAPI-ish).
   * Residual: trailing slash on health/ready 404 after resources/models gained slash.
   * Residual: /ping|/api/ping|/peer/v1/ping not_found while health already lives. */
  if (is_get && (strcmp(path, "/peer/v1/health") == 0 ||
                 strcmp(path, "/peer/v1/health/") == 0 ||
                 strcmp(path, "/health") == 0 ||
                 strcmp(path, "/health/") == 0 ||
                 strcmp(path, "/api/health") == 0 ||
                 strcmp(path, "/api/health/") == 0 ||
                 strcmp(path, "/ready") == 0 ||
                 strcmp(path, "/ready/") == 0 ||
                 strcmp(path, "/peer/v1/ready") == 0 ||
                 strcmp(path, "/peer/v1/ready/") == 0 ||
                 strcmp(path, "/api/ready") == 0 ||
                 strcmp(path, "/api/ready/") == 0 ||
                 strcmp(path, "/ping") == 0 ||
                 strcmp(path, "/ping/") == 0 ||
                 strcmp(path, "/api/ping") == 0 ||
                 strcmp(path, "/api/ping/") == 0 ||
                 strcmp(path, "/peer/v1/ping") == 0 ||
                 strcmp(path, "/peer/v1/ping/") == 0)) {
    char body[640];
    char *ver = ng_json_escape(NG_VERSION);
    int jn = jobs_meta_count();
    int is_ready = (strcmp(path, "/ready") == 0 ||
                    strcmp(path, "/ready/") == 0 ||
                    strcmp(path, "/peer/v1/ready") == 0 ||
                    strcmp(path, "/peer/v1/ready/") == 0 ||
                    strcmp(path, "/api/ready") == 0 ||
                    strcmp(path, "/api/ready/") == 0);
    int is_ping = (strcmp(path, "/ping") == 0 ||
                   strcmp(path, "/ping/") == 0 ||
                   strcmp(path, "/api/ping") == 0 ||
                   strcmp(path, "/api/ping/") == 0 ||
                   strcmp(path, "/peer/v1/ping") == 0 ||
                   strcmp(path, "/peer/v1/ping/") == 0);
    const char *act = is_ready ? "ready" : (is_ping ? "ping" : "health");
    int n = snprintf(body, sizeof body,
      "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,\"action\":\"%s\","
      "\"service\":\"nanobot-peer\",\"version\":\"%s\",\"role\":\"session-bus\","
      "\"pid\":%d,\"started\":%ld,\"jobs\":%d,\"jobs_keep\":%d,"
      NG_PEER_HTTP_DUAL_WIRE "}",
      act,
      ver ? ver : "",
      (int)(g_serve_pid ? g_serve_pid : getpid()),
      (long)g_serve_started,
      jn, (int)NG_JOBS_KEEP);
    free(ver);
    http_response(cfd, 200, "application/json", body, (size_t)n);
    free(req); close(cfd); return;
  }

  /* Residual: mesh OpenAPI-ish probes hit /version|/api/version|/peer/v1/version
   * and got not_found while health/info already expose NG_VERSION. */
  if (is_get && (strcmp(path, "/version") == 0 || strcmp(path, "/version/") == 0 ||
                 strcmp(path, "/api/version") == 0 || strcmp(path, "/api/version/") == 0 ||
                 strcmp(path, "/peer/v1/version") == 0 ||
                 strcmp(path, "/peer/v1/version/") == 0)) {
    char body[512];
    char *ver = ng_json_escape(NG_VERSION);
    int n = snprintf(body, sizeof body,
      "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,\"action\":\"version\","
      "\"service\":\"nanobot-peer\",\"version\":\"%s\","
      NG_PEER_HTTP_DUAL_WIRE "}",
      ver ? ver : "");
    free(ver);
    http_response(cfd, 200, "application/json", body, (size_t)n);
    free(req); close(cfd); return;
  }

  /* Residual: mesh capability probes hit /capabilities|/api/capabilities|
   * /peer/v1/capabilities and got not_found while info/tools already exist. */
  if (is_get && (strcmp(path, "/capabilities") == 0 || strcmp(path, "/capabilities/") == 0 ||
                 strcmp(path, "/api/capabilities") == 0 ||
                 strcmp(path, "/api/capabilities/") == 0 ||
                 strcmp(path, "/peer/v1/capabilities") == 0 ||
                 strcmp(path, "/peer/v1/capabilities/") == 0)) {
    char body[1400];
    char *ver = ng_json_escape(NG_VERSION);
    int n = snprintf(body, sizeof body,
      "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,\"action\":\"capabilities\","
      "\"service\":\"nanobot-peer\",\"version\":\"%s\","
      "\"tools\":[\"prompt\",\"shell\"],"
      "\"methods\":[\"GET\",\"POST\",\"OPTIONS\"],"
      "\"features\":{"
        "\"peer_http\":true,"
        "\"async_jobs\":true,"
        "\"shell\":true,"
        "\"prompt\":true,"
        "\"openapi\":true,"
        "\"metrics\":true,"
        "\"whoami\":true,"
        "\"braincube\":true"
      "},"
      "\"discovery\":["
        "\"/peer/v1/health\",\"/peer/v1/info\",\"/openapi.json\","
        "\"/version\",\"/metrics\",\"/whoami\",\"/capabilities\""
      "],"
      NG_PEER_HTTP_DUAL_WIRE "}",
      ver ? ver : "");
    free(ver);
    http_response(cfd, 200, "application/json", body, (size_t)n);
    free(req); close(cfd); return;
  }

  /* Residual: mesh/browser probes hit /favicon.ico and got not_found (noisy 404
   * without www_root). Serve a tiny cyan-cube SVG so probes stay quiet. */
  if (is_get && (strcmp(path, "/favicon.ico") == 0 || strcmp(path, "/favicon.ico/") == 0 ||
                 strcmp(path, "/favicon") == 0 || strcmp(path, "/favicon/") == 0)) {
    static const char favicon_svg[] =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 32 32\">"
      "<rect width=\"32\" height=\"32\" fill=\"#0a0a0a\"/>"
      "<rect x=\"6\" y=\"6\" width=\"20\" height=\"20\" fill=\"none\" "
      "stroke=\"#00e5ff\" stroke-width=\"2\"/>"
      "</svg>";
    http_response(cfd, 200, "image/svg+xml", favicon_svg, sizeof favicon_svg - 1);
    free(req); close(cfd); return;
  }

  /* Residual: mesh OpenAPI probes hit /openapi.json|/openapi.yaml (and dual-wire
   * aliases) while only static www exclusion reserved the path — always 404. */
  if (is_get && (strcmp(path, "/openapi.json") == 0 || strcmp(path, "/openapi.json/") == 0 ||
                 strcmp(path, "/openapi") == 0 || strcmp(path, "/openapi/") == 0 ||
                 strcmp(path, "/api/openapi.json") == 0 ||
                 strcmp(path, "/api/openapi.json/") == 0 ||
                 strcmp(path, "/api/openapi") == 0 || strcmp(path, "/api/openapi/") == 0 ||
                 strcmp(path, "/peer/v1/openapi.json") == 0 ||
                 strcmp(path, "/peer/v1/openapi.json/") == 0 ||
                 strcmp(path, "/peer/v1/openapi") == 0 ||
                 strcmp(path, "/peer/v1/openapi/") == 0 ||
                 strcmp(path, "/openapi.yaml") == 0 || strcmp(path, "/openapi.yaml/") == 0 ||
                 strcmp(path, "/api/openapi.yaml") == 0 ||
                 strcmp(path, "/api/openapi.yaml/") == 0 ||
                 strcmp(path, "/peer/v1/openapi.yaml") == 0 ||
                 strcmp(path, "/peer/v1/openapi.yaml/") == 0)) {
    int want_yaml =
      (strcmp(path, "/openapi.yaml") == 0 || strcmp(path, "/openapi.yaml/") == 0 ||
       strcmp(path, "/api/openapi.yaml") == 0 || strcmp(path, "/api/openapi.yaml/") == 0 ||
       strcmp(path, "/peer/v1/openapi.yaml") == 0 ||
       strcmp(path, "/peer/v1/openapi.yaml/") == 0);
    char *ver = ng_json_escape(NG_VERSION);
    char body[2800];
    int n;
    if (want_yaml) {
      n = snprintf(body, sizeof body,
        "openapi: 3.0.3\n"
        "info:\n"
        "  title: nanobot peer HTTP\n"
        "  version: \"%s\"\n"
        "  description: Lab ops peer bus (not product SMX2).\n"
        "paths:\n"
        "  /peer/v1/health:\n"
        "    get: {summary: health}\n"
        "  /health:\n"
        "    get: {summary: health alias}\n"
        "  /api/health:\n"
        "    get: {summary: health alias}\n"
        "  /ready:\n"
        "    get: {summary: ready}\n"
        "  /peer/v1/info:\n"
        "    get: {summary: info}\n"
        "  /peer/v1/prompt:\n"
        "    post: {summary: prompt job}\n"
        "  /peer/v1/shell:\n"
        "    post: {summary: shell job}\n"
        "  /peer/v1/jobs:\n"
        "    get: {summary: jobs index}\n"
        "  /peer/v1/control:\n"
        "    get: {summary: control}\n"
        "  /version:\n"
        "    get: {summary: version}\n"
        "  /metrics:\n"
        "    get: {summary: host metrics}\n"
        "  /whoami:\n"
        "    get: {summary: auth whoami}\n"
        "  /openapi.json:\n"
        "    get: {summary: this document (JSON)}\n"
        "  /favicon.ico:\n"
        "    get: {summary: favicon SVG}\n",
        ver ? ver : "");
      free(ver);
      http_response(cfd, 200, "application/yaml", body, (size_t)n);
    } else {
      n = snprintf(body, sizeof body,
        "{"
        "\"openapi\":\"3.0.3\","
        "\"info\":{"
          "\"title\":\"nanobot peer HTTP\","
          "\"version\":\"%s\","
          "\"description\":\"Lab ops peer bus (not product SMX2).\""
        "},"
        "\"paths\":{"
          "\"/peer/v1/health\":{\"get\":{\"summary\":\"health\","
            "\"responses\":{\"200\":{\"description\":\"ok\"}}}},"
          "\"/health\":{\"get\":{\"summary\":\"health alias\","
            "\"responses\":{\"200\":{\"description\":\"ok\"}}}},"
          "\"/api/health\":{\"get\":{\"summary\":\"health alias\","
            "\"responses\":{\"200\":{\"description\":\"ok\"}}}},"
          "\"/ready\":{\"get\":{\"summary\":\"ready\","
            "\"responses\":{\"200\":{\"description\":\"ok\"}}}},"
          "\"/peer/v1/info\":{\"get\":{\"summary\":\"info\","
            "\"responses\":{\"200\":{\"description\":\"ok\"}}}},"
          "\"/peer/v1/prompt\":{\"post\":{\"summary\":\"prompt job\","
            "\"responses\":{\"200\":{\"description\":\"ok\"}}}},"
          "\"/peer/v1/shell\":{\"post\":{\"summary\":\"shell job\","
            "\"responses\":{\"200\":{\"description\":\"ok\"}}}},"
          "\"/peer/v1/jobs\":{\"get\":{\"summary\":\"jobs index\","
            "\"responses\":{\"200\":{\"description\":\"ok\"}}}},"
          "\"/peer/v1/control\":{\"get\":{\"summary\":\"control\","
            "\"responses\":{\"200\":{\"description\":\"ok\"}}}},"
          "\"/version\":{\"get\":{\"summary\":\"version\","
            "\"responses\":{\"200\":{\"description\":\"ok\"}}}},"
          "\"/metrics\":{\"get\":{\"summary\":\"host metrics\","
            "\"responses\":{\"200\":{\"description\":\"ok\"}}}},"
          "\"/whoami\":{\"get\":{\"summary\":\"auth whoami\","
            "\"responses\":{\"200\":{\"description\":\"ok\"}}}},"
          "\"/openapi.json\":{\"get\":{\"summary\":\"this document\","
            "\"responses\":{\"200\":{\"description\":\"ok\"}}}},"
          "\"/favicon.ico\":{\"get\":{\"summary\":\"favicon SVG\","
            "\"responses\":{\"200\":{\"description\":\"ok\"}}}}"
        "},"
        "\"x-nanobot\":{"
          "\"schema\":\"nanobot.peer_http.v1\","
          "\"action\":\"openapi\","
          "\"product_wire\":\"smx2\","
          "\"peer_http\":\"lab_ops_only\","
          "\"peer_http_is_product_bus\":false,"
          "\"share\":\"state_matrix_only\","
          "\"hold_flash\":1,"
          "\"llm_is_commander\":false,"
          "\"python\":0"
        "}"
        "}",
        ver ? ver : "");
      free(ver);
      http_response(cfd, 200, "application/json", body, (size_t)n);
    }
    free(req); close(cfd); return;
  }

  /* Residual: /api/info and trailing slash 404 while health/jobs already
   * accept /api + slash aliases — mesh OpenAPI-ish probes hit not_found.
   * Residual: bare /hello 404 while /api/hello and /peer/v1/hello already info. */
  if (is_get && (strcmp(path, "/peer/v1/info") == 0 || strcmp(path, "/peer/v1/hello") == 0 ||
                 strcmp(path, "/peer/v1/info/") == 0 || strcmp(path, "/peer/v1/hello/") == 0 ||
                 strcmp(path, "/api/info") == 0 || strcmp(path, "/api/info/") == 0 ||
                 strcmp(path, "/api/hello") == 0 || strcmp(path, "/api/hello/") == 0 ||
                 strcmp(path, "/hello") == 0 || strcmp(path, "/hello/") == 0)) {
    /* Residual: info used raw ng_session_valid without ensure — soft-expired
     * access_token reported signed_in=false while /api/auth ensure → true. */
    if (session && agent && ng_agent_needs_browser_session(agent)
        && !ng_session_valid(session) && !session->login_pending)
      (void)ng_session_ensure(session);
    int signed_in = session && ng_session_valid(session);
    /* Room for expanded dual-wire endpoints list (/api/* + control). */
    char body[1536];
    char *ver = ng_json_escape(NG_VERSION);
    char *md = ng_json_escape(agent && agent->model ? agent->model : "");
    char *wd = ng_json_escape(ng_workdir());
    int jn = jobs_meta_count();
    int n = snprintf(body, sizeof body,
      "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,\"action\":\"info\","
      "\"service\":\"nanobot-peer\",\"version\":\"%s\","
      "\"signed_in\":%s,\"model\":\"%s\",\"workdir\":\"%s\","
      "\"pid\":%d,\"started\":%ld,\"jobs\":%d,\"jobs_keep\":%d,"
      "\"tools\":[\"prompt\",\"shell\"],"
      "\"endpoints\":["
      "\"/peer/v1/health\","
      "\"/health\","
      "\"/api/health\","
      "\"/ready\","
      "\"/peer/v1/ready\","
      "\"/api/ready\","
      "\"/peer/v1/info\","
      "\"/api/info\","
      "\"/peer/v1/prompt\","
      "\"/api/prompt\","
      "\"/peer/v1/shell\","
      "\"/api/shell\","
      "\"/peer/v1/jobs\","
      "\"/api/jobs\","
      "\"/peer/v1/control\","
      "\"/api/control\""
      "],"
      NG_PEER_HTTP_DUAL_WIRE "}",
      ver ? ver : "",
      signed_in ? "true" : "false",
      md ? md : "",
      wd ? wd : "",
      (int)(g_serve_pid ? g_serve_pid : getpid()),
      (long)g_serve_started,
      jn, (int)NG_JOBS_KEEP);
    free(ver); free(md); free(wd);
    if (n < 0 || n >= (int)sizeof body) {
      http_peer_err(cfd, 500, "oom");
      free(req); close(cfd); return;
    }
    http_response(cfd, 200, "application/json", body, (size_t)n);
    free(req); close(cfd); return;
  }

  /* Control plane: shell / watcher / ui — persisted in $HOME/settings.
   * Residual: /api/control and trailing slash 404 (jobs/health already aliased). */
  if (is_get && (strcmp(path, "/peer/v1/control") == 0 || strcmp(path, "/peer/v1/control/") == 0 ||
                 strcmp(path, "/api/control") == 0 || strcmp(path, "/api/control/") == 0)) {
    char sp[640], wp[640];
    snprintf(sp, sizeof sp, "%s/shell_enabled", ng_workdir());
    snprintf(wp, sizeof wp, "%s/watcher_enabled", ng_workdir());
    int shell_on = 1, watch_on = 0, ui_on = 0;
    {
      FILE *f = fopen(sp, "r");
      if (f) {
        char b[16] = {0};
        if (fgets(b, sizeof b, f) && (b[0] == '0' || !strncasecmp(b, "off", 3))) shell_on = 0;
        fclose(f);
      }
    }
    {
      FILE *f = fopen(wp, "r");
      if (f) {
        char b[16] = {0};
        if (fgets(b, sizeof b, f) && (b[0] == '1' || !strncasecmp(b, "on", 2))) watch_on = 1;
        fclose(f);
      }
    }
    {
      char *u = ng_settings_get("UI");
      if (u && (!strcasecmp(u, "on") || !strcmp(u, "1") || !strcasecmp(u, "true"))) ui_on = 1;
      free(u);
      if (!ui_on && cfg->www_root && cfg->www_root[0]) ui_on = 1;
    }
    char *www = ng_settings_get("WWW");
    const char *www_raw = (www && www[0]) ? www
                           : (cfg->www_root && cfg->www_root[0]) ? cfg->www_root
                                                                : "";
    char *www_esc = ng_json_escape(www_raw);
    char *sp_esc = ng_json_escape(ng_settings_path());
    char *jb = NULL;
    /* Dual-wire control plate — no free-text note essay; paths inject-sanitized. */
    asprintf(&jb,
             "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,"
             "\"action\":\"control\","
             "\"shell_enabled\":%s,\"watcher_enabled\":%s,\"ui_enabled\":%s,"
             "\"www\":\"%s\",\"settings\":\"%s\",\"status\":\"%s\","
             "\"persist\":true," NG_PEER_HTTP_DUAL_WIRE "}",
             shell_on ? "true" : "false", watch_on ? "true" : "false",
             ui_on ? "true" : "false", www_esc ? www_esc : "",
             sp_esc ? sp_esc : "",
             (session && ng_session_valid(session)) ? "online" : "offline");
    http_response(cfd, 200, "application/json", jb ? jb : "{}",
                  jb ? strlen(jb) : 2);
    free(www);
    free(www_esc);
    free(sp_esc);
    free(jb);
    free(req); close(cfd); return;
  }

  if (is_post && (strcmp(path, "/peer/v1/control") == 0 || strcmp(path, "/peer/v1/control/") == 0 ||
                  strcmp(path, "/api/control") == 0 || strcmp(path, "/api/control/") == 0)) {
    if (!require_peer_auth(cfd, req, 0)) { free(req); close(cfd); return; }
    char *body = strstr(req, "\r\n\r\n");
    body = body ? body + 4 : "";
    char *svc = ng_json_get_string(body, "service");
    if (!svc) svc = ng_json_get_string(body, "name");
    char *act = ng_json_get_string(body, "action");
    int en = -1;
    {
      const char *e = strstr(body, "\"enabled\"");
      if (e) {
        e = strchr(e, ':');
        if (e) {
          while (*e && (*e == ':' || *e == ' ')) e++;
          if (!strncmp(e, "true", 4) || *e == '1') en = 1;
          else if (!strncmp(e, "false", 5) || *e == '0') en = 0;
        }
      }
    }
    if (!act && en >= 0) act = strdup(en ? "on" : "off");
    /* Residual: whitespace service/action passed [0] checks; action "  " was
     * treated as off and flipped shell/watcher (not on|enable|off|disable). */
    if (svc) {
      const char *p = svc;
      while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
      if (!*p) {
        free(svc);
        svc = NULL;
      } else if (p != svc) {
        char *t = strdup(p);
        free(svc);
        svc = t;
      }
    }
    if (act) {
      const char *p = act;
      while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
      if (!*p) {
        free(act);
        act = NULL;
      } else if (p != act) {
        char *t = strdup(p);
        free(act);
        act = t;
      }
    }
    if (!svc || !act) {
      free(svc); free(act);
      http_peer_err(cfd, 400, "need_service_action");
      free(req); close(cfd); return;
    }
    /* Residual: service "Shell" / action "ON" rejected while job kind
     * case-folds (4ed64ef); mesh control posts mixed-case tokens. */
    for (char *p = svc; *p; p++) *p = (char)tolower((unsigned char)*p);
    for (char *p = act; *p; p++) *p = (char)tolower((unsigned char)*p);
    char pathf[640];
    int ok = 0;
    int on = (!strcmp(act, "on") || !strcmp(act, "enable"));
    int off = (!strcmp(act, "off") || !strcmp(act, "disable"));
    if (!on && !off) {
      free(svc); free(act);
      http_peer_err(cfd, 400, "need_service_action");
      free(req); close(cfd); return;
    }
    /* Canonical action leaf for dual-wire plate. */
    const char *act_canon = on ? "on" : "off";
    if (!strcmp(svc, "shell")) {
      snprintf(pathf, sizeof pathf, "%s/shell_enabled", ng_workdir());
      const char *v = on ? "1" : "0";
      ok = ng_write_file(pathf, v, strlen(v)) == 0;
      ng_settings_set("SHELL", on ? "on" : "off");
    } else if (!strcmp(svc, "watcher")) {
      snprintf(pathf, sizeof pathf, "%s/watcher_enabled", ng_workdir());
      const char *v = on ? "1" : "0";
      ok = ng_write_file(pathf, v, strlen(v)) == 0;
      ng_settings_set("WATCHER", on ? "on" : "off");
    } else if (!strcmp(svc, "ui") || !strcmp(svc, "www") || !strcmp(svc, "web")) {
      /* Persist optional static-root preference; applies on next start if --www not set. */
      ok = ng_settings_set("UI", on ? "on" : "off") == 0;
      if (on) {
        char *w = ng_settings_get("WWW");
        if (!w || !w[0]) {
          if (cfg->www_root && cfg->www_root[0]) {
            ng_settings_set("WWW", cfg->www_root);
          } else {
            char defw[700];
            snprintf(defw, sizeof defw, "%s/www", ng_workdir());
            ng_settings_set("WWW", defw);
          }
        }
        free(w);
      }
    } else {
      free(svc); free(act);
      http_peer_err(cfd, 400, "unknown_service");
      free(req); close(cfd); return;
    }
    char *jb = NULL;
    char *sp = ng_json_escape(ng_settings_path());
    asprintf(&jb,
      "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":%s,"
      "\"service\":\"%s\",\"action\":\"%s\",\"persist\":true,"
      "\"settings\":\"%s\","
      NG_PEER_HTTP_DUAL_WIRE "}",
      ok ? "true" : "false", svc, act_canon, sp ? sp : "");
    free(sp);
    http_response(cfd, ok ? 200 : 400, "application/json", jb ? jb : "{}", jb ? strlen(jb) : 2);
    free(svc); free(act); free(jb);
    free(req); close(cfd); return;
  }

  /* Async jobs: accept work immediately, poll result — keeps peer responsive.
   * Residual: POST /peer/v1/jobs/ (trailing slash) and /api/jobs fell through
   * to not_found while collection without slash worked. */
  if (is_post && (strcmp(path, "/peer/v1/jobs") == 0 || strcmp(path, "/peer/v1/job") == 0 ||
                  strcmp(path, "/peer/v1/jobs/") == 0 || strcmp(path, "/peer/v1/job/") == 0 ||
                  strcmp(path, "/api/jobs") == 0 || strcmp(path, "/api/job") == 0 ||
                  strcmp(path, "/api/jobs/") == 0 || strcmp(path, "/api/job/") == 0)) {
    if (!require_peer_auth(cfd, req, 0)) { free(req); close(cfd); return; }
    char *body = strstr(req, "\r\n\r\n");
    body = body ? body + 4 : "";
    char *prompt = ng_json_get_string(body, "prompt");
    char *cmd = ng_json_get_string(body, "command");
    if (!cmd) cmd = ng_json_get_string(body, "cmd");
    /* Residual: "" checked only via [0]; whitespace queued empty shell (exit 0)
     * and empty prompt jobs that still burned an agent turn (sync paths 400). */
    if (prompt) {
      const char *p = prompt;
      while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
      if (!*p) {
        free(prompt);
        prompt = NULL;
      }
    }
    if (cmd) {
      const char *p = cmd;
      while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
      if (!*p) {
        free(cmd);
        cmd = NULL;
      }
    }
    char *kind_raw = ng_json_get_string(body, "kind"); /* prompt|shell|watcher */
    /* Residual: kind " shell " / "\\t" rejected as unknown_kind while "" omitted
     * and defaulted to prompt|shell inference — strip like control service. */
    if (kind_raw) {
      const char *p = kind_raw;
      while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
      size_t n = strlen(p);
      while (n && (p[n - 1] == ' ' || p[n - 1] == '\t' || p[n - 1] == '\n' ||
                   p[n - 1] == '\r'))
        n--;
      if (!n) {
        free(kind_raw);
        kind_raw = NULL;
      } else if (p != kind_raw || p[n] != '\0') {
        char *t = (char *)malloc(n + 1);
        if (t) {
          memcpy(t, p, n);
          t[n] = 0;
          free(kind_raw);
          kind_raw = t;
        }
      }
    }
    /* Machine kind token only — never free-text inject into job meta.
     * Residual: kind "Shell"/"WATCHER" rejected as unknown_kind while mesh
     * clients sometimes capitalize tokens; case-fold to canonical leaf. */
    const char *kind = "prompt";
    if (kind_raw && !strcasecmp(kind_raw, "shell")) kind = "shell";
    else if (kind_raw && !strcasecmp(kind_raw, "watcher")) kind = "watcher";
    else if (kind_raw && !strcasecmp(kind_raw, "prompt")) kind = "prompt";
    else if (!kind_raw && cmd && !prompt) kind = "shell";
    /* Residual: unknown kind (e.g. "nope") fell through as prompt and burned
     * an agent turn when a prompt body was present. */
    else if (kind_raw && kind_raw[0]) {
      free(kind_raw); free(prompt); free(cmd);
      http_peer_err(cfd, 400, "unknown_kind");
      free(req); close(cfd); return;
    }
    free(kind_raw);
    /* Residual: kind=watcher only flips watcher_enabled; empty payload is valid
     * (prompt/shell still need work). Whitespace already nulled above. */
    if (!prompt && !cmd && strcmp(kind, "watcher") != 0) {
      free(prompt); free(cmd);
      http_peer_err(cfd, 400, "need_prompt_or_command");
      free(req); close(cfd); return;
    }
    /* Residual: shell-off still queued async shell jobs that finished
     * exit=403 shell_disabled after poll — fail-fast at accept (sync shell
     * still runs ng_run_command which returns the same token). */
    {
      int will_shell = (!strcmp(kind, "shell") || (cmd && cmd[0] && !prompt));
      if (will_shell && !ng_shell_is_enabled()) {
        free(prompt); free(cmd);
        http_peer_err(cfd, 403, "shell_disabled");
        free(req); close(cfd); return;
      }
    }
    /* Residual: prompt jobs queued without login gate; soft-expired access ran
     * agent turns that failed late (sync /peer/v1/prompt ensures first). Shell
     * jobs do not need browser session. */
    if (!strcmp(kind, "prompt")) {
      int need_browser = agent && ng_agent_needs_browser_session(agent);
      if (need_browser && session && !ng_session_valid(session)
          && !session->login_pending)
        (void)ng_session_ensure(session);
      if (need_browser && (!session || !ng_session_valid(session))) {
        free(prompt); free(cmd);
        http_peer_err_flag(cfd, 401, "need_login", "need_login");
        free(req); close(cfd); return;
      }
    }
    char jdir[640];
    snprintf(jdir, sizeof jdir, "%s/jobs", ng_workdir());
    mkdir(jdir, 0755);
    jobs_gc(jdir); /* residual: mesh leaves dozens of done job files */
    char id[32];
    snprintf(id, sizeof id, "%ld%04d", (long)time(NULL), (int)(getpid() % 10000));
    /* Dual-wire job meta — polled via GET /peer/v1/jobs/{id}. */
    char meta[768];
    int mn = snprintf(meta, sizeof meta,
      "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,\"action\":\"job\","
      "\"id\":\"%s\",\"status\":\"queued\",\"kind\":\"%s\","
      NG_PEER_HTTP_DUAL_WIRE "}\n", id, kind);
    char mpath[700], rpath[700], pp[700];
    snprintf(mpath, sizeof mpath, "%s/%s.json", jdir, id);
    snprintf(rpath, sizeof rpath, "%s/%s.out", jdir, id);
    snprintf(pp, sizeof pp, "%s/%s.in", jdir, id);
    ng_write_file(mpath, meta, (size_t)mn);
    if (prompt && prompt[0]) ng_write_file(pp, prompt, strlen(prompt));
    else if (cmd) ng_write_file(pp, cmd, strlen(cmd));
    ng_hub_event("job.queued", "id", id, "kind", kind);

    /* fork worker: do not block peer HTTP */
    pid_t w = fork();
    if (w == 0) {
      close(cfd);
      /* mark running */
      char runm[512];
      int rn = snprintf(runm, sizeof runm,
        "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,\"action\":\"job\","
        "\"id\":\"%s\",\"status\":\"running\",\"kind\":\"%s\","
        NG_PEER_HTTP_DUAL_WIRE "}\n", id, kind);
      ng_write_file(mpath, runm, (size_t)rn);
      ng_hub_event("job.running", "id", id, "kind", kind);
      char *payload = ng_read_file(pp, NULL);
      if (!strcmp(kind, "shell") || (cmd && cmd[0] && !prompt)) {
        /* Residual: async shell used full agent CMD_TIMEOUT (600s) so mesh
         * adb pulls stacked non-listening job workers until deadline. */
        int job_to = agent && agent->timeout_sec > 0 ? agent->timeout_sec
                                                    : ng_cmd_timeout_sec();
        if (job_to <= 0 || job_to > NG_JOB_SHELL_TIMEOUT_SEC)
          job_to = NG_JOB_SHELL_TIMEOUT_SEC;
        ng_cmd_result cr = ng_run_command(payload ? payload : "", job_to);
        char *esc = ng_json_escape(cr.output ? cr.output : "");
        /* Residual: shell_disabled/personal_acl nested JSON only in output;
         * mesh had to parse nested shell.v1 for a machine error token. */
        char *err_tok = NULL;
        if (cr.output && cr.output[0] == '{')
          err_tok = ng_json_get_string(cr.output, "error");
        /* Residual: wall timeout was exit=124 only — no machine error leaf. */
        if ((!err_tok || !err_tok[0]) && cr.exit_code == 124)
          err_tok = strdup("timeout");
        char *jb = NULL;
        if (err_tok && err_tok[0]) {
          char *ee = ng_json_escape(err_tok);
          asprintf(&jb,
            "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":%s,\"action\":\"job\","
            "\"id\":\"%s\",\"status\":\"done\",\"kind\":\"shell\",\"exit\":%d,"
            "\"error\":\"%s\",\"output\":\"%s\"," NG_PEER_HTTP_DUAL_WIRE "}",
            cr.exit_code == 0 ? "true" : "false",
            id, cr.exit_code, ee ? ee : "", esc ? esc : "");
          free(ee);
        } else {
          asprintf(&jb,
            "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":%s,\"action\":\"job\","
            "\"id\":\"%s\",\"status\":\"done\",\"kind\":\"shell\",\"exit\":%d,"
            "\"output\":\"%s\"," NG_PEER_HTTP_DUAL_WIRE "}",
            cr.exit_code == 0 ? "true" : "false",
            id, cr.exit_code, esc ? esc : "");
        }
        if (jb) { ng_write_file(mpath, jb, strlen(jb)); free(jb); }
        free(esc);
        free(err_tok);
        ng_cmd_result_free(&cr);
        free(payload);
        ng_hub_event("job.done", "id", id, "kind", "shell");
      } else if (!strcmp(kind, "watcher")) {
        char wp[640];
        snprintf(wp, sizeof wp, "%s/watcher_enabled", ng_workdir());
        ng_write_file(wp, "1", 1);
        char *esc = ng_json_escape(payload ? payload : "");
        char *jb = NULL;
        asprintf(&jb,
          "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,\"action\":\"job\","
          "\"id\":\"%s\",\"status\":\"done\",\"kind\":\"watcher\","
          "\"enabled\":true,\"prompt\":\"%s\"," NG_PEER_HTTP_DUAL_WIRE "}",
          id, esc ? esc : "");
        if (jb) { ng_write_file(mpath, jb, strlen(jb)); free(jb); }
        free(esc);
        free(payload);
        ng_hub_event("job.done", "id", id, "kind", "watcher");
      } else {
        char *reply = ng_agent_run(agent, payload ? payload : "");
        char *esc = ng_json_escape(reply ? reply : "");
        char *jb = NULL;
        asprintf(&jb,
          "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,\"action\":\"job\","
          "\"id\":\"%s\",\"status\":\"done\",\"kind\":\"prompt\",\"reply\":\"%s\","
          NG_PEER_HTTP_DUAL_WIRE "}",
          id, esc ? esc : "");
        if (jb) { ng_write_file(mpath, jb, strlen(jb)); free(jb); }
        free(esc); free(reply); free(payload);
        ng_hub_event("job.done", "id", id, "kind", "prompt");
      }
      /* Residual: mesh polls GET /jobs/{id} only — never hit collection GC.
       * After done, prune finished metas so we do not sit at KEEP+1 until next POST. */
      jobs_gc(jdir);
      _exit(0);
    }
    /* Residual: fork fail still returned job_queued; meta stayed queued until
     * cool_restart orphan sweep. Mark error + 503 so mesh does not poll forever. */
    if (w < 0) {
      char *jb = NULL;
      asprintf(&jb,
        "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":false,\"action\":\"job\","
        "\"id\":\"%s\",\"status\":\"error\",\"kind\":\"%s\","
        "\"error\":\"fork_failed\","
        NG_PEER_HTTP_DUAL_WIRE "}\n",
        id, kind);
      if (jb) {
        ng_write_file(mpath, jb, strlen(jb));
        free(jb);
      }
      free(prompt); free(cmd);
      http_peer_err(cfd, 503, "fork_failed");
      free(req); close(cfd); return;
    }
    char *ack = NULL;
    char *id_esc = ng_json_escape(id);
    /* Residual: job_queued omitted kind; mesh had to poll meta to learn
     * prompt|shell|watcher while hub events already carried it. */
    asprintf(&ack,
      "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,\"action\":\"job_queued\","
      "\"id\":\"%s\",\"status\":\"queued\",\"kind\":\"%s\","
      "\"poll\":\"/peer/v1/jobs/%s\","
      NG_PEER_HTTP_DUAL_WIRE "}",
      id_esc ? id_esc : "", kind, id_esc ? id_esc : "");
    free(id_esc);
    http_response(cfd, 202, "application/json", ack ? ack : "{}", ack ? strlen(ack) : 2);
    free(prompt); free(cmd); free(ack);
    free(req); close(cfd); return;
  }

  /* GET /peer/v1/jobs — dual-wire index of recent job metas (no full replies).
   * Residual: mesh probes hit collection path and got not_found (only /jobs/{id}).
   * Residual: GET .../jobs/ (trailing slash) matched /jobs/{id} with empty id →
   * bad_id; /api/jobs was 404 while health/ready already had /api aliases. */
  if (is_get && (strcmp(path, "/peer/v1/jobs") == 0 || strcmp(path, "/peer/v1/job") == 0 ||
                 strcmp(path, "/peer/v1/jobs/") == 0 || strcmp(path, "/peer/v1/job/") == 0 ||
                 strcmp(path, "/api/jobs") == 0 || strcmp(path, "/api/job") == 0 ||
                 strcmp(path, "/api/jobs/") == 0 || strcmp(path, "/api/job/") == 0)) {
    if (!require_peer_auth(cfd, req, 0)) { free(req); close(cfd); return; }
    char jdir[640];
    snprintf(jdir, sizeof jdir, "%s/jobs", ng_workdir());
    jobs_gc(jdir);
    DIR *d = opendir(jdir);
    /* Match GC keep so mesh index can see every retained finished meta. */
    enum { MAX_LIST = NG_JOBS_KEEP };
    char ids[MAX_LIST][32];
    int nids = 0;
    if (d) {
      struct dirent *de;
      while ((de = readdir(d)) != NULL) {
        const char *name = de->d_name;
        size_t len = strlen(name);
        if (len < 6 || strcmp(name + len - 5, ".json") != 0) continue;
        /* id is digits before .json */
        int ok = 1;
        size_t idlen = len - 5;
        if (idlen >= sizeof ids[0]) continue;
        for (size_t i = 0; i < idlen; i++) {
          if (!isdigit((unsigned char)name[i])) { ok = 0; break; }
        }
        if (!ok) continue;
        if (nids < MAX_LIST) {
          memcpy(ids[nids], name, idlen);
          ids[nids][idlen] = 0;
          nids++;
        } else {
          /* keep newest-ish: ids are time-prefixed; replace smallest */
          int worst = 0;
          for (int i = 1; i < MAX_LIST; i++)
            if (strcmp(ids[i], ids[worst]) < 0) worst = i;
          if (strncmp(name, ids[worst], idlen) > 0) {
            memcpy(ids[worst], name, idlen);
            ids[worst][idlen] = 0;
          }
        }
      }
      closedir(d);
    }
    /* sort ids descending (newest first) — simple insertion */
    for (int i = 1; i < nids; i++) {
      char tmp[32];
      memcpy(tmp, ids[i], sizeof tmp);
      int j = i;
      while (j > 0 && strcmp(ids[j - 1], tmp) < 0) {
        memcpy(ids[j], ids[j - 1], sizeof ids[j]);
        j--;
      }
      memcpy(ids[j], tmp, sizeof tmp);
    }
    /* Per-entry budget: id/status/kind + optional ok/exit/error + poll. */
    enum { JOB_IDX_ENTRY = 320 };
    size_t cap = 256 + (size_t)nids * JOB_IDX_ENTRY;
    char *out = (char *)malloc(cap);
    if (!out) {
      http_peer_err(cfd, 500, "oom");
      free(req); close(cfd); return;
    }
    size_t used = 0;
    /* Residual: health/info expose jobs_keep; jobs index only had count — mesh
     * probes could not compare retain cap without hardcoding NG_JOBS_KEEP. */
    int wr = snprintf(out + used, cap - used,
      "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,\"action\":\"jobs\","
      "\"count\":%d,\"jobs_keep\":%d,\"jobs\":[", nids, (int)NG_JOBS_KEEP);
    if (wr > 0) used += (size_t)wr;
    for (int i = 0; i < nids && used + JOB_IDX_ENTRY < cap; i++) {
      char mpath[700];
      snprintf(mpath, sizeof mpath, "%s/%s.json", jdir, ids[i]);
      char *meta = ng_read_file(mpath, NULL);
      const char *st = "unknown";
      const char *kind = "unknown";
      if (meta) {
        char *s = ng_json_get_string(meta, "status");
        char *k = ng_json_get_string(meta, "kind");
        /* Residual: error tokens (orphan_restart, shell_disabled) only on
         * full job poll — mesh index could not fail-fast without GET by id. */
        char *e = ng_json_get_string(meta, "error");
        /* Residual: shell exit codes only on full poll; index lacked exit leaf
         * so mesh could not sort fail vs ok without GET /jobs/{id}. */
        int has_exit = 0, exit_code = 0;
        {
          const char *ep = strstr(meta, "\"exit\":");
          if (ep) {
            ep += 7;
            while (*ep == ' ' || *ep == '\t') ep++;
            if (*ep == '-' || isdigit((unsigned char)*ep)) {
              exit_code = atoi(ep);
              has_exit = 1;
            }
          }
        }
        /* Residual: job plate ok (done+exit!=0 → false, orphan → false) only on
         * full poll; index had status/exit/error but mesh dual-wire still needed
         * to re-derive success without the plate ok leaf. */
        int has_ok = 0, ok_val = 0;
        {
          const char *op = strstr(meta, "\"ok\":");
          if (op) {
            op += 5;
            while (*op == ' ' || *op == '\t') op++;
            if (strncmp(op, "true", 4) == 0) {
              has_ok = 1;
              ok_val = 1;
            } else if (strncmp(op, "false", 5) == 0) {
              has_ok = 1;
              ok_val = 0;
            }
          }
        }
        if (s && s[0]) st = s;
        if (k && k[0]) kind = k;
        /* free later via copies — ng_json_get_string allocates */
        char *st_own = s;
        char *k_own = k;
        char *e_own = e;
        char *id_esc = ng_json_escape(ids[i]);
        char *st_esc = ng_json_escape(st);
        char *k_esc = ng_json_escape(kind);
        char *e_esc = (e_own && e_own[0]) ? ng_json_escape(e_own) : NULL;
        /* Optional dual-wire leaves — one snprintf path (no 8-way branch). */
        char extra[240];
        size_t eu = 0;
        extra[0] = 0;
        if (has_ok && eu + 16 < sizeof extra)
          eu += (size_t)snprintf(extra + eu, sizeof extra - eu,
            ",\"ok\":%s", ok_val ? "true" : "false");
        if (has_exit && eu + 24 < sizeof extra)
          eu += (size_t)snprintf(extra + eu, sizeof extra - eu,
            ",\"exit\":%d", exit_code);
        if (e_esc && e_esc[0] && eu + 8 + strlen(e_esc) < sizeof extra)
          eu += (size_t)snprintf(extra + eu, sizeof extra - eu,
            ",\"error\":\"%s\"", e_esc);
        wr = snprintf(out + used, cap - used,
          "%s{\"id\":\"%s\",\"status\":\"%s\",\"kind\":\"%s\"%s,"
          "\"poll\":\"/peer/v1/jobs/%s\"}",
          i ? "," : "",
          id_esc ? id_esc : ids[i],
          st_esc ? st_esc : st,
          k_esc ? k_esc : kind,
          extra,
          id_esc ? id_esc : ids[i]);
        if (wr > 0) used += (size_t)wr;
        free(id_esc); free(st_esc); free(k_esc); free(e_esc);
        free(st_own); free(k_own); free(e_own);
        free(meta);
      } else {
        char *id_esc = ng_json_escape(ids[i]);
        wr = snprintf(out + used, cap - used,
          "%s{\"id\":\"%s\",\"status\":\"missing\",\"kind\":\"\","
          "\"poll\":\"/peer/v1/jobs/%s\"}",
          i ? "," : "",
          id_esc ? id_esc : ids[i],
          id_esc ? id_esc : ids[i]);
        if (wr > 0) used += (size_t)wr;
        free(id_esc);
      }
    }
    wr = snprintf(out + used, cap - used,
      "]," NG_PEER_HTTP_DUAL_WIRE "}");
    if (wr > 0) used += (size_t)wr;
    http_response(cfd, 200, "application/json", out, used);
    free(out);
    free(req); close(cfd); return;
  }

  /* Residual: /api/jobs/{id} 404; id with trailing slash failed isdigit → bad_id. */
  if (is_get && (strncmp(path, "/peer/v1/jobs/", 14) == 0 ||
                 strncmp(path, "/peer/v1/job/", 13) == 0 ||
                 strncmp(path, "/api/jobs/", 10) == 0 ||
                 strncmp(path, "/api/job/", 9) == 0)) {
    if (!require_peer_auth(cfd, req, 0)) { free(req); close(cfd); return; }
    const char *idraw = path;
    if (strncmp(path, "/peer/v1/jobs/", 14) == 0) idraw = path + 14;
    else if (strncmp(path, "/peer/v1/job/", 13) == 0) idraw = path + 13;
    else if (strncmp(path, "/api/jobs/", 10) == 0) idraw = path + 10;
    else idraw = path + 9;
    /* strip one trailing slash so /jobs/123/ still polls */
    char idbuf[40];
    size_t idlen = 0;
    while (idraw[idlen] && idraw[idlen] != '/' && idlen + 1 < sizeof idbuf) {
      idbuf[idlen] = idraw[idlen];
      idlen++;
    }
    idbuf[idlen] = 0;
    /* only allow optional final slash, no mid-path segments */
    if (idraw[idlen] == '/') {
      if (idraw[idlen + 1] != 0) {
        http_peer_err(cfd, 400, "bad_id");
        free(req); close(cfd); return;
      }
    } else if (idraw[idlen] != 0) {
      http_peer_err(cfd, 400, "bad_id");
      free(req); close(cfd); return;
    }
    const char *id = idbuf;
    /* job ids are digits only (time+pid) */
    if (!id[0] || strstr(id, "..")) {
      http_peer_err(cfd, 400, "bad_id");
      free(req); close(cfd); return;
    }
    for (const char *p = id; *p; p++) {
      if (!isdigit((unsigned char)*p)) {
        http_peer_err(cfd, 400, "bad_id");
        free(req); close(cfd); return;
      }
    }
    char mpath[700];
    snprintf(mpath, sizeof mpath, "%s/jobs/%s.json", ng_workdir(), id);
    size_t blen = 0;
    char *body = ng_read_file(mpath, &blen);
    if (!body) {
      http_peer_err(cfd, 404, "job_not_found");
      free(req); close(cfd); return;
    }
    http_response(cfd, 200, "application/json", body, blen);
    free(body);
    /* Mesh mostly polls by id; opportunistic GC keeps jobs/ bounded. */
    {
      char jdir[640];
      snprintf(jdir, sizeof jdir, "%s/jobs", ng_workdir());
      jobs_gc(jdir);
    }
    free(req); close(cfd); return;
  }

  /* BrainCube plugin — sensor cubes + MetaCube; live viz for wrapper */
  /* Residual: trailing slash 404 after models/task/control gained slash aliases. */
  if (is_get && (strcmp(path, "/peer/v1/braincube/live") == 0 ||
                 strcmp(path, "/peer/v1/braincube/live/") == 0 ||
                 strcmp(path, "/api/braincube/live") == 0 ||
                 strcmp(path, "/api/braincube/live/") == 0)) {
    /* allow_loopback=1: local wrapper/CLI without token; remote still needs token */
    if (!require_peer_auth(cfd, req, 1)) { free(req); close(cfd); return; }
    {
      char *jb = ng_bc_live_json(); /* live_json calls init internally */
      http_response(cfd, 200, "application/json", jb ? jb : "{}", jb ? strlen(jb) : 2);
      free(jb);
    }
    free(req); close(cfd); return;
  }
  if (is_get && (strcmp(path, "/peer/v1/braincube") == 0 ||
                 strcmp(path, "/peer/v1/braincube/") == 0 ||
                 strcmp(path, "/api/braincube") == 0 ||
                 strcmp(path, "/api/braincube/") == 0)) {
    if (!require_peer_auth(cfd, req, 1)) { free(req); close(cfd); return; }
    {
      char *jb = ng_bc_status_json();
      http_response(cfd, 200, "application/json", jb ? jb : "{}", jb ? strlen(jb) : 2);
      free(jb);
    }
    free(req); close(cfd); return;
  }
  if (is_post && (strcmp(path, "/peer/v1/braincube") == 0 ||
                  strcmp(path, "/peer/v1/braincube/") == 0 ||
                  strcmp(path, "/api/braincube") == 0 ||
                  strcmp(path, "/api/braincube/") == 0)) {
    if (!require_peer_auth(cfd, req, 1)) { free(req); close(cfd); return; }
    char *body = strstr(req, "\r\n\r\n");
    body = body ? body + 4 : "";
    {
      char *jb = ng_bc_handle_post(body);
      http_response(cfd, 200, "application/json", jb ? jb : "{}", jb ? strlen(jb) : 2);
      free(jb);
    }
    free(req); close(cfd); return;
  }

  /* Subagents: list / spawn / status / cancel (light, max 8, share session) */
  /* Residual: trailing slash 404/misroute after collection paths gained slash. */
  if (is_get && (strcmp(path, "/peer/v1/subagents") == 0 ||
                 strcmp(path, "/peer/v1/subagents/") == 0 ||
                 strcmp(path, "/api/subagents") == 0 ||
                 strcmp(path, "/api/subagents/") == 0)) {
    /* GET list is read-only; allow loopback like /api/task so board poll reaps dead PIDs */
    if (!require_peer_auth(cfd, req, 1)) { free(req); close(cfd); return; }
    if (agent) ng_agent_apply_provider_policy(agent);
    char *list = ng_subagent_list_json();
    char *jb = NULL;
    asprintf(&jb,
      "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,\"action\":\"subagent_list\","
      "\"enabled\":%s,\"max\":%d,\"running\":%d,"
      "\"llm_serial\":%s,\"subagents\":%s,"
      NG_PEER_HTTP_DUAL_WIRE "}",
      ng_subagent_enabled() ? "true" : "false",
      ng_subagent_max(), ng_subagent_running_count(),
      ng_llm_sched_enabled() ? "true" : "false",
      list ? list : "[]");
    free(list);
    http_response(cfd, 200, "application/json", jb ? jb : "{}", jb ? strlen(jb) : 2);
    free(jb); free(req); close(cfd); return;
  }
  if (is_post && (strcmp(path, "/peer/v1/subagents") == 0 ||
                  strcmp(path, "/api/subagents") == 0)) {
    if (!require_peer_auth(cfd, req, 0)) { free(req); close(cfd); return; }
    if (agent) ng_agent_apply_provider_policy(agent);
    char *body = strstr(req, "\r\n\r\n");
    body = body ? body + 4 : "";
    char *action = ng_json_get_string(body, "action");
    if (!action) action = strdup("spawn");
    if (!strcmp(action, "cancel")) {
      char *id = ng_json_get_string(body, "id");
      int rc = ng_subagent_cancel(id ? id : "");
      free(id); free(action);
      if (rc == 0)
        http_json(cfd, 200,
          "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,"
          "\"action\":\"subagent_cancel\",\"cancelled\":true,"
          NG_PEER_HTTP_DUAL_WIRE "}");
      else
        http_peer_err(cfd, 400, "cancel_failed");
      free(req); close(cfd); return;
    }
    if (!strcmp(action, "status")) {
      char *id = ng_json_get_string(body, "id");
      char *j = ng_subagent_status_json(id ? id : "");
      free(id); free(action);
      http_response(cfd, 200, "application/json", j ? j : "{}", j ? strlen(j) : 2);
      free(j); free(req); close(cfd); return;
    }
    /* spawn */
    char *prompt = ng_json_get_string(body, "prompt");
    char *desc = ng_json_get_string(body, "description");
    char *type = ng_json_get_string(body, "type");
    /* Residual: whitespace-only spawn ran empty subagent (peer prompt trims). */
    if (prompt) {
      const char *p = prompt;
      while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
      if (!*p) {
        free(prompt);
        prompt = NULL;
      }
    }
    if (!prompt || !agent) {
      free(prompt); free(desc); free(type); free(action);
      http_peer_err(cfd, 400, "need_prompt");
      free(req); close(cfd); return;
    }
    /* Residual: spawn had no login gate; soft-expired access failed late in
     * the subagent worker (prompt/jobs ensure first). */
    {
      int need_browser = ng_agent_needs_browser_session(agent);
      if (need_browser && session && !ng_session_valid(session)
          && !session->login_pending)
        (void)ng_session_ensure(session);
      if (need_browser && (!session || !ng_session_valid(session))) {
        free(prompt); free(desc); free(type); free(action);
        http_peer_err_flag(cfd, 401, "need_login", "need_login");
        free(req); close(cfd); return;
      }
    }
    if (!ng_subagent_enabled()) {
      free(prompt); free(desc); free(type); free(action);
      http_peer_err(cfd, 503, "subagents_disabled");
      free(req); close(cfd); return;
    }
    char *id = ng_agent_subagent_spawn(agent, type, desc, prompt);
    free(prompt); free(desc); free(type); free(action);
    if (!id) {
      http_peer_err(cfd, 429, "spawn_failed");
      free(req); close(cfd); return;
    }
    char *jb = NULL;
    char *id_esc = ng_json_escape(id);
    asprintf(&jb,
             "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,"
             "\"action\":\"subagent_spawn\",\"id\":\"%s\",\"status\":\"running\","
             NG_PEER_HTTP_DUAL_WIRE "}",
             id_esc ? id_esc : "");
    free(id); free(id_esc);
    http_response(cfd, 202, "application/json", jb ? jb : "{}", jb ? strlen(jb) : 2);
    free(jb); free(req); close(cfd); return;
  }
  if (is_get && strncmp(path, "/peer/v1/subagents/", 19) == 0) {
    if (!require_peer_auth(cfd, req, 0)) { free(req); close(cfd); return; }
    const char *id = path + 19;
    char *j = ng_subagent_status_json(id);
    http_response(cfd, 200, "application/json", j ? j : "{}", j ? strlen(j) : 2);
    free(j); free(req); close(cfd); return;
  }

  /* Residual: /api/prompt and trailing slash 404 while /api/jobs already works. */
  if (is_post && (strcmp(path, "/peer/v1/prompt") == 0 || strcmp(path, "/peer/v1/prompt/") == 0 ||
                  strcmp(path, "/api/prompt") == 0 || strcmp(path, "/api/prompt/") == 0)) {
    if (!require_peer_auth(cfd, req, 0)) { free(req); close(cfd); return; }

    {
      int need_browser = agent && ng_agent_needs_browser_session(agent);
      /* Residual: soft-expired access failed login gate without ensure (info fixed). */
      if (need_browser && session && !ng_session_valid(session)
          && !session->login_pending)
        (void)ng_session_ensure(session);
      if (need_browser && (!session || !ng_session_valid(session))) {
        http_peer_err_flag(cfd, 401, "need_login", "need_login");
        free(req); close(cfd); return;
      }
    }
    char *body = strstr(req, "\r\n\r\n");
    body = body ? body + 4 : "";
    char *prompt = ng_json_get_string(body, "prompt");
    if (!prompt) prompt = ng_json_get_string(body, "message");
    if (!prompt) prompt = ng_json_get_string(body, "q");
    /* Residual: "" / whitespace ran agent empty_prompt plate nested in ok:true. */
    if (prompt) {
      const char *p = prompt;
      while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
      if (!*p) {
        free(prompt);
        prompt = NULL;
      }
    }
    if (!prompt) {
      http_peer_err(cfd, 400, "missing_prompt");
      free(req); close(cfd); return;
    }
    ng_log("peer: prompt from remote session: %.200s", prompt);
    char *reply = ng_agent_run(agent, prompt);
    char *esc = ng_json_escape(reply ? reply : "");
    char *jb = NULL;
    asprintf(&jb,
             "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,"
             "\"action\":\"prompt\",\"reply\":\"%s\",\"source\":\"nanobot-peer\","
             NG_PEER_HTTP_DUAL_WIRE "}",
             esc ? esc : "");
    http_response(cfd, 200, "application/json", jb ? jb : "{}", jb ? strlen(jb) : 2);
    free(prompt); free(reply); free(esc); free(jb);
    free(req); close(cfd); return;
  }

  /* Residual: /api/shell and trailing slash 404 (not /api/shell/approvals). */
  if (is_post && (strcmp(path, "/peer/v1/shell") == 0 || strcmp(path, "/peer/v1/shell/") == 0 ||
                  strcmp(path, "/api/shell") == 0 || strcmp(path, "/api/shell/") == 0)) {
    if (!require_peer_auth(cfd, req, 0)) { free(req); close(cfd); return; }

    char *body = strstr(req, "\r\n\r\n");
    body = body ? body + 4 : "";
    char *cmd = ng_json_get_string(body, "command");
    if (!cmd) cmd = ng_json_get_string(body, "cmd");
    /* Residual: "" / whitespace command ran as empty shell (ok:true exit 0). */
    if (cmd) {
      const char *p = cmd;
      while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
      if (!*p) {
        free(cmd);
        cmd = NULL;
      }
    }
    if (!cmd) {
      http_peer_err(cfd, 400, "missing_command");
      free(req); close(cfd); return;
    }
    /* Residual: sync shell returned HTTP 200 + nested shell.v1 shell_disabled
     * while async jobs fail-fast 403 (99691c3). Align dual-wire token + status. */
    if (!ng_shell_is_enabled()) {
      free(cmd);
      http_peer_err(cfd, 403, "shell_disabled");
      free(req); close(cfd); return;
    }
    ng_log("peer: shell from remote session: %.80s", cmd);
    ng_cmd_result cr = ng_run_command(cmd, agent->timeout_sec > 0 ? agent->timeout_sec : 60);
    char *esc = ng_json_escape(cr.output ? cr.output : "");
    char *jb = NULL;
    int need_appr = (cr.exit_code == 425);
    char *aid = NULL;
    if (need_appr && cr.output) {
      /* Dual-wire plate first; legacy free-text approval_id= fallback. */
      aid = ng_json_get_string(cr.output, "approval_id");
      if (!aid) {
        const char *p = strstr(cr.output, "approval_id=");
        if (p) {
          p += 12;
          size_t n = 0;
          while (p[n] && p[n] != '\n' && p[n] != '\r' && n < 32) n++;
          aid = malloc(n + 1);
          if (aid) {
            memcpy(aid, p, n);
            aid[n] = 0;
          }
        }
      }
    }
    /* Residual: shell_disabled nested only in output; surface machine error. */
    char *err_tok = NULL;
    if (!need_appr && cr.output && cr.output[0] == '{')
      err_tok = ng_json_get_string(cr.output, "error");
    if (need_appr) {
      char *aid_esc = ng_json_escape(aid ? aid : "");
      asprintf(&jb,
        "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":false,"
        "\"action\":\"shell\",\"exit\":425,\"need_approval\":true,"
        "\"approval_id\":\"%s\",\"output\":\"%s\",\"source\":\"nanobot-peer\","
        NG_PEER_HTTP_DUAL_WIRE "}",
        aid_esc ? aid_esc : "", esc ? esc : "");
      free(aid_esc);
    } else if (err_tok && err_tok[0]) {
      char *ee = ng_json_escape(err_tok);
      asprintf(&jb,
        "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":%s,"
        "\"action\":\"shell\",\"exit\":%d,\"error\":\"%s\",\"output\":\"%s\","
        "\"source\":\"nanobot-peer\","
        NG_PEER_HTTP_DUAL_WIRE "}",
        cr.exit_code == 0 ? "true" : "false", cr.exit_code,
        ee ? ee : "", esc ? esc : "");
      free(ee);
    } else {
      asprintf(&jb,
        "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":%s,"
        "\"action\":\"shell\",\"exit\":%d,\"output\":\"%s\","
        "\"source\":\"nanobot-peer\","
        NG_PEER_HTTP_DUAL_WIRE "}",
        cr.exit_code == 0 ? "true" : "false", cr.exit_code, esc ? esc : "");
    }
    http_response(cfd, 200, "application/json", jb ? jb : "{}", jb ? strlen(jb) : 2);
    free(cmd); free(esc); free(jb); free(aid); free(err_tok);
    ng_cmd_result_free(&cr);
    free(req); close(cfd); return;
  }

  /* Shell security: gate password + pending approvals */
  if (is_get && (strcmp(path, "/api/shell/approvals") == 0
                 || strcmp(path, "/peer/v1/shell/approvals") == 0)) {
    if (!require_peer_auth(cfd, req, 1)) { free(req); close(cfd); return; }
    char *list = ng_shell_approval_list_json();
    char *out = NULL;
    asprintf(&out,
             "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,"
             "\"action\":\"shell_approvals\",\"gate_configured\":%s,\"pending\":%s,"
             NG_PEER_HTTP_DUAL_WIRE "}",
             ng_shell_gate_configured() ? "true" : "false",
             list ? list : "[]");
    http_response(cfd, 200, "application/json", out ? out : "{}", out ? strlen(out) : 2);
    free(list); free(out);
    free(req); close(cfd); return;
  }
  if (is_post && (strcmp(path, "/api/shell/gate") == 0
                  || strcmp(path, "/peer/v1/shell/gate") == 0)) {
    if (!require_peer_auth(cfd, req, 0)) { free(req); close(cfd); return; }
    char *body = strstr(req, "\r\n\r\n");
    body = body ? body + 4 : "";
    char *action = ng_json_get_string(body, "action");
    char *pw = ng_json_get_string(body, "password");
    if (action && !strcmp(action, "set") && pw) {
      int rc = ng_shell_gate_set_password(pw);
      if (rc == 0)
        http_json(cfd, 200,
          "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,"
          "\"action\":\"gate_set\"," NG_PEER_HTTP_DUAL_WIRE "}");
      else
        http_peer_err(cfd, 400, "gate_set_failed");
    } else if (action && !strcmp(action, "verify") && pw) {
      int ok = ng_shell_gate_verify_password(pw);
      char plate[256];
      snprintf(plate, sizeof plate,
               "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,"
               "\"action\":\"gate_verify\",\"valid\":%s,"
               NG_PEER_HTTP_DUAL_WIRE "}",
               ok ? "true" : "false");
      http_json(cfd, 200, plate);
    } else {
      http_peer_err(cfd, 400, "need_gate_action");
    }
    free(action); free(pw);
    free(req); close(cfd); return;
  }
  if (is_post && (strcmp(path, "/api/shell/approve") == 0
                  || strcmp(path, "/peer/v1/shell/approve") == 0)) {
    if (!require_peer_auth(cfd, req, 0)) { free(req); close(cfd); return; }
    char *body = strstr(req, "\r\n\r\n");
    body = body ? body + 4 : "";
    char *id = ng_json_get_string(body, "id");
    if (!id) id = ng_json_get_string(body, "approval_id");
    char *pw = ng_json_get_string(body, "password");
    char *action = ng_json_get_string(body, "action");
    if (action && !strcmp(action, "reject") && id) {
      ng_shell_approval_reject(id);
      http_json(cfd, 200,
        "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,"
        "\"action\":\"shell_reject\",\"status\":\"rejected\","
        NG_PEER_HTTP_DUAL_WIRE "}");
      free(id); free(pw); free(action);
      free(req); close(cfd); return;
    }
    char *cmd = NULL;
    int rc = ng_shell_approval_approve(id, pw, &cmd);
    if (rc == 0 && cmd) {
      ng_cmd_result cr = ng_run_command_approved(cmd,
          agent && agent->timeout_sec > 0 ? agent->timeout_sec : 60);
      char *esc = ng_json_escape(cr.output ? cr.output : "");
      char *cmd_esc = ng_json_escape(cmd);
      char *jb = NULL;
      asprintf(&jb,
        "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":%s,\"action\":\"shell_approve\","
        "\"exit\":%d,\"output\":\"%s\",\"approved\":true,\"command\":\"%s\","
        NG_PEER_HTTP_DUAL_WIRE "}",
        cr.exit_code == 0 ? "true" : "false", cr.exit_code,
        esc ? esc : "", cmd_esc ? cmd_esc : "");
      http_response(cfd, 200, "application/json", jb ? jb : "{}", jb ? strlen(jb) : 2);
      free(esc); free(cmd_esc); free(jb);
      ng_cmd_result_free(&cr);
    } else {
      const char *err = rc == -3 ? "gate_password_not_configured"
                       : rc == -4 ? "invalid_password"
                       : rc == -2 ? "not_pending"
                       : "approval_failed";
      http_peer_err(cfd, 403, err);
    }
    free(id); free(pw); free(action); free(cmd);
    free(req); close(cfd); return;
  }


  http_peer_err(cfd, 404, "not_found");
  free(req);
  close(cfd);
}

/* sig_atomic_t: mutated from SIGCHLD handler and serve loop. */
static volatile sig_atomic_t g_live_children = 0;

static void reap_children(void) {
  int st;
  pid_t p;
  while ((p = waitpid(-1, &st, WNOHANG)) > 0) {
    if (g_live_children > 0) g_live_children--;
  }
}

static void on_sigchld(int s) {
  (void)s;
  /* waitpid is async-signal-safe (POSIX). Reap here so per-request worker
   * zombies do not pile under the peer when accept() is SA_RESTART-stuck or
   * the parent is blocked in session/env reload between accepts. */
  int st;
  pid_t p;
  while ((p = waitpid(-1, &st, WNOHANG)) > 0) {
    if (g_live_children > 0) g_live_children--;
  }
}

int ng_http_serve(ng_http_cfg *cfg) {
  signal(SIGPIPE, SIG_IGN);
  {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigchld;
    /* No SA_RESTART: accept()/waitpid must wake on SIGCHLD so reaps and
     * stop flags are not deferred until the next client connection. */
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGCHLD, &sa, NULL) != 0)
      signal(SIGCHLD, on_sigchld);
  }
  int sfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sfd < 0) { ng_log("socket: %s", strerror(errno)); return -1; }
  int on = 1;
  setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  /* Default: loopback only. --lan / bind_lan=1 opens 0.0.0.0 (opt-in MCP/LAN). */
  addr.sin_addr.s_addr = (cfg && cfg->bind_lan)
    ? htonl(INADDR_ANY)
    : htonl(INADDR_LOOPBACK);
  addr.sin_port = htons((uint16_t)cfg->port);
  if (bind(sfd, (struct sockaddr *)&addr, sizeof addr) != 0) {
    const char *h = (cfg && cfg->bind_lan) ? "0.0.0.0" : "127.0.0.1";
    ng_log("bind %s:%d: %s", h, cfg->port, strerror(errno));
    /* Dual-wire bind fail — do not leave only free-text in the log. */
    fprintf(stderr,
            "{\"schema\":\"nanobot.serve.v1\",\"ok\":false,"
            "\"action\":\"listen\",\"error\":\"bind_failed\","
            "\"host\":\"%s\",\"port\":%d,\"errno_msg\":\"%s\","
            "\"product_wire\":\"smx2\",\"peer_http\":\"lab_ops_only\","
            "\"peer_http_is_product_bus\":false,\"share\":\"state_matrix_only\","
            "\"hold_flash\":1,\"llm_is_commander\":false,\"python\":0}\n",
            h, cfg->port, strerror(errno));
    fflush(stderr);
    close(sfd);
    return -1;
  }
  if (listen(sfd, 64) != 0) {
    ng_log("listen: %s", strerror(errno));
    close(sfd);
    return -1;
  }
  ng_log("http listening on %s:%d (concurrent fork, max %d)%s",
         (cfg && cfg->bind_lan) ? "0.0.0.0" : "127.0.0.1",
         cfg->port, ng_http_max_children(),
         (cfg && cfg->bind_lan) ? " [LAN — peer token required for mutate]" : " [loopback only]");
  g_serve_started = time(NULL);
  g_serve_pid = getpid(); /* parent — health is answered in fork workers */
  /* Dual-wire listen plate only after socket is live (no false ok on EADDRINUSE). */
  if (cfg && cfg->on_listening)
    cfg->on_listening(cfg);

  /* Residual: previous process workers are gone; finish their live metas. */
  {
    char jdir[640];
    snprintf(jdir, sizeof jdir, "%s/jobs", ng_workdir());
    jobs_mark_orphans(jdir);
    jobs_gc(jdir);
  }

  g_live_children = 0;
  {
    time_t last_sub_reap = 0;
    while (!(cfg->stop && *cfg->stop)) {
    reap_children();
    /* Subagents are double-forked (reparented to init); still sweep metas so
     * main stays "idle" with accurate running=0 and no stale "running" PIDs. */
    {
      time_t now = time(NULL);
      if (now != last_sub_reap) {
        last_sub_reap = now;
        ng_subagent_reap_all();
      }
    }
    while (g_live_children >= ng_http_max_children()) {
      int st = 0;
      pid_t d = waitpid(-1, &st, 0);
      if (d > 0) {
        if (g_live_children > 0) g_live_children--;
      } else if (errno == EINTR) {
        continue;
      } else {
        break;
      }
    }

    struct sockaddr_in cli;
    socklen_t cl = sizeof cli;
    int cfd = accept(sfd, (struct sockaddr *)&cli, &cl);
    if (cfd < 0) {
      if (errno == EINTR) {
        reap_children();
        /* SIGTERM/SIGINT must be non-SA_RESTART so we exit when *stop is set. */
        if (cfg->stop && *cfg->stop) break;
        continue;
      }
      ng_log("accept: %s", strerror(errno));
      break;
    }

    /* Refresh tokens + pending device login from secure files (fork-safe).
     * Parent token poll is interval-throttled inside ng_session_poll_login so
     * accept() is not blocked on curl for every connection. */
    if (cfg->session) {
      ng_session_load(cfg->session);
      if (cfg->session->login_pending) {
        int pr = ng_session_poll_login(cfg->session);
        if (pr == 1)
          ng_log("auth: browser approved session (parent poll)");
      }
    }
    /* Reload backend from env each accept (settings writes env in a child;
     * without this, fork COW keeps parent stuck on the old base_url forever). */
    if (cfg->agent) {
      char envpath[640];
      snprintf(envpath, sizeof envpath, "%s/env", ng_workdir());
      ng_agent_load_env(cfg->agent, envpath);
    }

    pid_t pid = fork();
    if (pid < 0) {
      ng_log("fork: %s — handling inline", strerror(errno));
      handle_client(cfd, cfg);
      continue;
    }
    if (pid == 0) {
      close(sfd);
      signal(SIGCHLD, SIG_DFL);
      handle_client(cfd, cfg);
      _exit(0);
    }
    close(cfd);
    g_live_children++;
    reap_children();
    } /* while !stop */
  } /* last_sub_reap scope */
  for (;;) {
    int st = 0;
    if (waitpid(-1, &st, WNOHANG) <= 0) break;
  }
  ng_subagent_reap_all();
  close(sfd);
  return 0;
}
