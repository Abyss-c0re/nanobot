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
        !strcmp(path, "/favicon") || !strcmp(path, "/favicon.svg") ||
        !strcmp(path, "/apple-touch-icon.png") ||
        !strcmp(path, "/apple-touch-icon-precomposed.png") ||
        !strcmp(path, "/apple-touch-icon") ||
        !strcmp(path, "/swagger.json") ||
        !strcmp(path, "/swagger") || !strcmp(path, "/docs") ||
        !strcmp(path, "/api/docs") || !strcmp(path, "/robots.txt") ||
        !strcmp(path, "/security.txt") || !strcmp(path, "/.well-known/security.txt") ||
        !strcmp(path, "/trust.txt") || !strcmp(path, "/.well-known/trust.txt") ||
        !strcmp(path, "/keybase.txt") || !strcmp(path, "/.well-known/keybase.txt") ||
        !strcmp(path, "/pgp-key.txt") || !strcmp(path, "/.well-known/pgp-key.txt") ||
        !strncmp(path, "/.well-known/openpgpkey", 24) ||
        !strncmp(path, "/openpgpkey", 11) ||
        !strcmp(path, "/sshfp") || !strcmp(path, "/.well-known/sshfp") ||
        !strcmp(path, "/sshfp.json") || !strcmp(path, "/.well-known/sshfp.json") ||
        !strcmp(path, "/jwks.json") || !strcmp(path, "/.well-known/jwks.json") ||
        !strcmp(path, "/jwks") || !strcmp(path, "/.well-known/jwks") ||
        !strcmp(path, "/.well-known/related-website-set.json") ||
        !strcmp(path, "/related-website-set.json") ||
        !strcmp(path, "/.well-known/microsoft-identity-association.json") ||
        !strcmp(path, "/microsoft-identity-association.json") ||
        !strcmp(path, "/.well-known/apple-developer-merchantid-domain-association") ||
        !strcmp(path, "/apple-developer-merchantid-domain-association") ||
        !strcmp(path, "/.well-known/nostr.json") || !strcmp(path, "/.well-known/nostr") ||
        !strcmp(path, "/nostr.json") || !strcmp(path, "/nostr") ||
        !strcmp(path, "/.well-known/atproto-did") || !strcmp(path, "/atproto-did") ||
        !strcmp(path, "/.well-known/stellar.toml") || !strcmp(path, "/stellar.toml") ||
        !strcmp(path, "/.well-known/web-identity") || !strcmp(path, "/web-identity") ||
        !strncmp(path, "/.well-known/posh", 16) || !strncmp(path, "/posh", 5) ||
        !strcmp(path, "/.well-known/traffic-advice") ||
        !strcmp(path, "/traffic-advice") ||
        !strcmp(path, "/.well-known/privacy-sandbox-attestations.json") ||
        !strcmp(path, "/.well-known/privacy-sandbox-attestations") ||
        !strcmp(path, "/privacy-sandbox-attestations.json") ||
        !strcmp(path,
                "/.well-known/resource-that-should-not-be-used-for-federation") ||
        !strcmp(path, "/resource-that-should-not-be-used-for-federation") ||
        !strncmp(path, "/.well-known/appspecific/", 25) ||
        !strcmp(path, "/com.chrome.devtools.json") ||
        !strcmp(path, "/.well-known/http-opportunistic") ||
        !strcmp(path, "/http-opportunistic") ||
        !strcmp(path, "/.well-known/core") || !strcmp(path, "/core") ||
        !strncmp(path, "/.well-known/mercure", 20) ||
        !strncmp(path, "/mercure", 8) ||
        !strcmp(path, "/.well-known/gnap-as-rs") ||
        !strcmp(path, "/gnap-as-rs") ||
        !strncmp(path, "/.well-known/csaf", 17) ||
        !strcmp(path, "/csaf/provider-metadata.json") ||
        !strcmp(path, "/.well-known/discord") || !strcmp(path, "/discord") ||
        !strcmp(path, "/.well-known/jmap") || !strcmp(path, "/jmap") ||
        !strcmp(path, "/.well-known/stun-key") || !strcmp(path, "/stun-key") ||
        !strcmp(path, "/.well-known/thread") || !strcmp(path, "/thread") ||
        !strcmp(path, "/.well-known/coap") || !strcmp(path, "/coap") ||
        !strcmp(path, "/.well-known/time") || !strcmp(path, "/time") ||
        !strcmp(path, "/.well-known/timezone") || !strcmp(path, "/timezone") ||
        !strncmp(path, "/.well-known/est", 16) || !strncmp(path, "/est", 4) ||
        !strncmp(path, "/.well-known/pki-validation", 27) ||
        !strncmp(path, "/pki-validation", 15) ||
        !strcmp(path, "/.well-known/looking-glass") ||
        !strcmp(path, "/looking-glass") ||
        !strncmp(path, "/.well-known/genid", 18) ||
        !strncmp(path, "/genid", 6) ||
        !strncmp(path, "/.well-known/acme-challenge", 27) ||
        !strncmp(path, "/acme-challenge", 15) ||
        !strcmp(path, "/.well-known/ni") || !strncmp(path, "/.well-known/ni/", 16) ||
        !strcmp(path, "/.well-known/ni.json") ||
        !strcmp(path, "/ni") || !strncmp(path, "/ni/", 4) ||
        !strcmp(path, "/ni.json") || !strcmp(path, "/api/ni") ||
        !strcmp(path, "/peer/v1/ni") ||
        !strcmp(path, "/.well-known/vapid") || !strncmp(path, "/.well-known/vapid/", 18) ||
        !strcmp(path, "/.well-known/vapid.json") ||
        !strcmp(path, "/vapid") || !strncmp(path, "/vapid/", 7) ||
        !strcmp(path, "/vapid.json") || !strcmp(path, "/api/vapid") ||
        !strcmp(path, "/peer/v1/vapid") ||
        !strcmp(path, "/.well-known/hoba") || !strncmp(path, "/.well-known/hoba/", 17) ||
        !strcmp(path, "/.well-known/hoba.json") ||
        !strcmp(path, "/hoba") || !strncmp(path, "/hoba/", 6) ||
        !strcmp(path, "/hoba.json") || !strcmp(path, "/api/hoba") ||
        !strcmp(path, "/peer/v1/hoba") ||
        !strcmp(path, "/.well-known/smime-aia") || !strncmp(path, "/.well-known/smime-aia/", 22) ||
        !strcmp(path, "/.well-known/smime-aia.json") ||
        !strcmp(path, "/smime-aia") || !strncmp(path, "/smime-aia/", 11) ||
        !strcmp(path, "/smime-aia.json") || !strcmp(path, "/api/smime-aia") ||
        !strcmp(path, "/peer/v1/smime-aia") ||
        !strcmp(path, "/.well-known/browserid") || !strncmp(path, "/.well-known/browserid/", 22) ||
        !strcmp(path, "/.well-known/browserid.json") ||
        !strcmp(path, "/browserid") || !strncmp(path, "/browserid/", 11) ||
        !strcmp(path, "/browserid.json") || !strcmp(path, "/api/browserid") ||
        !strcmp(path, "/peer/v1/browserid") ||
        !strcmp(path, "/.well-known/idp-proxy") || !strncmp(path, "/.well-known/idp-proxy/", 22) ||
        !strcmp(path, "/.well-known/idp-proxy.json") ||
        !strcmp(path, "/idp-proxy") || !strncmp(path, "/idp-proxy/", 11) ||
        !strcmp(path, "/idp-proxy.json") || !strcmp(path, "/api/idp-proxy") ||
        !strcmp(path, "/peer/v1/idp-proxy") ||
        !strcmp(path, "/.well-known/dnt") || !strncmp(path, "/.well-known/dnt/", 16) ||
        !strcmp(path, "/.well-known/dnt.json") ||
        !strcmp(path, "/dnt") || !strncmp(path, "/dnt/", 5) ||
        !strcmp(path, "/dnt.json") || !strcmp(path, "/api/dnt") ||
        !strcmp(path, "/peer/v1/dnt") ||
        !strcmp(path, "/.well-known/funding-manifest-urls") ||
        !strncmp(path, "/.well-known/funding-manifest-urls/", 33) ||
        !strcmp(path, "/.well-known/funding-manifest-urls.json") ||
        !strcmp(path, "/funding-manifest-urls") ||
        !strncmp(path, "/funding-manifest-urls/", 23) ||
        !strcmp(path, "/funding-manifest-urls.json") ||
        !strcmp(path, "/api/funding-manifest-urls") ||
        !strcmp(path, "/peer/v1/funding-manifest-urls") ||
        !strcmp(path, "/.well-known/xrpc-server-did") ||
        !strncmp(path, "/.well-known/xrpc-server-did/", 28) ||
        !strcmp(path, "/.well-known/xrpc-server-did.json") ||
        !strcmp(path, "/xrpc-server-did") ||
        !strncmp(path, "/xrpc-server-did/", 17) ||
        !strcmp(path, "/xrpc-server-did.json") ||
        !strcmp(path, "/api/xrpc-server-did") ||
        !strcmp(path, "/peer/v1/xrpc-server-did") ||
        !strcmp(path, "/.well-known/mcp.json") ||
        !strncmp(path, "/.well-known/mcp.json/", 20) ||
        !strcmp(path, "/.well-known/mcp") ||
        !strncmp(path, "/.well-known/mcp/", 16) ||
        !strcmp(path, "/mcp.json") ||
        !strncmp(path, "/mcp.json/", 10) ||
        !strcmp(path, "/api/mcp.json") ||
        !strcmp(path, "/peer/v1/mcp.json") ||
        !strcmp(path, "/.well-known/web-bot-auth") ||
        !strncmp(path, "/.well-known/web-bot-auth/", 25) ||
        !strcmp(path, "/.well-known/web-bot-auth.json") ||
        !strcmp(path, "/web-bot-auth") ||
        !strncmp(path, "/web-bot-auth/", 14) ||
        !strcmp(path, "/web-bot-auth.json") ||
        !strcmp(path, "/api/web-bot-auth") ||
        !strcmp(path, "/peer/v1/web-bot-auth") ||
        !strcmp(path, "/.well-known/sbom") ||
        !strncmp(path, "/.well-known/sbom/", 17) ||
        !strcmp(path, "/.well-known/sbom.json") ||
        !strcmp(path, "/.well-known/supply-chain") ||
        !strncmp(path, "/.well-known/supply-chain/", 25) ||
        !strcmp(path, "/sbom") ||
        !strncmp(path, "/sbom/", 6) ||
        !strcmp(path, "/sbom.json") ||
        !strcmp(path, "/api/sbom") ||
        !strcmp(path, "/peer/v1/sbom") ||
        !strcmp(path, "/.well-known/privacy-pass") ||
        !strncmp(path, "/.well-known/privacy-pass/", 25) ||
        !strcmp(path, "/.well-known/privacy-pass.json") ||
        !strcmp(path, "/privacy-pass") ||
        !strncmp(path, "/privacy-pass/", 14) ||
        !strcmp(path, "/privacy-pass.json") ||
        !strcmp(path, "/api/privacy-pass") ||
        !strcmp(path, "/peer/v1/privacy-pass") ||
        !strcmp(path, "/.well-known/ohttp-gateway") ||
        !strncmp(path, "/.well-known/ohttp-gateway/", 26) ||
        !strcmp(path, "/.well-known/ohttp-gateway.json") ||
        !strcmp(path, "/.well-known/ohttp-config") ||
        !strncmp(path, "/.well-known/ohttp-config/", 25) ||
        !strcmp(path, "/.well-known/ohttp-config.json") ||
        !strcmp(path, "/ohttp-gateway") ||
        !strncmp(path, "/ohttp-gateway/", 15) ||
        !strcmp(path, "/ohttp-config") ||
        !strcmp(path, "/api/ohttp-gateway") ||
        !strcmp(path, "/peer/v1/ohttp-gateway") ||
        !strcmp(path, "/.well-known/masque") ||
        !strncmp(path, "/.well-known/masque/", 19) ||
        !strcmp(path, "/.well-known/masque.json") ||
        !strcmp(path, "/masque") ||
        !strncmp(path, "/masque/", 8) ||
        !strcmp(path, "/masque.json") ||
        !strcmp(path, "/api/masque") ||
        !strcmp(path, "/peer/v1/masque") ||
        !strcmp(path, "/.well-known/doh") ||
        !strncmp(path, "/.well-known/doh/", 16) ||
        !strcmp(path, "/.well-known/doh.json") ||
        !strcmp(path, "/.well-known/dot") ||
        !strncmp(path, "/.well-known/dot/", 16) ||
        !strcmp(path, "/.well-known/dot.json") ||
        !strcmp(path, "/doh") ||
        !strncmp(path, "/doh/", 5) ||
        !strcmp(path, "/dot") ||
        !strncmp(path, "/dot/", 5) ||
        !strcmp(path, "/api/doh") ||
        !strcmp(path, "/peer/v1/doh") ||
        !strcmp(path, "/api/dot") ||
        !strcmp(path, "/peer/v1/dot") ||
        !strcmp(path, "/.well-known/bluesky") ||
        !strncmp(path, "/.well-known/bluesky/", 20) ||
        !strcmp(path, "/.well-known/bluesky.json") ||
        !strcmp(path, "/bluesky") ||
        !strncmp(path, "/bluesky/", 9) ||
        !strcmp(path, "/bluesky.json") ||
        !strcmp(path, "/api/bluesky") ||
        !strcmp(path, "/peer/v1/bluesky") ||
        !strcmp(path, "/.well-known/solid") ||
        !strncmp(path, "/.well-known/solid/", 18) ||
        !strcmp(path, "/.well-known/solid.json") ||
        !strcmp(path, "/solid") ||
        !strncmp(path, "/solid/", 7) ||
        !strcmp(path, "/solid.json") ||
        !strcmp(path, "/api/solid") ||
        !strcmp(path, "/peer/v1/solid") ||
        !strcmp(path, "/.well-known/web-app-origin-association") ||
        !strncmp(path, "/.well-known/web-app-origin-association/", 39) ||
        !strcmp(path, "/.well-known/web-app-origin-association.json") ||
        !strcmp(path, "/web-app-origin-association") ||
        !strncmp(path, "/web-app-origin-association/", 28) ||
        !strcmp(path, "/web-app-origin-association.json") ||
        !strcmp(path, "/api/web-app-origin-association") ||
        !strcmp(path, "/peer/v1/web-app-origin-association") ||
        !strcmp(path, "/.well-known/doq") ||
        !strncmp(path, "/.well-known/doq/", 16) ||
        !strcmp(path, "/.well-known/doq.json") ||
        !strcmp(path, "/.well-known/dns-query") ||
        !strncmp(path, "/.well-known/dns-query/", 22) ||
        !strcmp(path, "/.well-known/dns-query.json") ||
        !strcmp(path, "/doq") ||
        !strncmp(path, "/doq/", 5) ||
        !strcmp(path, "/doq.json") ||
        !strcmp(path, "/dns-query") ||
        !strncmp(path, "/dns-query/", 11) ||
        !strcmp(path, "/dns-query.json") ||
        !strcmp(path, "/api/doq") ||
        !strcmp(path, "/peer/v1/doq") ||
        !strcmp(path, "/api/dns-query") ||
        !strcmp(path, "/peer/v1/dns-query") ||
        !strcmp(path, "/.well-known/activitypub") ||
        !strncmp(path, "/.well-known/activitypub/", 24) ||
        !strcmp(path, "/.well-known/activitypub.json") ||
        !strcmp(path, "/activitypub") ||
        !strncmp(path, "/activitypub/", 13) ||
        !strcmp(path, "/activitypub.json") ||
        !strcmp(path, "/api/activitypub") ||
        !strcmp(path, "/peer/v1/activitypub") ||
        !strcmp(path, "/.well-known/a2a") ||
        !strncmp(path, "/.well-known/a2a/", 16) ||
        !strcmp(path, "/.well-known/a2a.json") ||
        !strcmp(path, "/.well-known/a2a-agent-card.json") ||
        !strcmp(path, "/.well-known/agent-card") ||
        !strcmp(path, "/a2a") ||
        !strncmp(path, "/a2a/", 5) ||
        !strcmp(path, "/a2a.json") ||
        !strcmp(path, "/api/a2a") ||
        !strcmp(path, "/peer/v1/a2a") ||
        !strcmp(path, "/.well-known/token-issuer-directory") ||
        !strncmp(path, "/.well-known/token-issuer-directory/", 34) ||
        !strcmp(path, "/.well-known/token-issuer-directory.json") ||
        !strcmp(path, "/.well-known/private-token-issuer-directory") ||
        !strncmp(path, "/.well-known/private-token-issuer-directory/", 42) ||
        !strcmp(path, "/token-issuer-directory") ||
        !strcmp(path, "/private-token-issuer-directory") ||
        !strcmp(path, "/api/token-issuer-directory") ||
        !strcmp(path, "/peer/v1/token-issuer-directory") ||
        !strcmp(path, "/api/private-token-issuer-directory") ||
        !strcmp(path, "/peer/v1/private-token-issuer-directory") ||
        !strcmp(path, "/.well-known/tls-rpt") ||
        !strncmp(path, "/.well-known/tls-rpt/", 20) ||
        !strcmp(path, "/.well-known/tls-rpt.json") ||
        !strcmp(path, "/tls-rpt") ||
        !strcmp(path, "/tls-rpt.json") ||
        !strcmp(path, "/api/tls-rpt") ||
        !strcmp(path, "/peer/v1/tls-rpt") ||
        !strcmp(path, "/.well-known/bimi") ||
        !strncmp(path, "/.well-known/bimi/", 17) ||
        !strcmp(path, "/.well-known/bimi.json") ||
        !strcmp(path, "/bimi") ||
        !strncmp(path, "/bimi/", 6) ||
        !strcmp(path, "/bimi.json") ||
        !strcmp(path, "/api/bimi") ||
        !strcmp(path, "/peer/v1/bimi") ||
        !strcmp(path, "/.well-known/statements.json") ||
        !strncmp(path, "/.well-known/statements.json/", 28) ||
        !strcmp(path, "/.well-known/statements") ||
        !strcmp(path, "/statements.json") ||
        !strcmp(path, "/api/statements.json") ||
        !strcmp(path, "/peer/v1/statements.json") ||
        !strcmp(path, "/api/statements") ||
        !strcmp(path, "/peer/v1/statements") ||
        !strcmp(path, "/manifest.json") || !strcmp(path, "/manifest.webmanifest") ||
        !strcmp(path, "/site.webmanifest") ||
        !strcmp(path, "/humans.txt") || !strcmp(path, "/sitemap.xml") ||
        !strcmp(path, "/sitemap_index.xml") ||
        !strcmp(path, "/llms.txt") || !strcmp(path, "/ai.txt") ||
        !strcmp(path, "/service-worker.js") || !strcmp(path, "/sw.js") ||
        !strcmp(path, "/ads.txt") || !strcmp(path, "/app-ads.txt") ||
        !strcmp(path, "/crossdomain.xml") ||
        !strcmp(path, "/clientaccesspolicy.xml") ||
        !strcmp(path, "/browserconfig.xml") ||
        !strcmp(path, "/.well-known/change-password") ||
        !strcmp(path, "/change-password") ||
        !strcmp(path, "/sellers.json") ||
        !strcmp(path, "/.well-known/ai-plugin.json") ||
        !strcmp(path, "/ai-plugin.json") ||
        !strcmp(path, "/.well-known/assetlinks.json") ||
        !strcmp(path, "/assetlinks.json") ||
        !strcmp(path, "/.well-known/apple-app-site-association") ||
        !strcmp(path, "/apple-app-site-association") ||
        !strcmp(path, "/.well-known/gpc.json") ||
        !strcmp(path, "/gpc.json") ||
        !strcmp(path, "/.well-known/dnt-policy.txt") ||
        !strcmp(path, "/dnt-policy.txt") ||
        !strcmp(path, "/.well-known/passkey-endpoints") ||
        !strcmp(path, "/passkey-endpoints") ||
        !strcmp(path, "/.well-known/webfinger") ||
        !strcmp(path, "/webfinger") ||
        !strcmp(path, "/.well-known/nodeinfo") ||
        !strcmp(path, "/nodeinfo") ||
        !strcmp(path, "/.well-known/host-meta") ||
        !strcmp(path, "/.well-known/host-meta.json") ||
        !strcmp(path, "/host-meta") ||
        !strcmp(path, "/host-meta.json") ||
        !strcmp(path, "/.well-known/matrix/client") ||
        !strcmp(path, "/.well-known/matrix/server") ||
        !strcmp(path, "/matrix/client") ||
        !strcmp(path, "/matrix/server") ||
        !strcmp(path, "/.well-known/tdmrep.json") ||
        !strcmp(path, "/tdmrep.json") ||
        !strcmp(path, "/.well-known/mta-sts.txt") ||
        !strcmp(path, "/mta-sts.txt") ||
        !strcmp(path, "/.well-known/mta-sts") ||
        !strncmp(path, "/.well-known/mta-sts/", 20) ||
        !strcmp(path, "/mta-sts") ||
        !strcmp(path, "/api/mta-sts") ||
        !strcmp(path, "/peer/v1/mta-sts") ||
        !strcmp(path, "/.well-known/caldav") ||
        !strcmp(path, "/.well-known/carddav") ||
        !strcmp(path, "/caldav") ||
        !strcmp(path, "/carddav") ||
        !strcmp(path, "/.well-known/api-catalog") ||
        !strcmp(path, "/api-catalog") ||
        !strcmp(path, "/.well-known/agent-card.json") ||
        !strcmp(path, "/.well-known/agent.json") ||
        !strcmp(path, "/agent-card.json") ||
        !strcmp(path, "/agent.json") ||
        !strcmp(path, "/.well-known/openid-configuration") ||
        !strcmp(path, "/.well-known/openid-configuration.json") ||
        !strcmp(path, "/openid-configuration") ||
        !strcmp(path, "/openid-configuration.json") ||
        !strcmp(path, "/.well-known/openid-federation") ||
        !strcmp(path, "/openid-federation") ||
        !strcmp(path, "/.well-known/uma2-configuration") ||
        !strcmp(path, "/uma2-configuration") ||
        !strcmp(path, "/.well-known/openid-credential-issuer") ||
        !strcmp(path, "/openid-credential-issuer") ||
        !strcmp(path, "/.well-known/fido2-configuration") ||
        !strcmp(path, "/fido2-configuration") ||
        !strcmp(path, "/.well-known/webauthn") ||
        !strcmp(path, "/webauthn") ||
        !strcmp(path, "/.well-known/did.json") ||
        !strcmp(path, "/did.json") ||
        !strcmp(path, "/.well-known/did-configuration") ||
        !strcmp(path, "/did-configuration") ||
        !strcmp(path, "/.well-known/oauth-authorization-server") ||
        !strcmp(path, "/.well-known/oauth-authorization-server.json") ||
        !strcmp(path, "/oauth-authorization-server") ||
        !strcmp(path, "/oauth-authorization-server.json") ||
        !strcmp(path, "/.well-known/oauth-client-registration") ||
        !strcmp(path, "/oauth-client-registration") ||
        !strcmp(path, "/.well-known/oauth-protected-resource") ||
        !strcmp(path, "/.well-known/oauth-protected-resource.json") ||
        !strcmp(path, "/oauth-protected-resource") ||
        !strcmp(path, "/oauth-protected-resource.json"))
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
   * Residual: /whoami|/api/whoami|/peer/v1/whoami not_found — identity probes.
   * Residual: bare /status 404 while /api/status and /peer/v1/status already auth. */
  if (is_get && (strcmp(path, "/api/auth") == 0 || strcmp(path, "/api/auth/") == 0 ||
                 strcmp(path, "/api/status") == 0 || strcmp(path, "/api/status/") == 0 ||
                 strcmp(path, "/peer/v1/auth") == 0 || strcmp(path, "/peer/v1/auth/") == 0 ||
                 strcmp(path, "/peer/v1/status") == 0 ||
                 strcmp(path, "/peer/v1/status/") == 0 ||
                 strcmp(path, "/status") == 0 || strcmp(path, "/status/") == 0 ||
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
   * Residual: /ping|/api/ping|/peer/v1/ping not_found while health already lives.
   * Residual: k8s-style /livez|/readyz|/healthz|/alive (+ dual-wire) not_found. */
  if (is_get && (strcmp(path, "/peer/v1/health") == 0 ||
                 strcmp(path, "/peer/v1/health/") == 0 ||
                 strcmp(path, "/health") == 0 ||
                 strcmp(path, "/health/") == 0 ||
                 strcmp(path, "/api/health") == 0 ||
                 strcmp(path, "/api/health/") == 0 ||
                 strcmp(path, "/api/v1/health") == 0 ||
                 strcmp(path, "/api/v1/health/") == 0 ||
                 strcmp(path, "/.well-known/health") == 0 ||
                 strcmp(path, "/.well-known/health/") == 0 ||
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
                 strcmp(path, "/peer/v1/ping/") == 0 ||
                 strcmp(path, "/livez") == 0 || strcmp(path, "/livez/") == 0 ||
                 strcmp(path, "/api/livez") == 0 || strcmp(path, "/api/livez/") == 0 ||
                 strcmp(path, "/peer/v1/livez") == 0 ||
                 strcmp(path, "/peer/v1/livez/") == 0 ||
                 strcmp(path, "/readyz") == 0 || strcmp(path, "/readyz/") == 0 ||
                 strcmp(path, "/api/readyz") == 0 || strcmp(path, "/api/readyz/") == 0 ||
                 strcmp(path, "/peer/v1/readyz") == 0 ||
                 strcmp(path, "/peer/v1/readyz/") == 0 ||
                 strcmp(path, "/healthz") == 0 || strcmp(path, "/healthz/") == 0 ||
                 strcmp(path, "/api/healthz") == 0 ||
                 strcmp(path, "/api/healthz/") == 0 ||
                 strcmp(path, "/peer/v1/healthz") == 0 ||
                 strcmp(path, "/peer/v1/healthz/") == 0 ||
                 strcmp(path, "/alive") == 0 || strcmp(path, "/alive/") == 0 ||
                 strcmp(path, "/api/alive") == 0 || strcmp(path, "/api/alive/") == 0 ||
                 strcmp(path, "/peer/v1/alive") == 0 ||
                 strcmp(path, "/peer/v1/alive/") == 0)) {
    char body[640];
    char *ver = ng_json_escape(NG_VERSION);
    int jn = jobs_meta_count();
    int is_ready = (strcmp(path, "/ready") == 0 ||
                    strcmp(path, "/ready/") == 0 ||
                    strcmp(path, "/peer/v1/ready") == 0 ||
                    strcmp(path, "/peer/v1/ready/") == 0 ||
                    strcmp(path, "/api/ready") == 0 ||
                    strcmp(path, "/api/ready/") == 0);
    int is_readyz = (strcmp(path, "/readyz") == 0 || strcmp(path, "/readyz/") == 0 ||
                     strcmp(path, "/api/readyz") == 0 ||
                     strcmp(path, "/api/readyz/") == 0 ||
                     strcmp(path, "/peer/v1/readyz") == 0 ||
                     strcmp(path, "/peer/v1/readyz/") == 0);
    int is_ping = (strcmp(path, "/ping") == 0 ||
                   strcmp(path, "/ping/") == 0 ||
                   strcmp(path, "/api/ping") == 0 ||
                   strcmp(path, "/api/ping/") == 0 ||
                   strcmp(path, "/peer/v1/ping") == 0 ||
                   strcmp(path, "/peer/v1/ping/") == 0);
    int is_livez = (strcmp(path, "/livez") == 0 || strcmp(path, "/livez/") == 0 ||
                    strcmp(path, "/api/livez") == 0 || strcmp(path, "/api/livez/") == 0 ||
                    strcmp(path, "/peer/v1/livez") == 0 ||
                    strcmp(path, "/peer/v1/livez/") == 0 ||
                    strcmp(path, "/alive") == 0 || strcmp(path, "/alive/") == 0 ||
                    strcmp(path, "/api/alive") == 0 || strcmp(path, "/api/alive/") == 0 ||
                    strcmp(path, "/peer/v1/alive") == 0 ||
                    strcmp(path, "/peer/v1/alive/") == 0);
    int is_healthz = (strcmp(path, "/healthz") == 0 || strcmp(path, "/healthz/") == 0 ||
                      strcmp(path, "/api/healthz") == 0 ||
                      strcmp(path, "/api/healthz/") == 0 ||
                      strcmp(path, "/peer/v1/healthz") == 0 ||
                      strcmp(path, "/peer/v1/healthz/") == 0);
    const char *act = is_readyz ? "readyz"
                      : is_ready ? "ready"
                      : is_ping ? "ping"
                      : is_livez ? "livez"
                      : is_healthz ? "healthz"
                      : "health";
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

  /* Residual: mesh probes hit /uptime|/api/uptime|/peer/v1/uptime and got
   * not_found while health already exposes started/pid. */
  if (is_get && (strcmp(path, "/uptime") == 0 || strcmp(path, "/uptime/") == 0 ||
                 strcmp(path, "/api/uptime") == 0 || strcmp(path, "/api/uptime/") == 0 ||
                 strcmp(path, "/peer/v1/uptime") == 0 ||
                 strcmp(path, "/peer/v1/uptime/") == 0)) {
    char body[640];
    char *ver = ng_json_escape(NG_VERSION);
    time_t now = time(NULL);
    long started = (long)g_serve_started;
    long up = (started > 0 && now >= (time_t)started) ? (long)(now - (time_t)started) : 0;
    int n = snprintf(body, sizeof body,
      "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,\"action\":\"uptime\","
      "\"service\":\"nanobot-peer\",\"version\":\"%s\","
      "\"pid\":%d,\"started\":%ld,\"uptime_sec\":%ld,"
      NG_PEER_HTTP_DUAL_WIRE "}",
      ver ? ver : "",
      (int)(g_serve_pid ? g_serve_pid : getpid()),
      started, up);
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
        "\"/version\",\"/metrics\",\"/whoami\",\"/capabilities\",\"/schema\""
      "],"
      NG_PEER_HTTP_DUAL_WIRE "}",
      ver ? ver : "");
    free(ver);
    http_response(cfd, 200, "application/json", body, (size_t)n);
    free(req); close(cfd); return;
  }

  /* Residual: mesh probes hit /schema|/api/schema|/peer/v1/schema and got
   * not_found while openapi/capabilities already list wire plates. */
  if (is_get && (strcmp(path, "/schema") == 0 || strcmp(path, "/schema/") == 0 ||
                 strcmp(path, "/api/schema") == 0 || strcmp(path, "/api/schema/") == 0 ||
                 strcmp(path, "/peer/v1/schema") == 0 ||
                 strcmp(path, "/peer/v1/schema/") == 0)) {
    /* Grow with well-known action catalog; 2048 headroom tight at ~sbom (2026-08-10). */
    char body[3072];
    char *ver = ng_json_escape(NG_VERSION);
    int n = snprintf(body, sizeof body,
      "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":true,\"action\":\"schema\","
      "\"service\":\"nanobot-peer\",\"version\":\"%s\","
      "\"openapi\":\"/openapi.json\","
      "\"plates\":["
        "\"nanobot.peer_http.v1\","
        "\"nanobot.auth.v1\","
        "\"nanobot.shell.v1\","
        "\"nanobot.task.v1\","
        "\"nanobot.task_reminder.v1\","
        "\"nanobot.mcp.v1\","
        "\"nanobot.braincell.v1\""
      "],"
      "\"actions\":["
        "\"health\",\"ready\",\"ping\",\"livez\",\"readyz\",\"healthz\","
        "\"info\",\"version\",\"uptime\",\"capabilities\",\"schema\","
        "\"metrics\",\"whoami\",\"status\",\"openapi\",\"manifest\","
        "\"robots\",\"security_txt\",\"humans\",\"sitemap\",\"llms\","
        "\"favicon\",\"service_worker\",\"ads\",\"crossdomain\","
        "\"browserconfig\",\"change_password\",\"sellers\","
        "\"apple_touch_icon\",\"ai_plugin\",\"assetlinks\","
        "\"apple_app_site_association\",\"gpc\","
        "\"openid_configuration\",\"oauth_authorization_server\","
        "\"oauth_protected_resource\",\"dnt_policy\","
        "\"passkey_endpoints\",\"webfinger\",\"nodeinfo\",\"host_meta\","
        "\"matrix_client\",\"matrix_server\",\"tdmrep\",\"mta_sts\","
        "\"caldav\",\"carddav\",\"api_catalog\",\"agent_card\","
        "\"oauth_client_registration\",\"openid_federation\","
        "\"uma2_configuration\",\"openid_credential_issuer\","
        "\"fido2_configuration\",\"webauthn\",\"did_json\","
        "\"did_configuration\",\"trust_txt\",\"keybase_txt\","
        "\"pgp_key_txt\",\"openpgpkey\",\"sshfp\",\"jwks\",\"related_website_set\",\"microsoft_identity_association\",\"apple_merchantid_domain_association\",\"nostr\",\"atproto_did\",\"stellar_toml\",\"web_identity\",\"posh\",\"traffic_advice\",\"privacy_sandbox_attestations\",\"no_federation\",\"chrome_devtools\",\"http_opportunistic\",\"core\",\"mercure\",\"gnap_as_rs\",\"csaf\",\"discord\",\"jmap\",\"stun_key\",\"thread\",\"coap\",\"time\",\"timezone\",\"est\",\"pki_validation\",\"looking_glass\",\"genid\",\"acme_challenge\",\"ni\",\"vapid\",\"hoba\",\"smime_aia\",\"browserid\",\"idp_proxy\",\"dnt\",\"funding_manifest_urls\",\"xrpc_server_did\",\"mcp_json\",\"web_bot_auth\",\"sbom\",\"privacy_pass\",\"ohttp_gateway\",\"masque\",\"doh\",\"bluesky\",\"solid\",\"web_app_origin_association\",\"doq\",\"activitypub\",\"a2a\",\"token_issuer_directory\",\"tls_rpt\",\"bimi\",\"statements\""
      "],"
      NG_PEER_HTTP_DUAL_WIRE "}",
      ver ? ver : "");
    free(ver);
    if (n < 0 || (size_t)n >= sizeof body) {
      http_response(cfd, 500, "application/json",
                    "{\"ok\":false,\"error\":\"schema_overflow\"}", 39);
      free(req); close(cfd); return;
    }
    http_response(cfd, 200, "application/json", body, (size_t)n);
    free(req); close(cfd); return;
  }

  /* Residual: mesh/browser probes hit /favicon.ico|/favicon.svg and got
   * not_found (noisy 404 without www_root). Tiny cyan-cube SVG.
   * Residual: Safari hits /apple-touch-icon(.png|-precomposed.png) same plate. */
  if (is_get && (strcmp(path, "/favicon.ico") == 0 || strcmp(path, "/favicon.ico/") == 0 ||
                 strcmp(path, "/favicon") == 0 || strcmp(path, "/favicon/") == 0 ||
                 strcmp(path, "/favicon.svg") == 0 || strcmp(path, "/favicon.svg/") == 0 ||
                 strcmp(path, "/api/favicon.svg") == 0 ||
                 strcmp(path, "/peer/v1/favicon.svg") == 0 ||
                 strcmp(path, "/api/favicon.ico") == 0 ||
                 strcmp(path, "/peer/v1/favicon.ico") == 0 ||
                 strcmp(path, "/apple-touch-icon.png") == 0 ||
                 strcmp(path, "/apple-touch-icon.png/") == 0 ||
                 strcmp(path, "/apple-touch-icon-precomposed.png") == 0 ||
                 strcmp(path, "/apple-touch-icon-precomposed.png/") == 0 ||
                 strcmp(path, "/apple-touch-icon") == 0 ||
                 strcmp(path, "/apple-touch-icon/") == 0 ||
                 strcmp(path, "/api/apple-touch-icon.png") == 0 ||
                 strcmp(path, "/peer/v1/apple-touch-icon.png") == 0)) {
    static const char favicon_svg[] =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 32 32\">"
      "<rect width=\"32\" height=\"32\" fill=\"#0a0a0a\"/>"
      "<rect x=\"6\" y=\"6\" width=\"20\" height=\"20\" fill=\"none\" "
      "stroke=\"#00e5ff\" stroke-width=\"2\"/>"
      "</svg>";
    http_response(cfd, 200, "image/svg+xml", favicon_svg, sizeof favicon_svg - 1);
    free(req); close(cfd); return;
  }

  /* Residual: browser/mesh probes hit /service-worker.js|/sw.js and got
   * not_found. Lab ops has no PWA — serve unregister-on-activate script. */
  if (is_get && (strcmp(path, "/service-worker.js") == 0 ||
                 strcmp(path, "/service-worker.js/") == 0 ||
                 strcmp(path, "/sw.js") == 0 || strcmp(path, "/sw.js/") == 0 ||
                 strcmp(path, "/api/service-worker.js") == 0 ||
                 strcmp(path, "/peer/v1/service-worker.js") == 0 ||
                 strcmp(path, "/api/sw.js") == 0 ||
                 strcmp(path, "/peer/v1/sw.js") == 0)) {
    static const char swjs[] =
      "/* nanobot peer HTTP — lab ops only (not product SMX2). No PWA. */\n"
      "self.addEventListener('install', function (e) { self.skipWaiting(); });\n"
      "self.addEventListener('activate', function (e) {\n"
      "  e.waitUntil(self.registration.unregister());\n"
      "});\n";
    http_response(cfd, 200, "application/javascript; charset=utf-8", swjs,
                  sizeof swjs - 1);
    free(req); close(cfd); return;
  }

  /* Residual: crawler/mesh probes hit /ads.txt|/app-ads.txt and got not_found.
   * Lab ops peer has no authorized ad sellers (IAB ads.txt / app-ads.txt). */
  if (is_get && (strcmp(path, "/ads.txt") == 0 || strcmp(path, "/ads.txt/") == 0 ||
                 strcmp(path, "/app-ads.txt") == 0 ||
                 strcmp(path, "/app-ads.txt/") == 0 ||
                 strcmp(path, "/api/ads.txt") == 0 ||
                 strcmp(path, "/peer/v1/ads.txt") == 0 ||
                 strcmp(path, "/api/app-ads.txt") == 0 ||
                 strcmp(path, "/peer/v1/app-ads.txt") == 0)) {
    static const char adstxt[] =
      "# nanobot peer HTTP — lab ops only (not product SMX2)\n"
      "# No authorized digital sellers. This is not a public ad inventory site.\n"
      "# CONTACT=https://github.com/Abyss-c0re/nanobot/security/advisories/new\n";
    http_response(cfd, 200, "text/plain; charset=utf-8", adstxt, sizeof adstxt - 1);
    free(req); close(cfd); return;
  }

  /* Residual: crawler/mesh probes hit /sellers.json and got not_found while
   * ads.txt already declares no sellers. IAB sellers.json empty plate. */
  if (is_get && (strcmp(path, "/sellers.json") == 0 ||
                 strcmp(path, "/sellers.json/") == 0 ||
                 strcmp(path, "/api/sellers.json") == 0 ||
                 strcmp(path, "/peer/v1/sellers.json") == 0)) {
    static const char sellers[] =
      "{"
      "\"version\":\"1.0\","
      "\"contact_email\":\"\","
      "\"contact_address\":\"\","
      "\"identifiers\":[],"
      "\"sellers\":[],"
      "\"x-nanobot\":{"
        "\"schema\":\"nanobot.peer_http.v1\","
        "\"action\":\"sellers\","
        "\"product_wire\":\"smx2\","
        "\"peer_http\":\"lab_ops_only\","
        "\"peer_http_is_product_bus\":false,"
        "\"share\":\"state_matrix_only\","
        "\"hold_flash\":1,"
        "\"llm_is_commander\":false,"
        "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", sellers, sizeof sellers - 1);
    free(req); close(cfd); return;
  }

  /* Residual: ChatGPT/mesh probes hit /.well-known/ai-plugin.json and got
   * not_found. Lab ops is not a public OpenAI plugin host — honest plate. */
  if (is_get && (strcmp(path, "/.well-known/ai-plugin.json") == 0 ||
                 strcmp(path, "/.well-known/ai-plugin.json/") == 0 ||
                 strcmp(path, "/ai-plugin.json") == 0 ||
                 strcmp(path, "/ai-plugin.json/") == 0 ||
                 strcmp(path, "/api/ai-plugin.json") == 0 ||
                 strcmp(path, "/peer/v1/ai-plugin.json") == 0)) {
    static const char aiplug[] =
      "{"
      "\"schema_version\":\"v1\","
      "\"name_for_human\":\"nanobot peer HTTP\","
      "\"name_for_model\":\"nanobot_peer_http\","
      "\"description_for_human\":\"Lab ops peer bus (not product SMX2). Not a public ChatGPT plugin.\","
      "\"description_for_model\":\"Private lab mesh endpoint. robots Disallow. Prefer GitHub docs. Not product SMX2.\","
      "\"auth\":{\"type\":\"none\"},"
      "\"api\":{"
        "\"type\":\"openapi\","
        "\"url\":\"/openapi.json\","
        "\"is_user_authenticated\":false"
      "},"
      "\"logo_url\":\"/favicon.svg\","
      "\"contact_email\":\"\","
      "\"legal_info_url\":\"https://github.com/Abyss-c0re/nanobot\","
      "\"x-nanobot\":{"
        "\"schema\":\"nanobot.peer_http.v1\","
        "\"action\":\"ai_plugin\","
        "\"product_wire\":\"smx2\","
        "\"peer_http\":\"lab_ops_only\","
        "\"peer_http_is_product_bus\":false,"
        "\"share\":\"state_matrix_only\","
        "\"hold_flash\":1,"
        "\"llm_is_commander\":false,"
        "\"python\":0,"
        "\"public_plugin\":false"
      "}"
      "}";
    http_response(cfd, 200, "application/json", aiplug, sizeof aiplug - 1);
    free(req); close(cfd); return;
  }

  /* Residual: A2A/mesh probes hit /.well-known/agent-card.json and got
   * not_found. Lab ops is not a public agent host — private card plate. */
  if (is_get && (strcmp(path, "/.well-known/agent-card.json") == 0 ||
                 strcmp(path, "/.well-known/agent-card.json/") == 0 ||
                 strcmp(path, "/.well-known/agent.json") == 0 ||
                 strcmp(path, "/.well-known/agent.json/") == 0 ||
                 strcmp(path, "/agent-card.json") == 0 ||
                 strcmp(path, "/agent-card.json/") == 0 ||
                 strcmp(path, "/agent.json") == 0 ||
                 strcmp(path, "/agent.json/") == 0 ||
                 strcmp(path, "/api/agent-card.json") == 0 ||
                 strcmp(path, "/peer/v1/agent-card.json") == 0)) {
    static const char acard[] =
      "{"
      "\"name\":\"nanobot peer HTTP\","
      "\"description\":\"Lab ops peer bus (not product SMX2). Not a public A2A agent.\","
      "\"url\":\"/peer/v1/health\","
      "\"provider\":{\"organization\":\"Abyss-c0re\",\"url\":\"https://github.com/Abyss-c0re/nanobot\"},"
      "\"version\":\"0.5.3\","
      "\"documentationUrl\":\"https://github.com/Abyss-c0re/nanobot\","
      "\"capabilities\":{"
        "\"streaming\":false,"
        "\"pushNotifications\":false,"
        "\"stateTransitionHistory\":false"
      "},"
      "\"authentication\":{\"schemes\":[]},"
      "\"defaultInputModes\":[\"text\"],"
      "\"defaultOutputModes\":[\"text\"],"
      "\"skills\":[],"
      "\"x-nanobot\":{"
        "\"schema\":\"nanobot.peer_http.v1\","
        "\"action\":\"agent_card\","
        "\"agent_card\":true,"
        "\"public_agent\":false,"
        "\"product_wire\":\"smx2\","
        "\"peer_http\":\"lab_ops_only\","
        "\"peer_http_is_product_bus\":false,"
        "\"share\":\"state_matrix_only\","
        "\"hold_flash\":1,"
        "\"llm_is_commander\":false,"
        "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", acard, sizeof acard - 1);
    free(req); close(cfd); return;
  }

  /* Residual: Android/mesh probes hit /.well-known/assetlinks.json and got
   * not_found. Lab ops has no app link statements — empty DAL array. */
  if (is_get && (strcmp(path, "/.well-known/assetlinks.json") == 0 ||
                 strcmp(path, "/.well-known/assetlinks.json/") == 0 ||
                 strcmp(path, "/assetlinks.json") == 0 ||
                 strcmp(path, "/assetlinks.json/") == 0 ||
                 strcmp(path, "/api/assetlinks.json") == 0 ||
                 strcmp(path, "/peer/v1/assetlinks.json") == 0)) {
    static const char assetlinks[] = "[]";
    http_response(cfd, 200, "application/json", assetlinks,
                  sizeof assetlinks - 1);
    free(req); close(cfd); return;
  }

  /* Residual: iOS/mesh probes hit apple-app-site-association and got not_found.
   * Lab ops has no Universal Links — empty AASA plate (companion to assetlinks). */
  if (is_get &&
      (strcmp(path, "/.well-known/apple-app-site-association") == 0 ||
       strcmp(path, "/.well-known/apple-app-site-association/") == 0 ||
       strcmp(path, "/apple-app-site-association") == 0 ||
       strcmp(path, "/apple-app-site-association/") == 0 ||
       strcmp(path, "/api/apple-app-site-association") == 0 ||
       strcmp(path, "/peer/v1/apple-app-site-association") == 0)) {
    static const char aasa[] =
      "{"
      "\"applinks\":{"
        "\"apps\":[],"
        "\"details\":[]"
      "},"
      "\"webcredentials\":{"
        "\"apps\":[]"
      "},"
      "\"x-nanobot\":{"
        "\"schema\":\"nanobot.peer_http.v1\","
        "\"action\":\"apple_app_site_association\","
        "\"product_wire\":\"smx2\","
        "\"peer_http\":\"lab_ops_only\","
        "\"peer_http_is_product_bus\":false,"
        "\"share\":\"state_matrix_only\","
        "\"hold_flash\":1,"
        "\"llm_is_commander\":false,"
        "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", aasa, sizeof aasa - 1);
    free(req); close(cfd); return;
  }

  /* Residual: privacy/mesh probes hit /.well-known/gpc.json and got not_found.
   * Lab ops honors Global Privacy Control (no sale/share of personal data). */
  if (is_get && (strcmp(path, "/.well-known/gpc.json") == 0 ||
                 strcmp(path, "/.well-known/gpc.json/") == 0 ||
                 strcmp(path, "/gpc.json") == 0 ||
                 strcmp(path, "/gpc.json/") == 0 ||
                 strcmp(path, "/api/gpc.json") == 0 ||
                 strcmp(path, "/peer/v1/gpc.json") == 0)) {
    static const char gpc[] =
      "{"
      "\"gpc\":true,"
      "\"x-nanobot\":{"
        "\"schema\":\"nanobot.peer_http.v1\","
        "\"action\":\"gpc\","
        "\"product_wire\":\"smx2\","
        "\"peer_http\":\"lab_ops_only\","
        "\"peer_http_is_product_bus\":false,"
        "\"share\":\"state_matrix_only\","
        "\"hold_flash\":1,"
        "\"llm_is_commander\":false,"
        "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", gpc, sizeof gpc - 1);
    free(req); close(cfd); return;
  }

  /* Residual: AI/mesh probes hit /.well-known/tdmrep.json (TDM Reservation)
   * and got not_found. Lab ops reserves all content against TDM (opt-out). */
  if (is_get && (strcmp(path, "/.well-known/tdmrep.json") == 0 ||
                 strcmp(path, "/.well-known/tdmrep.json/") == 0 ||
                 strcmp(path, "/tdmrep.json") == 0 ||
                 strcmp(path, "/tdmrep.json/") == 0 ||
                 strcmp(path, "/api/tdmrep.json") == 0 ||
                 strcmp(path, "/peer/v1/tdmrep.json") == 0)) {
    static const char tdm[] =
      "{"
      "\"version\":1,"
      "\"tdm-reservation\":\"all\","
      "\"x-nanobot\":{"
        "\"schema\":\"nanobot.peer_http.v1\","
        "\"action\":\"tdmrep\","
        "\"tdmrep\":true,"
        "\"product_wire\":\"smx2\","
        "\"peer_http\":\"lab_ops_only\","
        "\"peer_http_is_product_bus\":false,"
        "\"share\":\"state_matrix_only\","
        "\"hold_flash\":1,"
        "\"llm_is_commander\":false,"
        "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", tdm, sizeof tdm - 1);
    free(req); close(cfd); return;
  }

  /* Residual: mail/mesh probes hit /.well-known/mta-sts.txt (RFC 8461) and
   * got not_found. Lab ops is not an MTA — mode none policy plate. */
  if (is_get && (strcmp(path, "/.well-known/mta-sts.txt") == 0 ||
                 strcmp(path, "/.well-known/mta-sts.txt/") == 0 ||
                 strcmp(path, "/mta-sts.txt") == 0 ||
                 strcmp(path, "/mta-sts.txt/") == 0 ||
                 strcmp(path, "/api/mta-sts.txt") == 0 ||
                 strcmp(path, "/peer/v1/mta-sts.txt") == 0 ||
                 strcmp(path, "/.well-known/mta-sts") == 0 ||
                 strcmp(path, "/.well-known/mta-sts/") == 0 ||
                 strncmp(path, "/.well-known/mta-sts/", 20) == 0 ||
                 strcmp(path, "/mta-sts") == 0 ||
                 strcmp(path, "/mta-sts/") == 0 ||
                 strcmp(path, "/api/mta-sts") == 0 ||
                 strcmp(path, "/peer/v1/mta-sts") == 0)) {
    /* Prefer JSON dual-wire for extensionless probes; keep RFC 8461 text
     * for classic .txt paths. */
    int want_txt =
      (strcmp(path, "/.well-known/mta-sts.txt") == 0 ||
       strcmp(path, "/.well-known/mta-sts.txt/") == 0 ||
       strcmp(path, "/mta-sts.txt") == 0 ||
       strcmp(path, "/mta-sts.txt/") == 0 ||
       strcmp(path, "/api/mta-sts.txt") == 0 ||
       strcmp(path, "/peer/v1/mta-sts.txt") == 0);
    if (want_txt) {
      static const char mta[] =
        "version: STSv1\n"
        "mode: none\n"
        "max_age: 86400\n";
      http_response(cfd, 200, "text/plain; charset=utf-8", mta, sizeof mta - 1);
    } else {
      static const char mta_json[] =
        "{"
        "\"version\":\"STSv1\","
        "\"mode\":\"none\","
        "\"max_age\":86400,"
        "\"x-nanobot\":{"
        "\"schema\":\"nanobot.peer_http.v1\","
        "\"ok\":true,"
        "\"action\":\"mta_sts\","
        "\"mta_sts\":false,"
        "\"mode\":\"none\","
        "\"auth\":\"browser_device_code\","
        "\"auth_plate\":\"/api/auth\","
        "\"product_wire\":\"smx2\","
        "\"peer_http\":\"lab_ops_only\","
        "\"peer_http_is_product_bus\":false,"
        "\"share\":\"state_matrix_only\","
        "\"hold_flash\":1,"
        "\"llm_is_commander\":false,"
        "\"python\":0"
        "}"
        "}";
      http_response(cfd, 200, "application/json", mta_json, sizeof mta_json - 1);
    }
    free(req); close(cfd); return;
  }

  /* Residual: mesh/API probes hit /.well-known/api-catalog (RFC 9727) and
   * got not_found. Point catalog at live peer OpenAPI (lab ops only). */
  if (is_get && (strcmp(path, "/.well-known/api-catalog") == 0 ||
                 strcmp(path, "/.well-known/api-catalog/") == 0 ||
                 strcmp(path, "/api-catalog") == 0 ||
                 strcmp(path, "/api-catalog/") == 0 ||
                 strcmp(path, "/api/api-catalog") == 0 ||
                 strcmp(path, "/peer/v1/api-catalog") == 0)) {
    static const char cat[] =
      "{"
      "\"linkset\":[{"
        "\"anchor\":\"/\","
        "\"item\":["
          "{\"href\":\"/openapi.json\",\"type\":\"application/json\"},"
          "{\"href\":\"/openapi.yaml\",\"type\":\"application/yaml\"},"
          "{\"href\":\"/peer/v1/health\",\"type\":\"application/json\"},"
          "{\"href\":\"/peer/v1/info\",\"type\":\"application/json\"}"
        "]"
      "}],"
      "\"x-nanobot\":{"
        "\"schema\":\"nanobot.peer_http.v1\","
        "\"action\":\"api_catalog\","
        "\"api_catalog\":true,"
        "\"product_wire\":\"smx2\","
        "\"peer_http\":\"lab_ops_only\","
        "\"peer_http_is_product_bus\":false,"
        "\"share\":\"state_matrix_only\","
        "\"hold_flash\":1,"
        "\"llm_is_commander\":false,"
        "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/linkset+json", cat, sizeof cat - 1);
    free(req); close(cfd); return;
  }

  /* Residual: calendar/mesh probes hit /.well-known/caldav (RFC 6764) and
   * got not_found. Lab ops is not a CalDAV host — honest empty plate. */
  if (is_get && (strcmp(path, "/.well-known/caldav") == 0 ||
                 strcmp(path, "/.well-known/caldav/") == 0 ||
                 strcmp(path, "/caldav") == 0 ||
                 strcmp(path, "/caldav/") == 0 ||
                 strcmp(path, "/api/caldav") == 0 ||
                 strcmp(path, "/peer/v1/caldav") == 0)) {
    static const char cal[] =
      "{"
      "\"href\":\"\","
      "\"x-nanobot\":{"
        "\"schema\":\"nanobot.peer_http.v1\","
        "\"action\":\"caldav\","
        "\"caldav\":false,"
        "\"product_wire\":\"smx2\","
        "\"peer_http\":\"lab_ops_only\","
        "\"peer_http_is_product_bus\":false,"
        "\"share\":\"state_matrix_only\","
        "\"hold_flash\":1,"
        "\"llm_is_commander\":false,"
        "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", cal, sizeof cal - 1);
    free(req); close(cfd); return;
  }

  /* Residual: contacts/mesh probes hit /.well-known/carddav (RFC 6764) and
   * got not_found. Lab ops is not a CardDAV host — honest empty plate. */
  if (is_get && (strcmp(path, "/.well-known/carddav") == 0 ||
                 strcmp(path, "/.well-known/carddav/") == 0 ||
                 strcmp(path, "/carddav") == 0 ||
                 strcmp(path, "/carddav/") == 0 ||
                 strcmp(path, "/api/carddav") == 0 ||
                 strcmp(path, "/peer/v1/carddav") == 0)) {
    static const char card[] =
      "{"
      "\"href\":\"\","
      "\"x-nanobot\":{"
        "\"schema\":\"nanobot.peer_http.v1\","
        "\"action\":\"carddav\","
        "\"carddav\":false,"
        "\"product_wire\":\"smx2\","
        "\"peer_http\":\"lab_ops_only\","
        "\"peer_http_is_product_bus\":false,"
        "\"share\":\"state_matrix_only\","
        "\"hold_flash\":1,"
        "\"llm_is_commander\":false,"
        "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", card, sizeof card - 1);
    free(req); close(cfd); return;
  }

  /* Residual: privacy/mesh probes hit /.well-known/dnt-policy.txt and got
   * not_found while gpc.json already honors GPC. Short DNT honor plate. */
  if (is_get && (strcmp(path, "/.well-known/dnt-policy.txt") == 0 ||
                 strcmp(path, "/.well-known/dnt-policy.txt/") == 0 ||
                 strcmp(path, "/dnt-policy.txt") == 0 ||
                 strcmp(path, "/dnt-policy.txt/") == 0 ||
                 strcmp(path, "/api/dnt-policy.txt") == 0 ||
                 strcmp(path, "/peer/v1/dnt-policy.txt") == 0)) {
    static const char dnt[] =
      "# nanobot peer HTTP — lab ops only (not product SMX2)\n"
      "# Do Not Track (DNT) and Global Privacy Control (GPC) are honored.\n"
      "# No third-party tracking, ad networks, or sale of personal data.\n"
      "# Companion: /.well-known/gpc.json\n"
      "# Contact: https://github.com/Abyss-c0re/nanobot/security/advisories/new\n";
    http_response(cfd, 200, "text/plain; charset=utf-8", dnt, sizeof dnt - 1);
    free(req); close(cfd); return;
  }

  /* Residual: browser/mesh probes hit /.well-known/passkey-endpoints and got
   * not_found. Lab ops has no WebAuthn passkey host — empty plate; auth /api/auth. */
  if (is_get &&
      (strcmp(path, "/.well-known/passkey-endpoints") == 0 ||
       strcmp(path, "/.well-known/passkey-endpoints/") == 0 ||
       strcmp(path, "/passkey-endpoints") == 0 ||
       strcmp(path, "/passkey-endpoints/") == 0 ||
       strcmp(path, "/api/passkey-endpoints") == 0 ||
       strcmp(path, "/peer/v1/passkey-endpoints") == 0)) {
    static const char passkeys[] =
      "{"
      "\"enroll\":\"\","
      "\"manage\":\"\","
      "\"x-nanobot\":{"
        "\"schema\":\"nanobot.peer_http.v1\","
        "\"action\":\"passkey_endpoints\","
        "\"passkeys\":false,"
        "\"auth\":\"browser_device_code\","
        "\"auth_plate\":\"/api/auth\","
        "\"product_wire\":\"smx2\","
        "\"peer_http\":\"lab_ops_only\","
        "\"peer_http_is_product_bus\":false,"
        "\"share\":\"state_matrix_only\","
        "\"hold_flash\":1,"
        "\"llm_is_commander\":false,"
        "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", passkeys, sizeof passkeys - 1);
    free(req); close(cfd); return;
  }

  /* Residual: ActivityPub/mesh probes hit /.well-known/webfinger (RFC 7033)
   * and got not_found. Lab ops is not a WebFinger host — empty JRD plate. */
  if (is_get && (strcmp(path, "/.well-known/webfinger") == 0 ||
                 strcmp(path, "/.well-known/webfinger/") == 0 ||
                 strcmp(path, "/webfinger") == 0 ||
                 strcmp(path, "/webfinger/") == 0 ||
                 strcmp(path, "/api/webfinger") == 0 ||
                 strcmp(path, "/peer/v1/webfinger") == 0)) {
    static const char wf[] =
      "{"
      "\"subject\":\"\","
      "\"aliases\":[],"
      "\"links\":[],"
      "\"x-nanobot\":{"
        "\"schema\":\"nanobot.peer_http.v1\","
        "\"action\":\"webfinger\","
        "\"webfinger\":false,"
        "\"product_wire\":\"smx2\","
        "\"peer_http\":\"lab_ops_only\","
        "\"peer_http_is_product_bus\":false,"
        "\"share\":\"state_matrix_only\","
        "\"hold_flash\":1,"
        "\"llm_is_commander\":false,"
        "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/jrd+json", wf, sizeof wf - 1);
    free(req); close(cfd); return;
  }

  /* Residual: Fediverse/mesh probes hit /.well-known/nodeinfo (NodeInfo)
   * and got not_found. Lab ops is not a NodeInfo host — empty links plate. */
  if (is_get && (strcmp(path, "/.well-known/nodeinfo") == 0 ||
                 strcmp(path, "/.well-known/nodeinfo/") == 0 ||
                 strcmp(path, "/nodeinfo") == 0 ||
                 strcmp(path, "/nodeinfo/") == 0 ||
                 strcmp(path, "/api/nodeinfo") == 0 ||
                 strcmp(path, "/peer/v1/nodeinfo") == 0)) {
    static const char ni[] =
      "{"
      "\"links\":[],"
      "\"x-nanobot\":{"
        "\"schema\":\"nanobot.peer_http.v1\","
        "\"action\":\"nodeinfo\","
        "\"nodeinfo\":false,"
        "\"product_wire\":\"smx2\","
        "\"peer_http\":\"lab_ops_only\","
        "\"peer_http_is_product_bus\":false,"
        "\"share\":\"state_matrix_only\","
        "\"hold_flash\":1,"
        "\"llm_is_commander\":false,"
        "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", ni, sizeof ni - 1);
    free(req); close(cfd); return;
  }

  /* Residual: mesh/WebFinger probes hit /.well-known/host-meta (RFC 6415)
   * and got not_found. Lab ops is not a host-meta authority — empty JRD plate. */
  if (is_get && (strcmp(path, "/.well-known/host-meta") == 0 ||
                 strcmp(path, "/.well-known/host-meta/") == 0 ||
                 strcmp(path, "/.well-known/host-meta.json") == 0 ||
                 strcmp(path, "/.well-known/host-meta.json/") == 0 ||
                 strcmp(path, "/host-meta") == 0 ||
                 strcmp(path, "/host-meta/") == 0 ||
                 strcmp(path, "/host-meta.json") == 0 ||
                 strcmp(path, "/host-meta.json/") == 0 ||
                 strcmp(path, "/api/host-meta") == 0 ||
                 strcmp(path, "/peer/v1/host-meta") == 0)) {
    static const char hm[] =
      "{"
      "\"subject\":\"\","
      "\"links\":[],"
      "\"x-nanobot\":{"
        "\"schema\":\"nanobot.peer_http.v1\","
        "\"action\":\"host_meta\","
        "\"host_meta\":false,"
        "\"product_wire\":\"smx2\","
        "\"peer_http\":\"lab_ops_only\","
        "\"peer_http_is_product_bus\":false,"
        "\"share\":\"state_matrix_only\","
        "\"hold_flash\":1,"
        "\"llm_is_commander\":false,"
        "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/jrd+json", hm, sizeof hm - 1);
    free(req); close(cfd); return;
  }

  /* Residual: Matrix/mesh probes hit /.well-known/matrix/client and got
   * not_found. Lab ops is not a Matrix client discovery host — empty plate. */
  if (is_get && (strcmp(path, "/.well-known/matrix/client") == 0 ||
                 strcmp(path, "/.well-known/matrix/client/") == 0 ||
                 strcmp(path, "/matrix/client") == 0 ||
                 strcmp(path, "/matrix/client/") == 0 ||
                 strcmp(path, "/api/matrix/client") == 0 ||
                 strcmp(path, "/peer/v1/matrix/client") == 0)) {
    static const char mc[] =
      "{"
      "\"m.homeserver\":{\"base_url\":\"\"},"
      "\"m.identity_server\":{\"base_url\":\"\"},"
      "\"x-nanobot\":{"
        "\"schema\":\"nanobot.peer_http.v1\","
        "\"action\":\"matrix_client\","
        "\"matrix\":false,"
        "\"product_wire\":\"smx2\","
        "\"peer_http\":\"lab_ops_only\","
        "\"peer_http_is_product_bus\":false,"
        "\"share\":\"state_matrix_only\","
        "\"hold_flash\":1,"
        "\"llm_is_commander\":false,"
        "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", mc, sizeof mc - 1);
    free(req); close(cfd); return;
  }

  /* Residual: Matrix federation probes hit /.well-known/matrix/server and got
   * not_found. Lab ops is not a Matrix homeserver — empty m.server plate. */
  if (is_get && (strcmp(path, "/.well-known/matrix/server") == 0 ||
                 strcmp(path, "/.well-known/matrix/server/") == 0 ||
                 strcmp(path, "/matrix/server") == 0 ||
                 strcmp(path, "/matrix/server/") == 0 ||
                 strcmp(path, "/api/matrix/server") == 0 ||
                 strcmp(path, "/peer/v1/matrix/server") == 0)) {
    static const char ms[] =
      "{"
      "\"m.server\":\"\","
      "\"x-nanobot\":{"
        "\"schema\":\"nanobot.peer_http.v1\","
        "\"action\":\"matrix_server\","
        "\"matrix\":false,"
        "\"product_wire\":\"smx2\","
        "\"peer_http\":\"lab_ops_only\","
        "\"peer_http_is_product_bus\":false,"
        "\"share\":\"state_matrix_only\","
        "\"hold_flash\":1,"
        "\"llm_is_commander\":false,"
        "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", ms, sizeof ms - 1);
    free(req); close(cfd); return;
  }

  /* Residual: OIDC/mesh probes hit /.well-known/openid-configuration and got
   * not_found. Lab ops is not an OIDC OP — honest plate; auth is /api/auth. */
  if (is_get &&
      (strcmp(path, "/.well-known/openid-configuration") == 0 ||
       strcmp(path, "/.well-known/openid-configuration/") == 0 ||
       strcmp(path, "/.well-known/openid-configuration.json") == 0 ||
       strcmp(path, "/openid-configuration") == 0 ||
       strcmp(path, "/openid-configuration/") == 0 ||
       strcmp(path, "/openid-configuration.json") == 0 ||
       strcmp(path, "/api/openid-configuration") == 0 ||
       strcmp(path, "/peer/v1/openid-configuration") == 0)) {
    static const char oidc[] =
      "{"
      "\"issuer\":\"\","
      "\"authorization_endpoint\":\"/api/auth\","
      "\"token_endpoint\":\"\","
      "\"userinfo_endpoint\":\"\","
      "\"jwks_uri\":\"\","
      "\"response_types_supported\":[],"
      "\"subject_types_supported\":[],"
      "\"id_token_signing_alg_values_supported\":[],"
      "\"scopes_supported\":[],"
      "\"claims_supported\":[],"
      "\"x-nanobot\":{"
        "\"schema\":\"nanobot.peer_http.v1\","
        "\"action\":\"openid_configuration\","
        "\"oidc_provider\":false,"
        "\"auth\":\"browser_device_code\","
        "\"auth_plate\":\"/api/auth\","
        "\"product_wire\":\"smx2\","
        "\"peer_http\":\"lab_ops_only\","
        "\"peer_http_is_product_bus\":false,"
        "\"share\":\"state_matrix_only\","
        "\"hold_flash\":1,"
        "\"llm_is_commander\":false,"
        "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", oidc, sizeof oidc - 1);
    free(req); close(cfd); return;
  }

  /* Residual: OIDC/mesh probes hit /.well-known/openid-federation and got
   * not_found. Lab ops is not an OpenID Federation entity — empty plate. */
  if (is_get &&
      (strcmp(path, "/.well-known/openid-federation") == 0 ||
       strcmp(path, "/.well-known/openid-federation/") == 0 ||
       strcmp(path, "/openid-federation") == 0 ||
       strcmp(path, "/openid-federation/") == 0 ||
       strcmp(path, "/api/openid-federation") == 0 ||
       strcmp(path, "/peer/v1/openid-federation") == 0)) {
    static const char oif[] =
      "{"
      "\"iss\":\"\","
      "\"sub\":\"\","
      "\"iat\":0,"
      "\"exp\":0,"
      "\"jwks\":{\"keys\":[]},"
      "\"metadata\":{},"
      "\"authority_hints\":[],"
      "\"x-nanobot\":{"
        "\"schema\":\"nanobot.peer_http.v1\","
        "\"action\":\"openid_federation\","
        "\"openid_federation\":false,"
        "\"oidc_provider\":false,"
        "\"auth\":\"browser_device_code\","
        "\"auth_plate\":\"/api/auth\","
        "\"product_wire\":\"smx2\","
        "\"peer_http\":\"lab_ops_only\","
        "\"peer_http_is_product_bus\":false,"
        "\"share\":\"state_matrix_only\","
        "\"hold_flash\":1,"
        "\"llm_is_commander\":false,"
        "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", oif, sizeof oif - 1);
    free(req); close(cfd); return;
  }

  /* Residual: UMA/mesh probes hit /.well-known/uma2-configuration and got
   * not_found. Lab ops is not a UMA 2.0 AS/RS — empty plate; auth is /api/auth. */
  if (is_get &&
      (strcmp(path, "/.well-known/uma2-configuration") == 0 ||
       strcmp(path, "/.well-known/uma2-configuration/") == 0 ||
       strcmp(path, "/uma2-configuration") == 0 ||
       strcmp(path, "/uma2-configuration/") == 0 ||
       strcmp(path, "/api/uma2-configuration") == 0 ||
       strcmp(path, "/peer/v1/uma2-configuration") == 0)) {
    static const char uma2[] =
      "{"
      "\"issuer\":\"\","
      "\"authorization_endpoint\":\"/api/auth\","
      "\"token_endpoint\":\"\","
      "\"jwks_uri\":\"\","
      "\"registration_endpoint\":\"\","
      "\"resource_registration_endpoint\":\"\","
      "\"permission_endpoint\":\"\","
      "\"introspection_endpoint\":\"\","
      "\"claims_interaction_endpoint\":\"\","
      "\"grant_types_supported\":[],"
      "\"response_types_supported\":[],"
      "\"token_endpoint_auth_methods_supported\":[],"
      "\"uma_profiles_supported\":[],"
      "\"x-nanobot\":{"
        "\"schema\":\"nanobot.peer_http.v1\","
        "\"action\":\"uma2_configuration\","
        "\"uma2\":false,"
        "\"uma_as\":false,"
        "\"auth\":\"browser_device_code\","
        "\"auth_plate\":\"/api/auth\","
        "\"product_wire\":\"smx2\","
        "\"peer_http\":\"lab_ops_only\","
        "\"peer_http_is_product_bus\":false,"
        "\"share\":\"state_matrix_only\","
        "\"hold_flash\":1,"
        "\"llm_is_commander\":false,"
        "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", uma2, sizeof uma2 - 1);
    free(req); close(cfd); return;
  }

  /* Residual: OID4VCI/mesh probes hit /.well-known/openid-credential-issuer
   * and got not_found. Lab ops is not a credential issuer — empty plate. */
  if (is_get &&
      (strcmp(path, "/.well-known/openid-credential-issuer") == 0 ||
       strcmp(path, "/.well-known/openid-credential-issuer/") == 0 ||
       strcmp(path, "/openid-credential-issuer") == 0 ||
       strcmp(path, "/openid-credential-issuer/") == 0 ||
       strcmp(path, "/api/openid-credential-issuer") == 0 ||
       strcmp(path, "/peer/v1/openid-credential-issuer") == 0)) {
    static const char oci[] =
      "{"
      "\"credential_issuer\":\"\","
      "\"authorization_servers\":[],"
      "\"credential_endpoint\":\"\","
      "\"batch_credential_endpoint\":\"\","
      "\"deferred_credential_endpoint\":\"\","
      "\"notification_endpoint\":\"\","
      "\"credential_response_encryption\":{},"
      "\"credential_configurations_supported\":{},"
      "\"display\":[],"
      "\"x-nanobot\":{"
        "\"schema\":\"nanobot.peer_http.v1\","
        "\"action\":\"openid_credential_issuer\","
        "\"openid_credential_issuer\":false,"
        "\"oid4vci\":false,"
        "\"auth\":\"browser_device_code\","
        "\"auth_plate\":\"/api/auth\","
        "\"product_wire\":\"smx2\","
        "\"peer_http\":\"lab_ops_only\","
        "\"peer_http_is_product_bus\":false,"
        "\"share\":\"state_matrix_only\","
        "\"hold_flash\":1,"
        "\"llm_is_commander\":false,"
        "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", oci, sizeof oci - 1);
    free(req); close(cfd); return;
  }

  /* Residual: FIDO2/mesh probes hit /.well-known/fido2-configuration and got
   * not_found. Lab ops is not a FIDO2 server — empty plate; passkeys empty too. */
  if (is_get &&
      (strcmp(path, "/.well-known/fido2-configuration") == 0 ||
       strcmp(path, "/.well-known/fido2-configuration/") == 0 ||
       strcmp(path, "/fido2-configuration") == 0 ||
       strcmp(path, "/fido2-configuration/") == 0 ||
       strcmp(path, "/api/fido2-configuration") == 0 ||
       strcmp(path, "/peer/v1/fido2-configuration") == 0)) {
    static const char fido2[] =
      "{"
      "\"version\":\"\","
      "\"server\":{},"
      "\"attestation\":{},"
      "\"authentication\":{},"
      "\"mdsEndpoints\":[],"
      "\"x-nanobot\":{"
        "\"schema\":\"nanobot.peer_http.v1\","
        "\"action\":\"fido2_configuration\","
        "\"fido2\":false,"
        "\"webauthn_rp\":false,"
        "\"passkey_endpoints\":\"/.well-known/passkey-endpoints\","
        "\"auth\":\"browser_device_code\","
        "\"auth_plate\":\"/api/auth\","
        "\"product_wire\":\"smx2\","
        "\"peer_http\":\"lab_ops_only\","
        "\"peer_http_is_product_bus\":false,"
        "\"share\":\"state_matrix_only\","
        "\"hold_flash\":1,"
        "\"llm_is_commander\":false,"
        "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", fido2, sizeof fido2 - 1);
    free(req); close(cfd); return;
  }

  /* Residual: WebAuthn/mesh probes hit /.well-known/webauthn (related origins)
   * and got not_found. Lab ops is not a WebAuthn RP — empty origins plate. */
  if (is_get &&
      (strcmp(path, "/.well-known/webauthn") == 0 ||
       strcmp(path, "/.well-known/webauthn/") == 0 ||
       strcmp(path, "/webauthn") == 0 ||
       strcmp(path, "/webauthn/") == 0 ||
       strcmp(path, "/api/webauthn") == 0 ||
       strcmp(path, "/peer/v1/webauthn") == 0)) {
    static const char wa[] =
      "{"
      "\"origins\":[],"
      "\"x-nanobot\":{"
        "\"schema\":\"nanobot.peer_http.v1\","
        "\"action\":\"webauthn\","
        "\"webauthn\":false,"
        "\"webauthn_rp\":false,"
        "\"related_origins\":false,"
        "\"fido2_configuration\":\"/.well-known/fido2-configuration\","
        "\"passkey_endpoints\":\"/.well-known/passkey-endpoints\","
        "\"auth\":\"browser_device_code\","
        "\"auth_plate\":\"/api/auth\","
        "\"product_wire\":\"smx2\","
        "\"peer_http\":\"lab_ops_only\","
        "\"peer_http_is_product_bus\":false,"
        "\"share\":\"state_matrix_only\","
        "\"hold_flash\":1,"
        "\"llm_is_commander\":false,"
        "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", wa, sizeof wa - 1);
    free(req); close(cfd); return;
  }

  /* Residual: DID/mesh probes hit /.well-known/did.json (did:web) and got
   * not_found. Lab ops is not a DID subject — empty document plate. */
  if (is_get &&
      (strcmp(path, "/.well-known/did.json") == 0 ||
       strcmp(path, "/.well-known/did.json/") == 0 ||
       strcmp(path, "/did.json") == 0 ||
       strcmp(path, "/did.json/") == 0 ||
       strcmp(path, "/api/did.json") == 0 ||
       strcmp(path, "/peer/v1/did.json") == 0)) {
    static const char did[] =
      "{"
      "\"@context\":[\"https://www.w3.org/ns/did/v1\"],"
      "\"id\":\"\","
      "\"verificationMethod\":[],"
      "\"authentication\":[],"
      "\"assertionMethod\":[],"
      "\"service\":[],"
      "\"x-nanobot\":{"
        "\"schema\":\"nanobot.peer_http.v1\","
        "\"action\":\"did_json\","
        "\"did\":false,"
        "\"did_web\":false,"
        "\"auth\":\"browser_device_code\","
        "\"auth_plate\":\"/api/auth\","
        "\"product_wire\":\"smx2\","
        "\"peer_http\":\"lab_ops_only\","
        "\"peer_http_is_product_bus\":false,"
        "\"share\":\"state_matrix_only\","
        "\"hold_flash\":1,"
        "\"llm_is_commander\":false,"
        "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", did, sizeof did - 1);
    free(req); close(cfd); return;
  }

  /* Residual: DID/mesh probes hit /.well-known/did-configuration (domain
   * linkage) and got not_found. Lab ops has no domain linkage VCs — empty. */
  if (is_get &&
      (strcmp(path, "/.well-known/did-configuration") == 0 ||
       strcmp(path, "/.well-known/did-configuration/") == 0 ||
       strcmp(path, "/did-configuration") == 0 ||
       strcmp(path, "/did-configuration/") == 0 ||
       strcmp(path, "/api/did-configuration") == 0 ||
       strcmp(path, "/peer/v1/did-configuration") == 0)) {
    static const char didc[] =
      "{"
      "\"@context\":\"https://identity.foundation/.well-known/did-configuration/v1\","
      "\"linked_dids\":[],"
      "\"x-nanobot\":{"
        "\"schema\":\"nanobot.peer_http.v1\","
        "\"action\":\"did_configuration\","
        "\"did_configuration\":false,"
        "\"domain_linkage\":false,"
        "\"did_json\":\"/.well-known/did.json\","
        "\"auth\":\"browser_device_code\","
        "\"auth_plate\":\"/api/auth\","
        "\"product_wire\":\"smx2\","
        "\"peer_http\":\"lab_ops_only\","
        "\"peer_http_is_product_bus\":false,"
        "\"share\":\"state_matrix_only\","
        "\"hold_flash\":1,"
        "\"llm_is_commander\":false,"
        "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", didc, sizeof didc - 1);
    free(req); close(cfd); return;
  }

  /* Residual: OAuth/mesh probes hit /.well-known/oauth-authorization-server
   * (RFC 8414) and got not_found. Lab ops is not an OAuth AS — honest plate. */
  if (is_get &&
      (strcmp(path, "/.well-known/oauth-authorization-server") == 0 ||
       strcmp(path, "/.well-known/oauth-authorization-server/") == 0 ||
       strcmp(path, "/.well-known/oauth-authorization-server.json") == 0 ||
       strcmp(path, "/oauth-authorization-server") == 0 ||
       strcmp(path, "/oauth-authorization-server/") == 0 ||
       strcmp(path, "/oauth-authorization-server.json") == 0 ||
       strcmp(path, "/api/oauth-authorization-server") == 0 ||
       strcmp(path, "/peer/v1/oauth-authorization-server") == 0)) {
    static const char oauth_as[] =
      "{"
      "\"issuer\":\"\","
      "\"authorization_endpoint\":\"/api/auth\","
      "\"token_endpoint\":\"\","
      "\"registration_endpoint\":\"\","
      "\"jwks_uri\":\"\","
      "\"response_types_supported\":[],"
      "\"grant_types_supported\":[],"
      "\"token_endpoint_auth_methods_supported\":[],"
      "\"scopes_supported\":[],"
      "\"code_challenge_methods_supported\":[],"
      "\"x-nanobot\":{"
        "\"schema\":\"nanobot.peer_http.v1\","
        "\"action\":\"oauth_authorization_server\","
        "\"oauth_as\":false,"
        "\"auth\":\"browser_device_code\","
        "\"auth_plate\":\"/api/auth\","
        "\"product_wire\":\"smx2\","
        "\"peer_http\":\"lab_ops_only\","
        "\"peer_http_is_product_bus\":false,"
        "\"share\":\"state_matrix_only\","
        "\"hold_flash\":1,"
        "\"llm_is_commander\":false,"
        "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", oauth_as, sizeof oauth_as - 1);
    free(req); close(cfd); return;
  }

  /* Residual: OAuth/mesh probes hit /.well-known/oauth-client-registration
   * (RFC 7591 DCR) and got not_found. Lab ops does not offer DCR — empty plate. */
  if (is_get &&
      (strcmp(path, "/.well-known/oauth-client-registration") == 0 ||
       strcmp(path, "/.well-known/oauth-client-registration/") == 0 ||
       strcmp(path, "/oauth-client-registration") == 0 ||
       strcmp(path, "/oauth-client-registration/") == 0 ||
       strcmp(path, "/api/oauth-client-registration") == 0 ||
       strcmp(path, "/peer/v1/oauth-client-registration") == 0)) {
    static const char oauth_reg[] =
      "{"
      "\"registration_endpoint\":\"\","
      "\"client_id_issued_at\":0,"
      "\"client_secret_expires_at\":0,"
      "\"registration_client_uri\":\"\","
      "\"x-nanobot\":{"
        "\"schema\":\"nanobot.peer_http.v1\","
        "\"action\":\"oauth_client_registration\","
        "\"oauth_client_registration\":false,"
        "\"dynamic_client_registration\":false,"
        "\"auth\":\"browser_device_code\","
        "\"auth_plate\":\"/api/auth\","
        "\"product_wire\":\"smx2\","
        "\"peer_http\":\"lab_ops_only\","
        "\"peer_http_is_product_bus\":false,"
        "\"share\":\"state_matrix_only\","
        "\"hold_flash\":1,"
        "\"llm_is_commander\":false,"
        "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", oauth_reg, sizeof oauth_reg - 1);
    free(req); close(cfd); return;
  }

  /* Residual: OAuth/mesh probes hit /.well-known/oauth-protected-resource
   * (RFC 9728) and got not_found. Lab ops uses peer token + /api/auth, not
   * a public OAuth protected-resource metadata AS chain. */
  if (is_get &&
      (strcmp(path, "/.well-known/oauth-protected-resource") == 0 ||
       strcmp(path, "/.well-known/oauth-protected-resource/") == 0 ||
       strcmp(path, "/.well-known/oauth-protected-resource.json") == 0 ||
       strcmp(path, "/oauth-protected-resource") == 0 ||
       strcmp(path, "/oauth-protected-resource/") == 0 ||
       strcmp(path, "/oauth-protected-resource.json") == 0 ||
       strcmp(path, "/api/oauth-protected-resource") == 0 ||
       strcmp(path, "/peer/v1/oauth-protected-resource") == 0)) {
    static const char oauth_pr[] =
      "{"
      "\"resource\":\"\","
      "\"authorization_servers\":[],"
      "\"scopes_supported\":[],"
      "\"bearer_methods_supported\":[\"header\"],"
      "\"resource_documentation\":\"https://github.com/Abyss-c0re/nanobot\","
      "\"x-nanobot\":{"
        "\"schema\":\"nanobot.peer_http.v1\","
        "\"action\":\"oauth_protected_resource\","
        "\"oauth_resource_metadata\":true,"
        "\"public_oauth\":false,"
        "\"auth\":\"browser_device_code\","
        "\"auth_plate\":\"/api/auth\","
        "\"peer_token_header\":\"X-Nanobot-Peer-Token\","
        "\"product_wire\":\"smx2\","
        "\"peer_http\":\"lab_ops_only\","
        "\"peer_http_is_product_bus\":false,"
        "\"share\":\"state_matrix_only\","
        "\"hold_flash\":1,"
        "\"llm_is_commander\":false,"
        "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", oauth_pr, sizeof oauth_pr - 1);
    free(req); close(cfd); return;
  }

  /* Residual: scanner/mesh probes hit /crossdomain.xml and got not_found.
   * Lab ops denies all Adobe/Flash cross-domain policies. */
  if (is_get && (strcmp(path, "/crossdomain.xml") == 0 ||
                 strcmp(path, "/crossdomain.xml/") == 0 ||
                 strcmp(path, "/api/crossdomain.xml") == 0 ||
                 strcmp(path, "/peer/v1/crossdomain.xml") == 0 ||
                 strcmp(path, "/clientaccesspolicy.xml") == 0 ||
                 strcmp(path, "/clientaccesspolicy.xml/") == 0)) {
    static const char xdom[] =
      "<?xml version=\"1.0\"?>\n"
      "<!-- nanobot peer HTTP — lab ops only (not product SMX2) -->\n"
      "<!DOCTYPE cross-domain-policy SYSTEM "
      "\"http://www.adobe.com/xml/dtds/cross-domain-policy.dtd\">\n"
      "<cross-domain-policy>\n"
      "  <site-control permitted-cross-domain-policies=\"none\"/>\n"
      "</cross-domain-policy>\n";
    http_response(cfd, 200, "text/x-cross-domain-policy", xdom, sizeof xdom - 1);
    free(req); close(cfd); return;
  }

  /* Residual: browser/mesh probes hit /browserconfig.xml and got not_found.
   * MS tile config — lab ops has no pinned tiles; theme only. */
  if (is_get && (strcmp(path, "/browserconfig.xml") == 0 ||
                 strcmp(path, "/browserconfig.xml/") == 0 ||
                 strcmp(path, "/api/browserconfig.xml") == 0 ||
                 strcmp(path, "/peer/v1/browserconfig.xml") == 0)) {
    static const char bcfg[] =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<!-- nanobot peer HTTP — lab ops only (not product SMX2) -->\n"
      "<browserconfig>\n"
      "  <msapplication>\n"
      "    <tile>\n"
      "      <TileColor>#0a0a0a</TileColor>\n"
      "    </tile>\n"
      "  </msapplication>\n"
      "</browserconfig>\n";
    http_response(cfd, 200, "application/xml; charset=utf-8", bcfg, sizeof bcfg - 1);
    free(req); close(cfd); return;
  }

  /* Residual: browsers hit /.well-known/change-password (W3C) and got not_found.
   * No local password store — 302 to /api/auth (provider session plate). */
  if (is_get && (strcmp(path, "/.well-known/change-password") == 0 ||
                 strcmp(path, "/.well-known/change-password/") == 0 ||
                 strcmp(path, "/api/change-password") == 0 ||
                 strcmp(path, "/peer/v1/change-password") == 0 ||
                 strcmp(path, "/change-password") == 0 ||
                 strcmp(path, "/change-password/") == 0)) {
    static const char hdr[] =
      "HTTP/1.1 302 Found\r\n"
      "Location: /api/auth\r\n"
      "Content-Length: 0\r\n"
      "Connection: close\r\n"
      "Cache-Control: no-store\r\n\r\n";
    send_all(cfd, hdr, sizeof hdr - 1);
    free(req); close(cfd); return;
  }

  /* Residual: crawler/mesh probes hit /robots.txt and got dual-wire not_found.
   * Lab ops peer is not public product — disallow all. */
  if (is_get && (strcmp(path, "/robots.txt") == 0 || strcmp(path, "/robots.txt/") == 0)) {
    static const char robots[] =
      "# nanobot peer HTTP — lab ops only (not product SMX2)\n"
      "User-agent: *\n"
      "Disallow: /\n";
    http_response(cfd, 200, "text/plain; charset=utf-8", robots, sizeof robots - 1);
    free(req); close(cfd); return;
  }

  /* Residual: mesh/security scanners hit /security.txt and
   * /.well-known/security.txt (RFC 9116) and got not_found. */
  if (is_get && (strcmp(path, "/security.txt") == 0 ||
                 strcmp(path, "/security.txt/") == 0 ||
                 strcmp(path, "/.well-known/security.txt") == 0 ||
                 strcmp(path, "/.well-known/security.txt/") == 0 ||
                 strcmp(path, "/api/security.txt") == 0 ||
                 strcmp(path, "/peer/v1/security.txt") == 0)) {
    static const char sectxt[] =
      "# nanobot peer HTTP — lab ops only (not product SMX2)\n"
      "Contact: https://github.com/Abyss-c0re/nanobot/security/advisories/new\n"
      "Policy: https://github.com/Abyss-c0re/nanobot/blob/main/SECURITY.md\n"
      "Preferred-Languages: en\n"
      "Canonical: https://github.com/Abyss-c0re/nanobot/blob/main/SECURITY.md\n";
    http_response(cfd, 200, "text/plain; charset=utf-8", sectxt, sizeof sectxt - 1);
    free(req); close(cfd); return;
  }

  /* Residual: mesh/crawler probes hit /trust.txt and /.well-known/trust.txt
   * and got not_found. Lab ops peer has no org trust memberships — empty plate. */
  if (is_get && (strcmp(path, "/trust.txt") == 0 ||
                 strcmp(path, "/trust.txt/") == 0 ||
                 strcmp(path, "/.well-known/trust.txt") == 0 ||
                 strcmp(path, "/.well-known/trust.txt/") == 0 ||
                 strcmp(path, "/api/trust.txt") == 0 ||
                 strcmp(path, "/peer/v1/trust.txt") == 0)) {
    static const char trust[] =
      "# nanobot peer HTTP — lab ops only (not product SMX2)\n"
      "# trust.txt — no public membership / control edges declared\n"
      "# Companion: /.well-known/security.txt\n"
      "# See: https://github.com/Abyss-c0re/nanobot\n";
    http_response(cfd, 200, "text/plain; charset=utf-8", trust, sizeof trust - 1);
    free(req); close(cfd); return;
  }

  /* Residual: mesh/Keybase probes hit /keybase.txt and /.well-known/keybase.txt
   * and got not_found. Lab ops publishes no Keybase site proofs — empty plate. */
  if (is_get && (strcmp(path, "/keybase.txt") == 0 ||
                 strcmp(path, "/keybase.txt/") == 0 ||
                 strcmp(path, "/.well-known/keybase.txt") == 0 ||
                 strcmp(path, "/.well-known/keybase.txt/") == 0 ||
                 strcmp(path, "/api/keybase.txt") == 0 ||
                 strcmp(path, "/peer/v1/keybase.txt") == 0)) {
    static const char kbase[] =
      "# nanobot peer HTTP — lab ops only (not product SMX2)\n"
      "# keybase.txt — no site proofs published for this peer\n"
      "# Companion: /.well-known/security.txt /.well-known/trust.txt\n"
      "# See: https://github.com/Abyss-c0re/nanobot\n";
    http_response(cfd, 200, "text/plain; charset=utf-8", kbase, sizeof kbase - 1);
    free(req); close(cfd); return;
  }

  /* Residual: mesh/PGP probes hit /pgp-key.txt and /.well-known/pgp-key.txt
   * and got not_found. Lab ops does not publish an armored key here — empty. */
  if (is_get && (strcmp(path, "/pgp-key.txt") == 0 ||
                 strcmp(path, "/pgp-key.txt/") == 0 ||
                 strcmp(path, "/.well-known/pgp-key.txt") == 0 ||
                 strcmp(path, "/.well-known/pgp-key.txt/") == 0 ||
                 strcmp(path, "/api/pgp-key.txt") == 0 ||
                 strcmp(path, "/peer/v1/pgp-key.txt") == 0)) {
    static const char pgpk[] =
      "# nanobot peer HTTP — lab ops only (not product SMX2)\n"
      "# pgp-key.txt — no OpenPGP public key published at this peer\n"
      "# Contact/policy: /.well-known/security.txt\n"
      "# See: https://github.com/Abyss-c0re/nanobot/security/advisories/new\n";
    http_response(cfd, 200, "text/plain; charset=utf-8", pgpk, sizeof pgpk - 1);
    free(req); close(cfd); return;
  }

  /* Residual: OpenPGP WKD probes hit /.well-known/openpgpkey(/policy|/hu/…)
   * and got not_found. Lab ops does not offer WKD — policy empty, hu → 404. */
  if (is_get &&
      (strcmp(path, "/.well-known/openpgpkey/policy") == 0 ||
       strcmp(path, "/.well-known/openpgpkey/policy/") == 0 ||
       strcmp(path, "/openpgpkey/policy") == 0 ||
       strcmp(path, "/openpgpkey/policy/") == 0)) {
    static const char wkdp[] =
      "# nanobot peer HTTP — lab ops only (not product SMX2)\n"
      "# openpgpkey policy — Web Key Directory not offered\n"
      "# Companion: /.well-known/pgp-key.txt /.well-known/security.txt\n";
    http_response(cfd, 200, "text/plain; charset=utf-8", wkdp, sizeof wkdp - 1);
    free(req); close(cfd); return;
  }
  if (is_get &&
      (strcmp(path, "/.well-known/openpgpkey") == 0 ||
       strcmp(path, "/.well-known/openpgpkey/") == 0 ||
       strcmp(path, "/openpgpkey") == 0 ||
       strcmp(path, "/openpgpkey/") == 0 ||
       strcmp(path, "/api/openpgpkey") == 0 ||
       strcmp(path, "/peer/v1/openpgpkey") == 0)) {
    static const char wkd[] =
      "{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"openpgpkey\","
      "\"openpgpkey\":false,"
      "\"wkd\":false,"
      "\"policy\":\"/.well-known/openpgpkey/policy\","
      "\"pgp_key_txt\":\"/.well-known/pgp-key.txt\","
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}";
    http_response(cfd, 200, "application/json", wkd, sizeof wkd - 1);
    free(req); close(cfd); return;
  }
  if (is_get &&
      (strncmp(path, "/.well-known/openpgpkey/", 24) == 0 ||
       strncmp(path, "/openpgpkey/", 12) == 0)) {
    /* WKD hu/<hash> lookups — no keys published. */
    static const char nf[] =
      "{\"schema\":\"nanobot.peer_http.v1\",\"ok\":false,"
      "\"action\":\"openpgpkey\",\"error\":\"not_found\","
      "\"wkd\":false,\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\",\"hold_flash\":1,"
      "\"llm_is_commander\":false,\"python\":0}";
    http_response(cfd, 404, "application/json", nf, sizeof nf - 1);
    free(req); close(cfd); return;
  }

  /* Residual: mesh/SSHFP probes hit /.well-known/sshfp and got not_found.
   * Lab ops does not publish SSH host-key fingerprints (DNS SSHFP is out of band). */
  if (is_get && (strcmp(path, "/sshfp") == 0 ||
                 strcmp(path, "/sshfp/") == 0 ||
                 strcmp(path, "/sshfp.json") == 0 ||
                 strcmp(path, "/sshfp.json/") == 0 ||
                 strcmp(path, "/.well-known/sshfp") == 0 ||
                 strcmp(path, "/.well-known/sshfp/") == 0 ||
                 strcmp(path, "/.well-known/sshfp.json") == 0 ||
                 strcmp(path, "/.well-known/sshfp.json/") == 0 ||
                 strcmp(path, "/api/sshfp") == 0 ||
                 strcmp(path, "/api/sshfp.json") == 0 ||
                 strcmp(path, "/peer/v1/sshfp") == 0 ||
                 strcmp(path, "/peer/v1/sshfp.json") == 0)) {
    static const char sshfp[] =
      "{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"sshfp\","
      "\"sshfp\":false,"
      "\"fingerprints\":[],"
      "\"dns_sshfp\":false,"
      "\"security_txt\":\"/.well-known/security.txt\","
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}";
    http_response(cfd, 200, "application/json", sshfp, sizeof sshfp - 1);
    free(req); close(cfd); return;
  }

  /* Residual: OIDC/mesh probes hit /.well-known/jwks.json and got not_found.
   * Lab ops is not an OIDC AS — empty JWKS (jwks_uri stays empty on openid plate). */
  if (is_get && (strcmp(path, "/jwks.json") == 0 ||
                 strcmp(path, "/jwks.json/") == 0 ||
                 strcmp(path, "/jwks") == 0 ||
                 strcmp(path, "/jwks/") == 0 ||
                 strcmp(path, "/.well-known/jwks.json") == 0 ||
                 strcmp(path, "/.well-known/jwks.json/") == 0 ||
                 strcmp(path, "/.well-known/jwks") == 0 ||
                 strcmp(path, "/.well-known/jwks/") == 0 ||
                 strcmp(path, "/api/jwks.json") == 0 ||
                 strcmp(path, "/api/jwks") == 0 ||
                 strcmp(path, "/peer/v1/jwks.json") == 0 ||
                 strcmp(path, "/peer/v1/jwks") == 0 ||
                 strcmp(path, "/.well-known/oauth-authorization-server/jwks") == 0 ||
                 strcmp(path, "/.well-known/openid-configuration/jwks") == 0)) {
    static const char jwks[] =
      "{"
      "\"keys\":[],"
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"jwks\","
      "\"jwks\":false,"
      "\"oidc_provider\":false,"
      "\"openid_configuration\":\"/.well-known/openid-configuration\","
      "\"oauth_authorization_server\":\"/.well-known/oauth-authorization-server\","
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", jwks, sizeof jwks - 1);
    free(req); close(cfd); return;
  }

  /* Residual: mesh/browser probes hit /.well-known/related-website-set.json
   * and got not_found. Lab ops publishes no Related Website Sets. */
  if (is_get && (strcmp(path, "/.well-known/related-website-set.json") == 0 ||
                 strcmp(path, "/.well-known/related-website-set.json/") == 0 ||
                 strcmp(path, "/related-website-set.json") == 0 ||
                 strcmp(path, "/related-website-set.json/") == 0 ||
                 strcmp(path, "/api/related-website-set.json") == 0 ||
                 strcmp(path, "/peer/v1/related-website-set.json") == 0)) {
    static const char rws[] =
      "{"
      "\"primary\":\"\","
      "\"associatedSites\":[],"
      "\"serviceSites\":[],"
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"related_website_set\","
      "\"related_website_set\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", rws, sizeof rws - 1);
    free(req); close(cfd); return;
  }

  /* Residual: Azure/mesh probes hit microsoft-identity-association.json
   * and got not_found. Lab ops publishes no Microsoft identity domain assoc. */
  if (is_get &&
      (strcmp(path, "/.well-known/microsoft-identity-association.json") == 0 ||
       strcmp(path, "/.well-known/microsoft-identity-association.json/") == 0 ||
       strcmp(path, "/microsoft-identity-association.json") == 0 ||
       strcmp(path, "/microsoft-identity-association.json/") == 0 ||
       strcmp(path, "/api/microsoft-identity-association.json") == 0 ||
       strcmp(path, "/peer/v1/microsoft-identity-association.json") == 0)) {
    static const char mia[] =
      "{"
      "\"associatedApplications\":[],"
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"microsoft_identity_association\","
      "\"microsoft_identity_association\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", mia, sizeof mia - 1);
    free(req); close(cfd); return;
  }

  /* Residual: Apple Pay/mesh probes hit apple-developer-merchantid-domain-association
   * and got not_found. Lab ops publishes no Apple merchant domain association. */
  if (is_get &&
      (strcmp(path, "/.well-known/apple-developer-merchantid-domain-association") == 0 ||
       strcmp(path, "/.well-known/apple-developer-merchantid-domain-association/") == 0 ||
       strcmp(path, "/apple-developer-merchantid-domain-association") == 0 ||
       strcmp(path, "/apple-developer-merchantid-domain-association/") == 0 ||
       strcmp(path, "/api/apple-developer-merchantid-domain-association") == 0 ||
       strcmp(path, "/peer/v1/apple-developer-merchantid-domain-association") == 0)) {
    static const char amd[] =
      "{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"apple_merchantid_domain_association\","
      "\"apple_merchantid_domain_association\":false,"
      "\"merchant_ids\":[],"
      "\"content_type\":\"application/json\","
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}";
    http_response(cfd, 200, "application/json", amd, sizeof amd - 1);
    free(req); close(cfd); return;
  }

  /* Residual: NIP-05/mesh probes hit /.well-known/nostr.json and got not_found.
   * Lab ops publishes no Nostr NIP-05 names map. */
  if (is_get &&
      (strcmp(path, "/.well-known/nostr.json") == 0 ||
       strcmp(path, "/.well-known/nostr.json/") == 0 ||
       strcmp(path, "/.well-known/nostr") == 0 ||
       strcmp(path, "/.well-known/nostr/") == 0 ||
       strcmp(path, "/nostr.json") == 0 ||
       strcmp(path, "/nostr.json/") == 0 ||
       strcmp(path, "/nostr") == 0 ||
       strcmp(path, "/nostr/") == 0 ||
       strcmp(path, "/api/nostr.json") == 0 ||
       strcmp(path, "/api/nostr") == 0 ||
       strcmp(path, "/peer/v1/nostr.json") == 0 ||
       strcmp(path, "/peer/v1/nostr") == 0)) {
    static const char nostr[] =
      "{"
      "\"names\":{},"
      "\"relays\":{},"
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"nostr\","
      "\"nostr\":false,"
      "\"nip05\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", nostr, sizeof nostr - 1);
    free(req); close(cfd); return;
  }

  /* Residual: AT Protocol/mesh probes hit /.well-known/atproto-did and got not_found.
   * Lab ops publishes no Bluesky/ATProto handle DID. */
  if (is_get &&
      (strcmp(path, "/.well-known/atproto-did") == 0 ||
       strcmp(path, "/.well-known/atproto-did/") == 0 ||
       strcmp(path, "/atproto-did") == 0 ||
       strcmp(path, "/atproto-did/") == 0 ||
       strcmp(path, "/api/atproto-did") == 0 ||
       strcmp(path, "/peer/v1/atproto-did") == 0)) {
    static const char atp[] =
      "{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"atproto_did\","
      "\"atproto_did\":false,"
      "\"did\":\"\","
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}";
    http_response(cfd, 200, "application/json", atp, sizeof atp - 1);
    free(req); close(cfd); return;
  }

  /* Residual: Stellar SEP-0001/mesh probes hit /.well-known/stellar.toml
   * and got not_found. Lab ops publishes no Stellar TOML. */
  if (is_get &&
      (strcmp(path, "/.well-known/stellar.toml") == 0 ||
       strcmp(path, "/.well-known/stellar.toml/") == 0 ||
       strcmp(path, "/stellar.toml") == 0 ||
       strcmp(path, "/stellar.toml/") == 0 ||
       strcmp(path, "/api/stellar.toml") == 0 ||
       strcmp(path, "/peer/v1/stellar.toml") == 0)) {
    static const char st[] =
      "# nanobot peer HTTP — lab ops only (not product SMX2)\n"
      "# stellar.toml — no Stellar SEP-0001 metadata published\n"
      "# Companion: /.well-known/security.txt\n";
    http_response(cfd, 200, "text/plain; charset=utf-8", st, sizeof st - 1);
    free(req); close(cfd); return;
  }

  /* Residual: FedCM/mesh probes hit /.well-known/web-identity and got not_found.
   * Lab ops is not an identity provider — empty plate. */
  if (is_get &&
      (strcmp(path, "/.well-known/web-identity") == 0 ||
       strcmp(path, "/.well-known/web-identity/") == 0 ||
       strcmp(path, "/web-identity") == 0 ||
       strcmp(path, "/web-identity/") == 0 ||
       strcmp(path, "/api/web-identity") == 0 ||
       strcmp(path, "/peer/v1/web-identity") == 0)) {
    static const char wi[] =
      "{"
      "\"provider_urls\":[],"
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"web_identity\","
      "\"web_identity\":false,"
      "\"fedcm\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", wi, sizeof wi - 1);
    free(req); close(cfd); return;
  }

  /* Residual: Microsoft POSH/Autodiscover mesh probes hit /.well-known/posh
   * and got not_found. Lab ops publishes no POSH fingerprints. */
  if (is_get &&
      (strcmp(path, "/.well-known/posh") == 0 ||
       strcmp(path, "/.well-known/posh/") == 0 ||
       strcmp(path, "/.well-known/posh/v1") == 0 ||
       strcmp(path, "/.well-known/posh/v1/") == 0 ||
       strcmp(path, "/.well-known/posh.json") == 0 ||
       strcmp(path, "/posh") == 0 ||
       strcmp(path, "/posh/") == 0 ||
       strcmp(path, "/posh/v1") == 0 ||
       strcmp(path, "/posh.json") == 0 ||
       strcmp(path, "/api/posh") == 0 ||
       strcmp(path, "/api/posh/v1") == 0 ||
       strcmp(path, "/peer/v1/posh") == 0 ||
       strcmp(path, "/peer/v1/posh/v1") == 0)) {
    static const char posh[] =
      "{"
      "\"fingerprints\":[],"
      "\"expires\":0,"
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"posh\","
      "\"posh\":false,"
      "\"autodiscover\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", posh, sizeof posh - 1);
    free(req); close(cfd); return;
  }

  /* Residual: Chrome/mesh probes hit /.well-known/traffic-advice and got not_found.
   * Lab ops is not a public site — prefetch-proxy fraction 0.0 (no private prefetch). */
  if (is_get &&
      (strcmp(path, "/.well-known/traffic-advice") == 0 ||
       strcmp(path, "/.well-known/traffic-advice/") == 0 ||
       strcmp(path, "/traffic-advice") == 0 ||
       strcmp(path, "/traffic-advice/") == 0 ||
       strcmp(path, "/api/traffic-advice") == 0 ||
       strcmp(path, "/peer/v1/traffic-advice") == 0)) {
    static const char ta[] =
      "{"
      "\"advice\":[{"
        "\"user_agent\":\"prefetch-proxy\","
        "\"google_prefetch_proxy_eap\":{\"fraction\":0.0}"
      "}],"
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"traffic_advice\","
      "\"traffic_advice\":true,"
      "\"prefetch_proxy\":false,"
      "\"fraction\":0.0,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", ta, sizeof ta - 1);
    free(req); close(cfd); return;
  }

  /* Residual: Chrome/mesh probes hit /.well-known/privacy-sandbox-attestations.json
   * and got not_found. Lab ops does not enroll Privacy Sandbox APIs. */
  if (is_get &&
      (strcmp(path, "/.well-known/privacy-sandbox-attestations.json") == 0 ||
       strcmp(path, "/.well-known/privacy-sandbox-attestations.json/") == 0 ||
       strcmp(path, "/.well-known/privacy-sandbox-attestations") == 0 ||
       strcmp(path, "/.well-known/privacy-sandbox-attestations/") == 0 ||
       strcmp(path, "/privacy-sandbox-attestations.json") == 0 ||
       strcmp(path, "/privacy-sandbox-attestations.json/") == 0 ||
       strcmp(path, "/privacy-sandbox-attestations") == 0 ||
       strcmp(path, "/api/privacy-sandbox-attestations.json") == 0 ||
       strcmp(path, "/api/privacy-sandbox-attestations") == 0 ||
       strcmp(path, "/peer/v1/privacy-sandbox-attestations.json") == 0 ||
       strcmp(path, "/peer/v1/privacy-sandbox-attestations") == 0)) {
    static const char psa[] =
      "{"
      "\"version\":1,"
      "\"attestations\":{},"
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"privacy_sandbox_attestations\","
      "\"privacy_sandbox_attestations\":false,"
      "\"privacy_sandbox\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", psa, sizeof psa - 1);
    free(req); close(cfd); return;
  }

  /* Residual: ActivityPub/mesh probes hit
   * /.well-known/resource-that-should-not-be-used-for-federation and got
   * not_found. Lab ops is not an AP actor — explicit non-federation plate. */
  if (is_get &&
      (strcmp(path,
              "/.well-known/resource-that-should-not-be-used-for-federation") == 0 ||
       strcmp(path,
              "/.well-known/resource-that-should-not-be-used-for-federation/") == 0 ||
       strcmp(path, "/resource-that-should-not-be-used-for-federation") == 0 ||
       strcmp(path, "/resource-that-should-not-be-used-for-federation/") == 0 ||
       strcmp(path,
              "/api/resource-that-should-not-be-used-for-federation") == 0 ||
       strcmp(path,
              "/peer/v1/resource-that-should-not-be-used-for-federation") == 0)) {
    static const char nofed[] =
      "{"
      "\"federation\":false,"
      "\"activitypub\":false,"
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"no_federation\","
      "\"no_federation\":true,"
      "\"activitypub\":false,"
      "\"federation\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", nofed, sizeof nofed - 1);
    free(req); close(cfd); return;
  }

  /* Residual: Chrome/mesh probes hit
   * /.well-known/appspecific/com.chrome.devtools.json and got not_found.
   * Lab ops does not expose DevTools workspace mapping. */
  if (is_get &&
      (strcmp(path, "/.well-known/appspecific/com.chrome.devtools.json") == 0 ||
       strcmp(path, "/.well-known/appspecific/com.chrome.devtools.json/") == 0 ||
       strcmp(path, "/com.chrome.devtools.json") == 0 ||
       strcmp(path, "/com.chrome.devtools.json/") == 0 ||
       strcmp(path, "/api/com.chrome.devtools.json") == 0 ||
       strcmp(path, "/peer/v1/com.chrome.devtools.json") == 0 ||
       strcmp(path, "/.well-known/appspecific/com.chrome.devtools") == 0 ||
       strcmp(path, "/api/chrome-devtools") == 0 ||
       strcmp(path, "/peer/v1/chrome-devtools") == 0)) {
    static const char cdt[] =
      "{"
      "\"workspace\":{},"
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"chrome_devtools\","
      "\"chrome_devtools\":false,"
      "\"workspace\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", cdt, sizeof cdt - 1);
    free(req); close(cfd); return;
  }

  /* Residual: mesh probes hit /.well-known/http-opportunistic (RFC 8164)
   * and got not_found. Lab ops does not advertise opportunistic HTTP origins. */
  if (is_get &&
      (strcmp(path, "/.well-known/http-opportunistic") == 0 ||
       strcmp(path, "/.well-known/http-opportunistic/") == 0 ||
       strcmp(path, "/http-opportunistic") == 0 ||
       strcmp(path, "/http-opportunistic/") == 0 ||
       strcmp(path, "/api/http-opportunistic") == 0 ||
       strcmp(path, "/peer/v1/http-opportunistic") == 0)) {
    static const char hop[] =
      "{"
      "\"origins\":[],"
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"http_opportunistic\","
      "\"http_opportunistic\":false,"
      "\"rfc8164\":true,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", hop, sizeof hop - 1);
    free(req); close(cfd); return;
  }

  /* Residual: CoAP/mesh probes hit /.well-known/core (RFC 6690) and got
   * not_found. Lab ops is not a CoRE resource directory — empty links. */
  if (is_get &&
      (strcmp(path, "/.well-known/core") == 0 ||
       strcmp(path, "/.well-known/core/") == 0 ||
       strcmp(path, "/core") == 0 ||
       strcmp(path, "/core/") == 0 ||
       strcmp(path, "/api/core") == 0 ||
       strcmp(path, "/peer/v1/core") == 0)) {
    static const char core[] =
      "{"
      "\"links\":[],"
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"core\","
      "\"core\":false,"
      "\"rfc6690\":true,"
      "\"coap\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", core, sizeof core - 1);
    free(req); close(cfd); return;
  }

  /* Residual: mesh probes hit /.well-known/mercure and got not_found.
   * Lab ops is not a Mercure hub — empty discovery plate. */
  if (is_get &&
      (strcmp(path, "/.well-known/mercure") == 0 ||
       strcmp(path, "/.well-known/mercure/") == 0 ||
       strcmp(path, "/mercure") == 0 ||
       strcmp(path, "/mercure/") == 0 ||
       strcmp(path, "/api/mercure") == 0 ||
       strcmp(path, "/peer/v1/mercure") == 0 ||
       strcmp(path, "/.well-known/mercure/subscriptions") == 0 ||
       strcmp(path, "/mercure/subscriptions") == 0)) {
    static const char merc[] =
      "{"
      "\"hubs\":[],"
      "\"subscriptions\":false,"
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"mercure\","
      "\"mercure\":false,"
      "\"hub\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", merc, sizeof merc - 1);
    free(req); close(cfd); return;
  }

  /* Residual: mesh/auth probes hit /.well-known/gnap-as-rs and got not_found.
   * Lab ops is not a GNAP AS/RS — empty plate (auth remains browser device code). */
  if (is_get &&
      (strcmp(path, "/.well-known/gnap-as-rs") == 0 ||
       strcmp(path, "/.well-known/gnap-as-rs/") == 0 ||
       strcmp(path, "/gnap-as-rs") == 0 ||
       strcmp(path, "/gnap-as-rs/") == 0 ||
       strcmp(path, "/api/gnap-as-rs") == 0 ||
       strcmp(path, "/peer/v1/gnap-as-rs") == 0 ||
       strcmp(path, "/.well-known/gnap") == 0 ||
       strcmp(path, "/gnap") == 0)) {
    static const char gnap[] =
      "{"
      "\"grant_request_endpoint\":\"\","
      "\"interaction_start_modes_supported\":[],"
      "\"interaction_finish_methods_supported\":[],"
      "\"key_proofs_supported\":[],"
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"gnap_as_rs\","
      "\"gnap_as_rs\":false,"
      "\"gnap\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", gnap, sizeof gnap - 1);
    free(req); close(cfd); return;
  }

  /* Residual: security/mesh probes hit /.well-known/csaf/provider-metadata.json
   * and got not_found. Lab ops is not a CSAF provider — empty plate. */
  if (is_get &&
      (strcmp(path, "/.well-known/csaf/provider-metadata.json") == 0 ||
       strcmp(path, "/.well-known/csaf/provider-metadata.json/") == 0 ||
       strcmp(path, "/.well-known/csaf") == 0 ||
       strcmp(path, "/.well-known/csaf/") == 0 ||
       strcmp(path, "/csaf/provider-metadata.json") == 0 ||
       strcmp(path, "/csaf/provider-metadata.json/") == 0 ||
       strcmp(path, "/api/csaf/provider-metadata.json") == 0 ||
       strcmp(path, "/api/csaf") == 0 ||
       strcmp(path, "/peer/v1/csaf/provider-metadata.json") == 0 ||
       strcmp(path, "/peer/v1/csaf") == 0)) {
    static const char csaf[] =
      "{"
      "\"canonical_url\":\"\","
      "\"distributions\":[],"
      "\"public_openpgp_keys\":[],"
      "\"role\":\"\","
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"csaf\","
      "\"csaf\":false,"
      "\"provider\":false,"
      "\"security_txt\":\"/.well-known/security.txt\","
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", csaf, sizeof csaf - 1);
    free(req); close(cfd); return;
  }

  /* Residual: Discord/mesh probes hit /.well-known/discord and got not_found.
   * Lab ops does not claim Discord domain verification. */
  if (is_get &&
      (strcmp(path, "/.well-known/discord") == 0 ||
       strcmp(path, "/.well-known/discord/") == 0 ||
       strcmp(path, "/discord") == 0 ||
       strcmp(path, "/discord/") == 0 ||
       strcmp(path, "/api/discord") == 0 ||
       strcmp(path, "/peer/v1/discord") == 0 ||
       strcmp(path, "/.well-known/discord.json") == 0 ||
       strcmp(path, "/discord.json") == 0)) {
    static const char disc[] =
      "{"
      "\"verified\":false,"
      "\"token\":\"\","
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"discord\","
      "\"discord\":false,"
      "\"domain_verification\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", disc, sizeof disc - 1);
    free(req); close(cfd); return;
  }

  /* Residual: mail/mesh probes hit /.well-known/jmap (RFC 8620) and got
   * not_found. Lab ops is not a JMAP server — empty session plate. */
  if (is_get &&
      (strcmp(path, "/.well-known/jmap") == 0 ||
       strcmp(path, "/.well-known/jmap/") == 0 ||
       strcmp(path, "/jmap") == 0 ||
       strcmp(path, "/jmap/") == 0 ||
       strcmp(path, "/api/jmap") == 0 ||
       strcmp(path, "/peer/v1/jmap") == 0 ||
       strcmp(path, "/.well-known/jmap.json") == 0 ||
       strcmp(path, "/jmap.json") == 0)) {
    static const char jmap[] =
      "{"
      "\"capabilities\":{},"
      "\"accounts\":{},"
      "\"primaryAccounts\":{},"
      "\"username\":\"\","
      "\"apiUrl\":\"\","
      "\"downloadUrl\":\"\","
      "\"uploadUrl\":\"\","
      "\"eventSourceUrl\":\"\","
      "\"state\":\"\","
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"jmap\","
      "\"jmap\":false,"
      "\"session\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", jmap, sizeof jmap - 1);
    free(req); close(cfd); return;
  }

  /* Residual: WebRTC/mesh probes hit /.well-known/stun-key and got not_found.
   * Lab ops is not a STUN TLS key host — empty plate. */
  if (is_get &&
      (strcmp(path, "/.well-known/stun-key") == 0 ||
       strcmp(path, "/.well-known/stun-key/") == 0 ||
       strcmp(path, "/stun-key") == 0 ||
       strcmp(path, "/stun-key/") == 0 ||
       strcmp(path, "/api/stun-key") == 0 ||
       strcmp(path, "/peer/v1/stun-key") == 0 ||
       strcmp(path, "/.well-known/stun-key.json") == 0 ||
       strcmp(path, "/stun-key.json") == 0)) {
    static const char stun[] =
      "{"
      "\"keys\":[],"
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"stun_key\","
      "\"stun_key\":false,"
      "\"stun\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", stun, sizeof stun - 1);
    free(req); close(cfd); return;
  }

  /* Residual: Thread/mesh probes hit /.well-known/thread and got not_found.
   * Lab ops is not a Thread Border Router — empty plate. */
  if (is_get &&
      (strcmp(path, "/.well-known/thread") == 0 ||
       strcmp(path, "/.well-known/thread/") == 0 ||
       strcmp(path, "/thread") == 0 ||
       strcmp(path, "/thread/") == 0 ||
       strcmp(path, "/api/thread") == 0 ||
       strcmp(path, "/peer/v1/thread") == 0 ||
       strcmp(path, "/.well-known/thread.json") == 0 ||
       strcmp(path, "/thread.json") == 0)) {
    static const char thr[] =
      "{"
      "\"border_agent\":false,"
      "\"datasets\":[],"
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"thread\","
      "\"thread\":false,"
      "\"border_router\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", thr, sizeof thr - 1);
    free(req); close(cfd); return;
  }

  /* Residual: CoAP/mesh probes hit /.well-known/coap and got not_found.
   * Lab ops is not a CoAP endpoint host — empty plate. */
  if (is_get &&
      (strcmp(path, "/.well-known/coap") == 0 ||
       strcmp(path, "/.well-known/coap/") == 0 ||
       strcmp(path, "/coap") == 0 ||
       strcmp(path, "/coap/") == 0 ||
       strcmp(path, "/api/coap") == 0 ||
       strcmp(path, "/peer/v1/coap") == 0 ||
       strcmp(path, "/.well-known/coap.json") == 0 ||
       strcmp(path, "/coap.json") == 0)) {
    static const char coap[] =
      "{"
      "\"endpoints\":[],"
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"coap\","
      "\"coap\":false,"
      "\"core\":\"/.well-known/core\","
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", coap, sizeof coap - 1);
    free(req); close(cfd); return;
  }

  /* Residual: time/mesh probes hit /.well-known/time and got not_found.
   * Lab ops is not a time-service host — empty plate (OS clock is local). */
  if (is_get &&
      (strcmp(path, "/.well-known/time") == 0 ||
       strcmp(path, "/.well-known/time/") == 0 ||
       strcmp(path, "/time") == 0 ||
       strcmp(path, "/time/") == 0 ||
       strcmp(path, "/api/time") == 0 ||
       strcmp(path, "/peer/v1/time") == 0 ||
       strcmp(path, "/.well-known/time.json") == 0 ||
       strcmp(path, "/time.json") == 0)) {
    static const char tim[] =
      "{"
      "\"unix\":null,"
      "\"iso8601\":\"\","
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"time\","
      "\"time\":false,"
      "\"time_service\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", tim, sizeof tim - 1);
    free(req); close(cfd); return;
  }

  /* Residual: timezone/mesh probes hit /.well-known/timezone and got not_found.
   * Lab ops does not publish a timezone service — empty plate. */
  if (is_get &&
      (strcmp(path, "/.well-known/timezone") == 0 ||
       strcmp(path, "/.well-known/timezone/") == 0 ||
       strcmp(path, "/timezone") == 0 ||
       strcmp(path, "/timezone/") == 0 ||
       strcmp(path, "/api/timezone") == 0 ||
       strcmp(path, "/peer/v1/timezone") == 0 ||
       strcmp(path, "/.well-known/timezone.json") == 0 ||
       strcmp(path, "/timezone.json") == 0)) {
    static const char tz[] =
      "{"
      "\"timezone\":\"\","
      "\"offsets\":[],"
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"timezone\","
      "\"timezone\":false,"
      "\"tz_service\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", tz, sizeof tz - 1);
    free(req); close(cfd); return;
  }

  /* Residual: EST/mesh probes hit /.well-known/est and got not_found.
   * Lab ops is not an EST enrollment server (RFC 7030) — empty plate. */
  if (is_get &&
      (strcmp(path, "/.well-known/est") == 0 ||
       strcmp(path, "/.well-known/est/") == 0 ||
       strncmp(path, "/.well-known/est/", 17) == 0 ||
       strcmp(path, "/est") == 0 ||
       strcmp(path, "/est/") == 0 ||
       strcmp(path, "/api/est") == 0 ||
       strcmp(path, "/peer/v1/est") == 0 ||
       strcmp(path, "/.well-known/est.json") == 0 ||
       strcmp(path, "/est.json") == 0)) {
    static const char est[] =
      "{"
      "\"cacerts\":false,"
      "\"simpleenroll\":false,"
      "\"simplereenroll\":false,"
      "\"csrattrs\":false,"
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"est\","
      "\"est\":false,"
      "\"enrollment\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", est, sizeof est - 1);
    free(req); close(cfd); return;
  }

  /* Residual: CA/mesh probes hit /.well-known/pki-validation and got not_found.
   * Lab ops does not host ACME/HTTP domain validation tokens — empty plate. */
  if (is_get &&
      (strcmp(path, "/.well-known/pki-validation") == 0 ||
       strcmp(path, "/.well-known/pki-validation/") == 0 ||
       strncmp(path, "/.well-known/pki-validation/", 28) == 0 ||
       strcmp(path, "/pki-validation") == 0 ||
       strcmp(path, "/pki-validation/") == 0 ||
       strcmp(path, "/api/pki-validation") == 0 ||
       strcmp(path, "/peer/v1/pki-validation") == 0 ||
       strcmp(path, "/.well-known/pki-validation.json") == 0 ||
       strcmp(path, "/pki-validation.json") == 0)) {
    static const char pki[] =
      "{"
      "\"tokens\":[],"
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"pki_validation\","
      "\"pki_validation\":false,"
      "\"domain_validation\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", pki, sizeof pki - 1);
    free(req); close(cfd); return;
  }

  /* Residual: network/mesh probes hit /.well-known/looking-glass and got not_found.
   * Lab ops is not an ISP looking glass — empty plate. */
  if (is_get &&
      (strcmp(path, "/.well-known/looking-glass") == 0 ||
       strcmp(path, "/.well-known/looking-glass/") == 0 ||
       strcmp(path, "/looking-glass") == 0 ||
       strcmp(path, "/looking-glass/") == 0 ||
       strcmp(path, "/api/looking-glass") == 0 ||
       strcmp(path, "/peer/v1/looking-glass") == 0 ||
       strcmp(path, "/.well-known/looking-glass.json") == 0 ||
       strcmp(path, "/looking-glass.json") == 0)) {
    static const char lg[] =
      "{"
      "\"url\":\"\","
      "\"commands\":[],"
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"looking_glass\","
      "\"looking_glass\":false,"
      "\"lg\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", lg, sizeof lg - 1);
    free(req); close(cfd); return;
  }

  /* Residual: mesh/NI probes hit /.well-known/genid and got not_found.
   * Lab ops does not mint genid URIs (RFC 6920) — empty plate. */
  if (is_get &&
      (strcmp(path, "/.well-known/genid") == 0 ||
       strcmp(path, "/.well-known/genid/") == 0 ||
       strncmp(path, "/.well-known/genid/", 19) == 0 ||
       strcmp(path, "/genid") == 0 ||
       strcmp(path, "/genid/") == 0 ||
       strcmp(path, "/api/genid") == 0 ||
       strcmp(path, "/peer/v1/genid") == 0 ||
       strcmp(path, "/.well-known/genid.json") == 0 ||
       strcmp(path, "/genid.json") == 0)) {
    static const char gen[] =
      "{"
      "\"ids\":[],"
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"genid\","
      "\"genid\":false,"
      "\"named_information\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", gen, sizeof gen - 1);
    free(req); close(cfd); return;
  }

  /* Residual: mesh/TLS probes hit /.well-known/acme-challenge and got not_found.
   * Lab ops does not run ACME HTTP-01 (RFC 8555) — empty plate. */
  if (is_get &&
      (strcmp(path, "/.well-known/acme-challenge") == 0 ||
       strcmp(path, "/.well-known/acme-challenge/") == 0 ||
       strncmp(path, "/.well-known/acme-challenge/", 28) == 0 ||
       strcmp(path, "/acme-challenge") == 0 ||
       strcmp(path, "/acme-challenge/") == 0 ||
       strcmp(path, "/api/acme-challenge") == 0 ||
       strcmp(path, "/peer/v1/acme-challenge") == 0 ||
       strcmp(path, "/.well-known/acme-challenge.json") == 0 ||
       strcmp(path, "/acme-challenge.json") == 0)) {
    static const char acme[] =
      "{"
      "\"challenges\":[],"
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"acme_challenge\","
      "\"acme_challenge\":false,"
      "\"acme_http01\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", acme, sizeof acme - 1);
    free(req); close(cfd); return;
  }

  /* Residual: mesh/NI probes hit /.well-known/ni and got not_found.
   * Lab ops does not host Named Information (RFC 6920) resources — empty plate. */
  if (is_get &&
      (strcmp(path, "/.well-known/ni") == 0 ||
       strcmp(path, "/.well-known/ni/") == 0 ||
       strncmp(path, "/.well-known/ni/", 16) == 0 ||
       strcmp(path, "/ni") == 0 ||
       strcmp(path, "/ni/") == 0 ||
       strcmp(path, "/api/ni") == 0 ||
       strcmp(path, "/peer/v1/ni") == 0 ||
       strcmp(path, "/.well-known/ni.json") == 0 ||
       strcmp(path, "/ni.json") == 0)) {
    static const char ni[] =
      "{"
      "\"resources\":[],"
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"ni\","
      "\"ni\":false,"
      "\"named_information\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", ni, sizeof ni - 1);
    free(req); close(cfd); return;
  }

  /* Residual: mesh/push probes hit /.well-known/vapid and got not_found.
   * Lab ops does not publish VAPID keys (RFC 8292) — empty plate. */
  if (is_get &&
      (strcmp(path, "/.well-known/vapid") == 0 ||
       strcmp(path, "/.well-known/vapid/") == 0 ||
       strncmp(path, "/.well-known/vapid/", 18) == 0 ||
       strcmp(path, "/vapid") == 0 ||
       strcmp(path, "/vapid/") == 0 ||
       strcmp(path, "/api/vapid") == 0 ||
       strcmp(path, "/peer/v1/vapid") == 0 ||
       strcmp(path, "/.well-known/vapid.json") == 0 ||
       strcmp(path, "/vapid.json") == 0)) {
    static const char vapid[] =
      "{"
      "\"keys\":[],"
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"vapid\","
      "\"vapid\":false,"
      "\"web_push\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", vapid, sizeof vapid - 1);
    free(req); close(cfd); return;
  }

  /* Residual: mesh/auth probes hit /.well-known/hoba and got not_found.
   * Lab ops does not offer HOBA (RFC 7486) — empty plate. */
  if (is_get &&
      (strcmp(path, "/.well-known/hoba") == 0 ||
       strcmp(path, "/.well-known/hoba/") == 0 ||
       strncmp(path, "/.well-known/hoba/", 17) == 0 ||
       strcmp(path, "/hoba") == 0 ||
       strcmp(path, "/hoba/") == 0 ||
       strcmp(path, "/api/hoba") == 0 ||
       strcmp(path, "/peer/v1/hoba") == 0 ||
       strcmp(path, "/.well-known/hoba.json") == 0 ||
       strcmp(path, "/hoba.json") == 0)) {
    static const char hoba[] =
      "{"
      "\"origins\":[],"
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"hoba\","
      "\"hoba\":false,"
      "\"http_origin_bound_auth\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", hoba, sizeof hoba - 1);
    free(req); close(cfd); return;
  }

  /* Residual: mesh/PKI probes hit /.well-known/smime-aia and got not_found.
   * Lab ops does not publish S/MIME AIA (RFC 7030 family) — empty plate. */
  if (is_get &&
      (strcmp(path, "/.well-known/smime-aia") == 0 ||
       strcmp(path, "/.well-known/smime-aia/") == 0 ||
       strncmp(path, "/.well-known/smime-aia/", 22) == 0 ||
       strcmp(path, "/smime-aia") == 0 ||
       strcmp(path, "/smime-aia/") == 0 ||
       strcmp(path, "/api/smime-aia") == 0 ||
       strcmp(path, "/peer/v1/smime-aia") == 0 ||
       strcmp(path, "/.well-known/smime-aia.json") == 0 ||
       strcmp(path, "/smime-aia.json") == 0)) {
    static const char aia[] =
      "{"
      "\"certificates\":[],"
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"smime_aia\","
      "\"smime_aia\":false,"
      "\"smime\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", aia, sizeof aia - 1);
    free(req); close(cfd); return;
  }

  /* Residual: mesh/auth probes hit /.well-known/browserid and got not_found.
   * Lab ops does not offer BrowserID/Persona — empty plate. */
  if (is_get &&
      (strcmp(path, "/.well-known/browserid") == 0 ||
       strcmp(path, "/.well-known/browserid/") == 0 ||
       strncmp(path, "/.well-known/browserid/", 22) == 0 ||
       strcmp(path, "/browserid") == 0 ||
       strcmp(path, "/browserid/") == 0 ||
       strcmp(path, "/api/browserid") == 0 ||
       strcmp(path, "/peer/v1/browserid") == 0 ||
       strcmp(path, "/.well-known/browserid.json") == 0 ||
       strcmp(path, "/browserid.json") == 0)) {
    static const char bid[] =
      "{"
      "\"authentication\":\"\","
      "\"provisioning\":\"\","
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"browserid\","
      "\"browserid\":false,"
      "\"persona\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", bid, sizeof bid - 1);
    free(req); close(cfd); return;
  }

  /* Residual: mesh/IdP probes hit /.well-known/idp-proxy and got not_found.
   * Lab ops does not offer IdP proxy endpoints — empty plate. */
  if (is_get &&
      (strcmp(path, "/.well-known/idp-proxy") == 0 ||
       strcmp(path, "/.well-known/idp-proxy/") == 0 ||
       strncmp(path, "/.well-known/idp-proxy/", 22) == 0 ||
       strcmp(path, "/idp-proxy") == 0 ||
       strcmp(path, "/idp-proxy/") == 0 ||
       strcmp(path, "/api/idp-proxy") == 0 ||
       strcmp(path, "/peer/v1/idp-proxy") == 0 ||
       strcmp(path, "/.well-known/idp-proxy.json") == 0 ||
       strcmp(path, "/idp-proxy.json") == 0)) {
    static const char idp[] =
      "{"
      "\"proxies\":[],"
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"idp_proxy\","
      "\"idp_proxy\":false,"
      "\"identity_provider\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", idp, sizeof idp - 1);
    free(req); close(cfd); return;
  }

  /* Residual: privacy/mesh probes hit /.well-known/dnt and got not_found.
   * Companion to dnt-policy.txt — lab ops has no tracking surface (empty plate). */
  if (is_get &&
      (strcmp(path, "/.well-known/dnt") == 0 ||
       strcmp(path, "/.well-known/dnt/") == 0 ||
       strncmp(path, "/.well-known/dnt/", 16) == 0 ||
       strcmp(path, "/dnt") == 0 ||
       strcmp(path, "/dnt/") == 0 ||
       strcmp(path, "/api/dnt") == 0 ||
       strcmp(path, "/peer/v1/dnt") == 0 ||
       strcmp(path, "/.well-known/dnt.json") == 0 ||
       strcmp(path, "/dnt.json") == 0)) {
    static const char dnt[] =
      "{"
      "\"tracking\":\"N\","
      "\"policy\":\"/.well-known/dnt-policy.txt\","
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"dnt\","
      "\"dnt\":true,"
      "\"do_not_track\":true,"
      "\"tracking\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", dnt, sizeof dnt - 1);
    free(req); close(cfd); return;
  }

  /* Residual: mesh/OSS probes hit /.well-known/funding-manifest-urls and got
   * not_found. Lab ops peer is not a public funding surface — empty plate. */
  if (is_get &&
      (strcmp(path, "/.well-known/funding-manifest-urls") == 0 ||
       strcmp(path, "/.well-known/funding-manifest-urls/") == 0 ||
       strncmp(path, "/.well-known/funding-manifest-urls/", 33) == 0 ||
       strcmp(path, "/funding-manifest-urls") == 0 ||
       strcmp(path, "/funding-manifest-urls/") == 0 ||
       strcmp(path, "/api/funding-manifest-urls") == 0 ||
       strcmp(path, "/peer/v1/funding-manifest-urls") == 0 ||
       strcmp(path, "/.well-known/funding-manifest-urls.json") == 0 ||
       strcmp(path, "/funding-manifest-urls.json") == 0)) {
    static const char fund[] =
      "{"
      "\"urls\":[],"
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"funding_manifest_urls\","
      "\"funding_manifest_urls\":false,"
      "\"funding\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", fund, sizeof fund - 1);
    free(req); close(cfd); return;
  }

  /* Residual: ATProto/mesh probes hit /.well-known/xrpc-server-did and got
   * not_found. Lab ops is not an XRPC server — empty plate. */
  if (is_get &&
      (strcmp(path, "/.well-known/xrpc-server-did") == 0 ||
       strcmp(path, "/.well-known/xrpc-server-did/") == 0 ||
       strncmp(path, "/.well-known/xrpc-server-did/", 28) == 0 ||
       strcmp(path, "/xrpc-server-did") == 0 ||
       strcmp(path, "/xrpc-server-did/") == 0 ||
       strcmp(path, "/api/xrpc-server-did") == 0 ||
       strcmp(path, "/peer/v1/xrpc-server-did") == 0 ||
       strcmp(path, "/.well-known/xrpc-server-did.json") == 0 ||
       strcmp(path, "/xrpc-server-did.json") == 0)) {
    static const char xrpc[] =
      "{"
      "\"did\":\"\","
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"xrpc_server_did\","
      "\"xrpc_server_did\":false,"
      "\"atproto\":false,"
      "\"xrpc\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", xrpc, sizeof xrpc - 1);
    free(req); close(cfd); return;
  }

  /* Residual: mesh/MCP probes hit /.well-known/mcp.json and got not_found.
   * Lab ops peer HTTP is not an MCP product bus — empty discovery plate
   * (stdio/HTTP-MCP bridge is separate; /api/mcp/servers is registry only). */
  if (is_get &&
      (strcmp(path, "/.well-known/mcp.json") == 0 ||
       strcmp(path, "/.well-known/mcp.json/") == 0 ||
       strncmp(path, "/.well-known/mcp.json/", 20) == 0 ||
       strcmp(path, "/.well-known/mcp") == 0 ||
       strcmp(path, "/.well-known/mcp/") == 0 ||
       strncmp(path, "/.well-known/mcp/", 16) == 0 ||
       strcmp(path, "/mcp.json") == 0 ||
       strcmp(path, "/mcp.json/") == 0 ||
       strcmp(path, "/api/mcp.json") == 0 ||
       strcmp(path, "/peer/v1/mcp.json") == 0)) {
    static const char mcpj[] =
      "{"
      "\"mcpServers\":[],"
      "\"servers\":[],"
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"mcp_json\","
      "\"mcp_json\":false,"
      "\"mcp\":false,"
      "\"mcp_over_peer_http\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", mcpj, sizeof mcpj - 1);
    free(req); close(cfd); return;
  }

  /* Residual: mesh/crawler probes hit /.well-known/web-bot-auth and got
   * not_found. Lab ops is not a Web Bot Auth publisher — empty plate
   * (peer auth remains browser_device_code / peer_token). */
  if (is_get &&
      (strcmp(path, "/.well-known/web-bot-auth") == 0 ||
       strcmp(path, "/.well-known/web-bot-auth/") == 0 ||
       strncmp(path, "/.well-known/web-bot-auth/", 25) == 0 ||
       strcmp(path, "/web-bot-auth") == 0 ||
       strcmp(path, "/web-bot-auth/") == 0 ||
       strcmp(path, "/api/web-bot-auth") == 0 ||
       strcmp(path, "/peer/v1/web-bot-auth") == 0 ||
       strcmp(path, "/.well-known/web-bot-auth.json") == 0 ||
       strcmp(path, "/web-bot-auth.json") == 0)) {
    static const char wba[] =
      "{"
      "\"keys\":[],"
      "\"jwks_uri\":\"\","
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"web_bot_auth\","
      "\"web_bot_auth\":false,"
      "\"bot_auth\":false,"
      "\"crawler_identity\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", wba, sizeof wba - 1);
    free(req); close(cfd); return;
  }

  /* Residual: mesh/supply-chain probes hit /.well-known/sbom and got not_found.
   * Lab ops peer does not publish SBOM at well-known — empty plate
   * (see repo NOTICE/LICENSE/SECURITY for human supply-chain docs). */
  if (is_get &&
      (strcmp(path, "/.well-known/sbom") == 0 ||
       strcmp(path, "/.well-known/sbom/") == 0 ||
       strncmp(path, "/.well-known/sbom/", 17) == 0 ||
       strcmp(path, "/.well-known/sbom.json") == 0 ||
       strcmp(path, "/.well-known/supply-chain") == 0 ||
       strcmp(path, "/.well-known/supply-chain/") == 0 ||
       strncmp(path, "/.well-known/supply-chain/", 25) == 0 ||
       strcmp(path, "/sbom") == 0 ||
       strcmp(path, "/sbom/") == 0 ||
       strcmp(path, "/sbom.json") == 0 ||
       strcmp(path, "/api/sbom") == 0 ||
       strcmp(path, "/peer/v1/sbom") == 0)) {
    static const char sbom[] =
      "{"
      "\"sbom\":[],"
      "\"formats\":[],"
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"sbom\","
      "\"sbom\":false,"
      "\"supply_chain\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", sbom, sizeof sbom - 1);
    free(req); close(cfd); return;
  }

  /* Residual: privacy/mesh probes hit /.well-known/privacy-pass and got
   * not_found. Lab ops is not a Privacy Pass issuer/origin — empty plate. */
  if (is_get &&
      (strcmp(path, "/.well-known/privacy-pass") == 0 ||
       strcmp(path, "/.well-known/privacy-pass/") == 0 ||
       strncmp(path, "/.well-known/privacy-pass/", 25) == 0 ||
       strcmp(path, "/privacy-pass") == 0 ||
       strcmp(path, "/privacy-pass/") == 0 ||
       strcmp(path, "/api/privacy-pass") == 0 ||
       strcmp(path, "/peer/v1/privacy-pass") == 0 ||
       strcmp(path, "/.well-known/privacy-pass.json") == 0 ||
       strcmp(path, "/privacy-pass.json") == 0)) {
    static const char pp[] =
      "{"
      "\"issuer-request-uri\":\"\","
      "\"token-keys\":[],"
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"privacy_pass\","
      "\"privacy_pass\":false,"
      "\"issuer\":false,"
      "\"origin\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", pp, sizeof pp - 1);
    free(req); close(cfd); return;
  }

  /* Residual: privacy/mesh probes hit /.well-known/ohttp-gateway|ohttp-config
   * and got not_found. Lab ops is not an Oblivious HTTP gateway — empty plate. */
  if (is_get &&
      (strcmp(path, "/.well-known/ohttp-gateway") == 0 ||
       strcmp(path, "/.well-known/ohttp-gateway/") == 0 ||
       strncmp(path, "/.well-known/ohttp-gateway/", 26) == 0 ||
       strcmp(path, "/.well-known/ohttp-gateway.json") == 0 ||
       strcmp(path, "/.well-known/ohttp-config") == 0 ||
       strcmp(path, "/.well-known/ohttp-config/") == 0 ||
       strncmp(path, "/.well-known/ohttp-config/", 25) == 0 ||
       strcmp(path, "/.well-known/ohttp-config.json") == 0 ||
       strcmp(path, "/ohttp-gateway") == 0 ||
       strcmp(path, "/ohttp-gateway/") == 0 ||
       strcmp(path, "/ohttp-config") == 0 ||
       strcmp(path, "/api/ohttp-gateway") == 0 ||
       strcmp(path, "/peer/v1/ohttp-gateway") == 0)) {
    static const char ohttp[] =
      "{"
      "\"gateway_uri\":\"\","
      "\"key_configs\":[],"
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"ohttp_gateway\","
      "\"ohttp_gateway\":false,"
      "\"ohttp\":false,"
      "\"oblivious_http\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", ohttp, sizeof ohttp - 1);
    free(req); close(cfd); return;
  }

  /* Residual: mesh/proxy probes hit /.well-known/masque and got not_found.
   * Lab ops is not a MASQUE (RFC 9298) proxy — empty plate. */
  if (is_get &&
      (strcmp(path, "/.well-known/masque") == 0 ||
       strcmp(path, "/.well-known/masque/") == 0 ||
       strncmp(path, "/.well-known/masque/", 19) == 0 ||
       strcmp(path, "/masque") == 0 ||
       strcmp(path, "/masque/") == 0 ||
       strcmp(path, "/api/masque") == 0 ||
       strcmp(path, "/peer/v1/masque") == 0 ||
       strcmp(path, "/.well-known/masque.json") == 0 ||
       strcmp(path, "/masque.json") == 0)) {
    static const char masque[] =
      "{"
      "\"template\":\"\","
      "\"protocols\":[],"
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"masque\","
      "\"masque\":false,"
      "\"http_datagram\":false,"
      "\"proxy\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", masque, sizeof masque - 1);
    free(req); close(cfd); return;
  }

  /* Residual: mesh/DNS probes hit /.well-known/doh|dot and got not_found.
   * Lab ops is not a DoH/DoT resolver — empty plate. */
  if (is_get &&
      (strcmp(path, "/.well-known/doh") == 0 ||
       strcmp(path, "/.well-known/doh/") == 0 ||
       strncmp(path, "/.well-known/doh/", 16) == 0 ||
       strcmp(path, "/.well-known/doh.json") == 0 ||
       strcmp(path, "/.well-known/dot") == 0 ||
       strcmp(path, "/.well-known/dot/") == 0 ||
       strncmp(path, "/.well-known/dot/", 16) == 0 ||
       strcmp(path, "/.well-known/dot.json") == 0 ||
       strcmp(path, "/doh") == 0 ||
       strcmp(path, "/doh/") == 0 ||
       strcmp(path, "/dot") == 0 ||
       strcmp(path, "/dot/") == 0 ||
       strcmp(path, "/api/doh") == 0 ||
       strcmp(path, "/peer/v1/doh") == 0 ||
       strcmp(path, "/api/dot") == 0 ||
       strcmp(path, "/peer/v1/dot") == 0)) {
    static const char doh[] =
      "{"
      "\"doh\":[],"
      "\"dot\":[],"
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"doh\","
      "\"doh\":false,"
      "\"dot\":false,"
      "\"dns_over_https\":false,"
      "\"dns_over_tls\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", doh, sizeof doh - 1);
    free(req); close(cfd); return;
  }

  /* Residual: mesh/ATProto probes hit /.well-known/bluesky and got not_found.
   * Lab ops is not a Bluesky/AppView surface — empty plate
   * (see atproto-did / xrpc-server-did empty plates). */
  if (is_get &&
      (strcmp(path, "/.well-known/bluesky") == 0 ||
       strcmp(path, "/.well-known/bluesky/") == 0 ||
       strncmp(path, "/.well-known/bluesky/", 20) == 0 ||
       strcmp(path, "/bluesky") == 0 ||
       strcmp(path, "/bluesky/") == 0 ||
       strcmp(path, "/api/bluesky") == 0 ||
       strcmp(path, "/peer/v1/bluesky") == 0 ||
       strcmp(path, "/.well-known/bluesky.json") == 0 ||
       strcmp(path, "/bluesky.json") == 0)) {
    static const char bsky[] =
      "{"
      "\"did\":\"\","
      "\"handle\":\"\","
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"bluesky\","
      "\"bluesky\":false,"
      "\"atproto\":false,"
      "\"appview\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", bsky, sizeof bsky - 1);
    free(req); close(cfd); return;
  }

  /* Residual: mesh/Solid probes hit /.well-known/solid and got not_found.
   * Lab ops is not a Solid Pod / identity provider — empty plate. */
  if (is_get &&
      (strcmp(path, "/.well-known/solid") == 0 ||
       strcmp(path, "/.well-known/solid/") == 0 ||
       strncmp(path, "/.well-known/solid/", 18) == 0 ||
       strcmp(path, "/solid") == 0 ||
       strcmp(path, "/solid/") == 0 ||
       strcmp(path, "/api/solid") == 0 ||
       strcmp(path, "/peer/v1/solid") == 0 ||
       strcmp(path, "/.well-known/solid.json") == 0 ||
       strcmp(path, "/solid.json") == 0)) {
    static const char solid[] =
      "{"
      "\"issuer\":\"\","
      "\"storage\":\"\","
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"solid\","
      "\"solid\":false,"
      "\"pod\":false,"
      "\"webid\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", solid, sizeof solid - 1);
    free(req); close(cfd); return;
  }

  /* Residual: mesh/PWA probes hit /.well-known/web-app-origin-association
   * and got not_found. Lab ops has no multi-origin web app association —
   * empty plate. */
  if (is_get &&
      (strcmp(path, "/.well-known/web-app-origin-association") == 0 ||
       strcmp(path, "/.well-known/web-app-origin-association/") == 0 ||
       strncmp(path, "/.well-known/web-app-origin-association/", 39) == 0 ||
       strcmp(path, "/web-app-origin-association") == 0 ||
       strcmp(path, "/web-app-origin-association/") == 0 ||
       strcmp(path, "/api/web-app-origin-association") == 0 ||
       strcmp(path, "/peer/v1/web-app-origin-association") == 0 ||
       strcmp(path, "/.well-known/web-app-origin-association.json") == 0 ||
       strcmp(path, "/web-app-origin-association.json") == 0)) {
    static const char waoa[] =
      "{"
      "\"web_apps\":[],"
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"web_app_origin_association\","
      "\"web_app_origin_association\":false,"
      "\"pwa\":false,"
      "\"related_origins\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", waoa, sizeof waoa - 1);
    free(req); close(cfd); return;
  }

  /* Residual: mesh/DNS probes hit /.well-known/doq|dns-query and got not_found.
   * Lab ops is not a DoQ / DNS-over-HTTP(S) resolver (RFC 9250) — empty plate
   * (see doh|dot empty plate). */
  if (is_get &&
      (strcmp(path, "/.well-known/doq") == 0 ||
       strcmp(path, "/.well-known/doq/") == 0 ||
       strncmp(path, "/.well-known/doq/", 16) == 0 ||
       strcmp(path, "/.well-known/doq.json") == 0 ||
       strcmp(path, "/.well-known/dns-query") == 0 ||
       strcmp(path, "/.well-known/dns-query/") == 0 ||
       strncmp(path, "/.well-known/dns-query/", 22) == 0 ||
       strcmp(path, "/.well-known/dns-query.json") == 0 ||
       strcmp(path, "/doq") == 0 ||
       strcmp(path, "/doq/") == 0 ||
       strcmp(path, "/doq.json") == 0 ||
       strcmp(path, "/dns-query") == 0 ||
       strcmp(path, "/dns-query/") == 0 ||
       strcmp(path, "/dns-query.json") == 0 ||
       strcmp(path, "/api/doq") == 0 ||
       strcmp(path, "/peer/v1/doq") == 0 ||
       strcmp(path, "/api/dns-query") == 0 ||
       strcmp(path, "/peer/v1/dns-query") == 0)) {
    static const char doq[] =
      "{"
      "\"doq\":[],"
      "\"dns_query\":[],"
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"doq\","
      "\"doq\":false,"
      "\"dns_query\":false,"
      "\"dns_over_quic\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", doq, sizeof doq - 1);
    free(req); close(cfd); return;
  }

  /* Residual: mesh/fediverse probes hit /.well-known/activitypub and got
   * not_found. Lab ops is not an ActivityPub actor/server — empty plate
   * (see solid / bluesky / nodeinfo / webfinger empty plates). */
  if (is_get &&
      (strcmp(path, "/.well-known/activitypub") == 0 ||
       strcmp(path, "/.well-known/activitypub/") == 0 ||
       strncmp(path, "/.well-known/activitypub/", 24) == 0 ||
       strcmp(path, "/.well-known/activitypub.json") == 0 ||
       strcmp(path, "/activitypub") == 0 ||
       strcmp(path, "/activitypub/") == 0 ||
       strcmp(path, "/activitypub.json") == 0 ||
       strcmp(path, "/api/activitypub") == 0 ||
       strcmp(path, "/peer/v1/activitypub") == 0)) {
    static const char apub[] =
      "{"
      "\"actor\":\"\","
      "\"inbox\":\"\","
      "\"outbox\":\"\","
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"activitypub\","
      "\"activitypub\":false,"
      "\"fediverse\":false,"
      "\"actor\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", apub, sizeof apub - 1);
    free(req); close(cfd); return;
  }

  /* Residual: mesh/agent probes hit /.well-known/a2a|agent-card and got
   * not_found. Lab ops peer is not an A2A agent endpoint — empty plate
   * (see agent-card.json / mcp.json empty plates). */
  if (is_get &&
      (strcmp(path, "/.well-known/a2a") == 0 ||
       strcmp(path, "/.well-known/a2a/") == 0 ||
       strncmp(path, "/.well-known/a2a/", 16) == 0 ||
       strcmp(path, "/.well-known/a2a.json") == 0 ||
       strcmp(path, "/.well-known/a2a-agent-card.json") == 0 ||
       strcmp(path, "/.well-known/agent-card") == 0 ||
       strcmp(path, "/a2a") == 0 ||
       strcmp(path, "/a2a/") == 0 ||
       strcmp(path, "/a2a.json") == 0 ||
       strcmp(path, "/api/a2a") == 0 ||
       strcmp(path, "/peer/v1/a2a") == 0)) {
    static const char a2a[] =
      "{"
      "\"agents\":[],"
      "\"url\":\"\","
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"a2a\","
      "\"a2a\":false,"
      "\"agent_card\":false,"
      "\"agent2agent\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", a2a, sizeof a2a - 1);
    free(req); close(cfd); return;
  }

  /* Residual: mesh/Privacy Pass probes hit /.well-known/token-issuer-directory
   * |private-token-issuer-directory and got not_found. Lab ops is not a
   * Privacy Pass / private token issuer — empty plate (see privacy_pass). */
  if (is_get &&
      (strcmp(path, "/.well-known/token-issuer-directory") == 0 ||
       strcmp(path, "/.well-known/token-issuer-directory/") == 0 ||
       strncmp(path, "/.well-known/token-issuer-directory/", 34) == 0 ||
       strcmp(path, "/.well-known/token-issuer-directory.json") == 0 ||
       strcmp(path, "/.well-known/private-token-issuer-directory") == 0 ||
       strcmp(path, "/.well-known/private-token-issuer-directory/") == 0 ||
       strncmp(path, "/.well-known/private-token-issuer-directory/", 42) == 0 ||
       strcmp(path, "/token-issuer-directory") == 0 ||
       strcmp(path, "/private-token-issuer-directory") == 0 ||
       strcmp(path, "/api/token-issuer-directory") == 0 ||
       strcmp(path, "/peer/v1/token-issuer-directory") == 0 ||
       strcmp(path, "/api/private-token-issuer-directory") == 0 ||
       strcmp(path, "/peer/v1/private-token-issuer-directory") == 0)) {
    static const char tid[] =
      "{"
      "\"issuer-request-uri\":\"\","
      "\"token-keys\":[],"
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"token_issuer_directory\","
      "\"token_issuer_directory\":false,"
      "\"private_token_issuer\":false,"
      "\"privacy_pass\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", tid, sizeof tid - 1);
    free(req); close(cfd); return;
  }

  /* Residual: mesh/MTA probes hit /.well-known/tls-rpt and got not_found.
   * Lab ops is not a mail TLS reporting endpoint (RFC 8460) — empty plate
   * (see mta-sts empty plate). */
  if (is_get &&
      (strcmp(path, "/.well-known/tls-rpt") == 0 ||
       strcmp(path, "/.well-known/tls-rpt/") == 0 ||
       strncmp(path, "/.well-known/tls-rpt/", 20) == 0 ||
       strcmp(path, "/.well-known/tls-rpt.json") == 0 ||
       strcmp(path, "/tls-rpt") == 0 ||
       strcmp(path, "/tls-rpt.json") == 0 ||
       strcmp(path, "/api/tls-rpt") == 0 ||
       strcmp(path, "/peer/v1/tls-rpt") == 0)) {
    static const char tlsrpt[] =
      "{"
      "\"rua\":[],"
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"tls_rpt\","
      "\"tls_rpt\":false,"
      "\"mail_tls_reporting\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", tlsrpt, sizeof tlsrpt - 1);
    free(req); close(cfd); return;
  }

  /* Residual: mesh/MTA probes hit /.well-known/bimi and got not_found.
   * Lab ops is not a BIMI brand indicator host — empty plate
   * (see mta-sts / tls-rpt empty plates). */
  if (is_get &&
      (strcmp(path, "/.well-known/bimi") == 0 ||
       strcmp(path, "/.well-known/bimi/") == 0 ||
       strncmp(path, "/.well-known/bimi/", 17) == 0 ||
       strcmp(path, "/.well-known/bimi.json") == 0 ||
       strcmp(path, "/bimi") == 0 ||
       strcmp(path, "/bimi/") == 0 ||
       strcmp(path, "/bimi.json") == 0 ||
       strcmp(path, "/api/bimi") == 0 ||
       strcmp(path, "/peer/v1/bimi") == 0)) {
    static const char bimi[] =
      "{"
      "\"location\":\"\","
      "\"authority\":\"\","
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"bimi\","
      "\"bimi\":false,"
      "\"brand_indicators\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", bimi, sizeof bimi - 1);
    free(req); close(cfd); return;
  }

  /* Residual: mesh/app probes hit /.well-known/statements.json and got
   * not_found. Lab ops has no Twitter/X app association statements —
   * empty plate (see assetlinks / apple-app-site-association). */
  if (is_get &&
      (strcmp(path, "/.well-known/statements.json") == 0 ||
       strcmp(path, "/.well-known/statements.json/") == 0 ||
       strncmp(path, "/.well-known/statements.json/", 28) == 0 ||
       strcmp(path, "/.well-known/statements") == 0 ||
       strcmp(path, "/statements.json") == 0 ||
       strcmp(path, "/api/statements.json") == 0 ||
       strcmp(path, "/peer/v1/statements.json") == 0 ||
       strcmp(path, "/api/statements") == 0 ||
       strcmp(path, "/peer/v1/statements") == 0)) {
    static const char stmts[] =
      "{"
      "\"statements\":[],"
      "\"x-nanobot\":{"
      "\"schema\":\"nanobot.peer_http.v1\","
      "\"ok\":true,"
      "\"action\":\"statements\","
      "\"statements\":false,"
      "\"twitter_app_association\":false,"
      "\"auth\":\"browser_device_code\","
      "\"auth_plate\":\"/api/auth\","
      "\"product_wire\":\"smx2\","
      "\"peer_http\":\"lab_ops_only\","
      "\"peer_http_is_product_bus\":false,"
      "\"share\":\"state_matrix_only\","
      "\"hold_flash\":1,"
      "\"llm_is_commander\":false,"
      "\"python\":0"
      "}"
      "}";
    http_response(cfd, 200, "application/json", stmts, sizeof stmts - 1);
    free(req); close(cfd); return;
  }

  /* Residual: crawler/mesh probes hit /humans.txt and got not_found.
   * humanstxt.org plate — lab ops peer credits + public surface pointers only. */
  if (is_get && (strcmp(path, "/humans.txt") == 0 || strcmp(path, "/humans.txt/") == 0 ||
                 strcmp(path, "/api/humans.txt") == 0 ||
                 strcmp(path, "/peer/v1/humans.txt") == 0)) {
    static const char humans[] =
      "/* TEAM */\n"
      "Maintainer: Abyss-c0re\n"
      "Site: https://github.com/Abyss-c0re/nanobot\n"
      "\n"
      "/* THANKS */\n"
      "Community: peer mesh operators\n"
      "\n"
      "/* SITE */\n"
      "Last update: 2026-08-10\n"
      "Standards: peer_http.v1, humans.txt, security.txt, robots.txt\n"
      "Software: nanobot peer HTTP (lab ops only — not product SMX2)\n"
      "Language: en\n"
      "Doctype: JSON peer bus (+ optional www static)\n"
      "\n"
      "/* NOTE */\n"
      "This listener is lab/ops peer HTTP, not a public product site.\n"
      "See README.md, INSTALL.md, SECURITY.md, docs/ for humans.\n";
    http_response(cfd, 200, "text/plain; charset=utf-8", humans, sizeof humans - 1);
    free(req); close(cfd); return;
  }

  /* Residual: crawler/mesh probes hit /sitemap.xml and got not_found while
   * robots Disallow:/. Empty urlset — lab ops has no public crawl surface. */
  if (is_get && (strcmp(path, "/sitemap.xml") == 0 || strcmp(path, "/sitemap.xml/") == 0 ||
                 strcmp(path, "/sitemap_index.xml") == 0 ||
                 strcmp(path, "/sitemap_index.xml/") == 0 ||
                 strcmp(path, "/api/sitemap.xml") == 0 ||
                 strcmp(path, "/peer/v1/sitemap.xml") == 0)) {
    static const char sitemap[] =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<!-- nanobot peer HTTP — lab ops only (not product SMX2); robots Disallow:/ -->\n"
      "<urlset xmlns=\"http://www.sitemaps.org/schemas/sitemap/0.9\">\n"
      "</urlset>\n";
    http_response(cfd, 200, "application/xml; charset=utf-8", sitemap, sizeof sitemap - 1);
    free(req); close(cfd); return;
  }

  /* Residual: mesh/AI probes hit /llms.txt|/ai.txt and got not_found.
   * llmstxt.org-style plate: lab ops peer is not a public product site. */
  if (is_get && (strcmp(path, "/llms.txt") == 0 || strcmp(path, "/llms.txt/") == 0 ||
                 strcmp(path, "/ai.txt") == 0 || strcmp(path, "/ai.txt/") == 0 ||
                 strcmp(path, "/api/llms.txt") == 0 ||
                 strcmp(path, "/peer/v1/llms.txt") == 0)) {
    static const char llms[] =
      "# nanobot peer HTTP\n"
      "\n"
      "> Lab/ops peer bus (schema nanobot.peer_http.v1). Not product SMX2.\n"
      "\n"
      "This listener is a private lab mesh endpoint. robots.txt Disallow: /.\n"
      "Do not index as a public product site. Prefer open source docs on GitHub.\n"
      "\n"
      "## Discovery\n"
      "- /schema — wire plates + actions\n"
      "- /openapi.json — OpenAPI document\n"
      "- /peer/v1/health — liveness\n"
      "- /peer/v1/info — session/info plate\n"
      "- /humans.txt — maintainer pointers\n"
      "- /security.txt — vulnerability contact\n"
      "\n"
      "## Source\n"
      "- https://github.com/Abyss-c0re/nanobot\n"
      "- README.md, INSTALL.md, SECURITY.md, docs/\n"
      "\n"
      "## Policy\n"
      "- peer_http is lab_ops_only; product_wire remains smx2 elsewhere\n"
      "- Mutating routes require peer token on LAN\n";
    http_response(cfd, 200, "text/plain; charset=utf-8", llms, sizeof llms - 1);
    free(req); close(cfd); return;
  }

  /* Residual: browser/mesh probes hit /manifest.json|/manifest.webmanifest|
   * /site.webmanifest and got not_found (no www PWA). Minimal lab-ops manifest. */
  if (is_get && (strcmp(path, "/manifest.json") == 0 ||
                 strcmp(path, "/manifest.json/") == 0 ||
                 strcmp(path, "/manifest.webmanifest") == 0 ||
                 strcmp(path, "/manifest.webmanifest/") == 0 ||
                 strcmp(path, "/site.webmanifest") == 0 ||
                 strcmp(path, "/site.webmanifest/") == 0 ||
                 strcmp(path, "/api/manifest.json") == 0 ||
                 strcmp(path, "/peer/v1/manifest.json") == 0)) {
    char body[900];
    char *ver = ng_json_escape(NG_VERSION);
    int n = snprintf(body, sizeof body,
      "{"
      "\"name\":\"nanobot peer HTTP\","
      "\"short_name\":\"nanobot\","
      "\"description\":\"Lab ops peer bus (not product SMX2).\","
      "\"start_url\":\"/\","
      "\"display\":\"browser\","
      "\"background_color\":\"#0a0a0a\","
      "\"theme_color\":\"#00e5ff\","
      "\"lang\":\"en\","
      "\"version\":\"%s\","
      "\"x-nanobot\":{"
        "\"schema\":\"nanobot.peer_http.v1\","
        "\"action\":\"manifest\","
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
    http_response(cfd, 200, "application/manifest+json", body, (size_t)n);
    free(req); close(cfd); return;
  }

  /* Residual: mesh OpenAPI probes hit /openapi.json|/openapi.yaml (and dual-wire
   * aliases) while only static www exclusion reserved the path — always 404.
   * Residual: /swagger.json|/swagger|/docs|/api/docs (and dual-wire) still
   * not_found after openapi plate — common mesh OpenAPI discovery aliases. */
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
                 strcmp(path, "/peer/v1/openapi.yaml/") == 0 ||
                 strcmp(path, "/swagger.json") == 0 || strcmp(path, "/swagger.json/") == 0 ||
                 strcmp(path, "/swagger") == 0 || strcmp(path, "/swagger/") == 0 ||
                 strcmp(path, "/api/swagger") == 0 || strcmp(path, "/api/swagger/") == 0 ||
                 strcmp(path, "/api/swagger.json") == 0 ||
                 strcmp(path, "/api/swagger.json/") == 0 ||
                 strcmp(path, "/api/v1/swagger") == 0 ||
                 strcmp(path, "/api/v1/swagger/") == 0 ||
                 strcmp(path, "/peer/v1/swagger") == 0 ||
                 strcmp(path, "/peer/v1/swagger/") == 0 ||
                 strcmp(path, "/peer/v1/swagger.json") == 0 ||
                 strcmp(path, "/peer/v1/swagger.json/") == 0 ||
                 strcmp(path, "/docs") == 0 || strcmp(path, "/docs/") == 0 ||
                 strcmp(path, "/api/docs") == 0 || strcmp(path, "/api/docs/") == 0 ||
                 strcmp(path, "/peer/v1/docs") == 0 ||
                 strcmp(path, "/peer/v1/docs/") == 0)) {
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
        "    get: {summary: favicon SVG}\n"
        "  /favicon.svg:\n"
        "    get: {summary: favicon SVG alias}\n",
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
            "\"responses\":{\"200\":{\"description\":\"ok\"}}}},"
          "\"/favicon.svg\":{\"get\":{\"summary\":\"favicon SVG alias\","
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
