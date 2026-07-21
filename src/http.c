#include "http.h"
#include "mcp_remote.h"
#include "auth.h"
#include "shell.h"
#include "shell_gate.h"
#include "util.h"
#include "hub_local.h"
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
#include <sys/wait.h>
#include <signal.h>
#include <time.h>

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
 * Normal chunks → {"delta":"..."}.
 * Chunks starting with 0x1e (RS) → raw JSON event (tool / thinking) for the client. */
typedef struct { int fd; } chat_sse_ud;
static void chat_sse_delta(void *p, const char *chunk, size_t n) {
  chat_sse_ud *u = (chat_sse_ud *)p;
  if (!chunk || !n || !u || u->fd < 0) return;
  /* Structured event from agent (tool / thinking) */
  if ((unsigned char)chunk[0] == 0x1e && n > 1) {
    char *line = NULL;
    if (asprintf(&line, "data: %.*s\n\n", (int)(n - 1), chunk + 1) > 0 && line) {
      send_all(u->fd, line, strlen(line));
      free(line);
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
  if (asprintf(&line, "data: {\"delta\":\"%s\"}\n\n", esc) > 0 && line) {
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
    http_json(cfd, 503,
      "{\"error\":\"peer_token not configured\",\"need_peer_token\":true}");
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
  if (!ok) {
    char *body0 = strstr(req, "\r\n\r\n");
    body0 = body0 ? body0 + 4 : "";
    char *bt = ng_json_get_string(body0, "peer_token");
    if (bt && nb_ct_eq(bt, strlen(bt), need, strlen(need))) ok = 1;
    free(bt);
  }
  free(need);
  if (!ok) {
    http_json(cfd, 401,
      "{\"error\":\"invalid or missing peer token\",\"need_peer_token\":true,"
      "\"hint\":\"Header X-Nanobot-Peer-Token or body peer_token\"}");
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

static char *read_request(int fd, size_t *out_len) {
  size_t cap = 4096, len = 0;
  char *buf = malloc(cap);
  if (!buf) return NULL;
  while (1) {
    if (len + 1024 > cap) {
      cap *= 2;
      if (cap > 1024 * 1024) break;
      char *nbuf = realloc(buf, cap);
      if (!nbuf) break;
      buf = nbuf;
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
      size_t cl = 0;
      const char *clh = strcasestr(buf, "Content-Length:");
      if (clh) cl = (size_t)strtoul(clh + 15, NULL, 10);
      if (len >= hlen + cl) break;
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

  /* advance pending device login on any request */
  if (session && session->login_pending) {
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
        !strcmp(path, "/activate") || !strcmp(path, "/openapi.yaml"))
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
        http_text(cfd, 404, "not found\n");
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
    const char *body =
      "{\"ok\":true,\"service\":\"nanobot\",\"role\":\"cli-api\","
      "\"hint\":\"CLI + peer/JSON API + optional MCP; optional --www for static files\","
      "\"endpoints\":[\"/peer/v1/info\",\"/peer/v1/prompt\",\"/peer/v1/shell\",\"/peer/v1/jobs\","
      "\"/peer/v1/task\",\"/peer/v1/models\",\"/api/chat\",\"/api/auth\",\"/api/task\",\"/api/settings\",\"/api/models\"]}";
    http_response(cfd, 200, "application/json", body, strlen(body));
    free(req); close(cfd); return;
  }

  if (is_get && strcmp(path, "/activate") == 0) {
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
      http_text(cfd, 503, "login not ready\n");
    }
    free(req); close(cfd); return;
  }

  if (is_get && (strcmp(path, "/api/auth") == 0 || strcmp(path, "/api/status") == 0)) {
    int need_browser = agent && ng_agent_needs_browser_session(agent);
    /* Soft-expired access_token still counts as signed-in after a successful refresh. */
    if (need_browser && session && !ng_session_valid(session))
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
    char body[2560];
    /* needs_browser = backend TYPE uses browser OAuth (always true for Grok).
     * login_required = user must open Connect NOW (not signed in). Apps must
     * use login_required / signed_in — not needs_browser — or they thrash Connect. */
    int login_required = need_browser && !signed_in;
    int n = snprintf(body, sizeof body,
      "{\"ok\":true,\"version\":\"%s\",\"model\":\"%s\",\"signed_in\":%s,"
      "\"login_pending\":%s,\"login_required\":%s,\"user_code\":\"%s\","
      "\"verification_uri\":\"%s\",\"verification_uri_complete\":\"%s\","
      "\"workdir\":\"%s\",\"auth\":\"%s\",\"backend\":\"%s\","
      "\"base_url\":\"%s\",\"needs_browser\":%s}",
      NG_VERSION,
      agent && agent->model ? agent->model : "",
      signed_in ? "true" : "false",
      (need_browser && session && session->login_pending) ? "true" : "false",
      login_required ? "true" : "false",
      uc ? uc : "",
      vu ? vu : "",
      vuc ? vuc : "",
      ng_workdir(),
      auth_mode,
      backend,
      base_esc ? base_esc : "",
      need_browser ? "true" : "false");
    http_response(cfd, 200, "application/json", body, (size_t)n);
    free(vu); free(vuc); free(uc); free(base_esc);
    free(req); close(cfd); return;
  }

  if (is_post && strcmp(path, "/api/auth/start") == 0) {
    if (!require_peer_auth(cfd, req, 1)) { free(req); close(cfd); return; }
    if (!session || !agent) {
      http_json(cfd, 500, "{\"error\":\"no session\"}");
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
          http_json(cfd, 500, "{\"error\":\"device login failed (network/DNS?)\"}");
          free(req); close(cfd); return;
        }
      }
    }
    if (ng_session_valid(session)) {
      http_response(cfd, 200, "application/json",
        "{\"ok\":true,\"signed_in\":true,\"login_pending\":false,"
        "\"backend\":\"grok\",\"message\":\"already connected\"}", 95);
      free(req); close(cfd); return;
    }
    char *vuc = session->verification_uri_complete
      ? ng_json_escape(session->verification_uri_complete)
      : ng_json_escape(session->verification_uri ? session->verification_uri : "");
    char *vu = session->verification_uri ? ng_json_escape(session->verification_uri) : NULL;
    char *uc = ng_json_escape(session->user_code ? session->user_code : "");
    char *body = NULL;
    asprintf(&body,
      "{\"ok\":true,\"signed_in\":false,\"login_pending\":true,"
      "\"backend\":\"grok\",\"needs_browser\":true,"
      "\"user_code\":\"%s\","
      "\"verification_uri\":\"%s\","
      "\"verification_uri_complete\":\"%s\","
      "\"activate_path\":\"/activate\","
      "\"message\":\"Open the link to authorize Grok in your browser\"}",
      uc ? uc : "",
      vu ? vu : "",
      vuc ? vuc : "");
    http_response(cfd, 200, "application/json", body ? body : "{}", body ? strlen(body) : 2);
    free(body); free(vuc); free(vu); free(uc);
    free(req); close(cfd); return;
  }

  /* List models: GET {base}/models (OpenAI-compatible; Grok session or local). */
  if (is_get && (strcmp(path, "/api/models") == 0 || strcmp(path, "/peer/v1/models") == 0)) {
    if (!agent) {
      http_json(cfd, 500, "{\"error\":\"no agent\"}");
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
    /* Surface upstream error/auth failure when list is empty so UIs can explain. */
    char *hint = NULL;
    if (!nonempty && raw && raw[0]) {
      if (strstr(raw, "Unauthenticated") || strstr(raw, "auth") || strstr(raw, "expired")
          || strstr(raw, "Invalid") || strstr(raw, "error")) {
        /* short escape: prefer JSON "error" string if present */
        char *e = ng_json_get_string(raw, "error");
        if (e && e[0]) {
          hint = ng_json_escape(e);
          free(e);
        } else {
          char trunc[180];
          size_t n = strlen(raw);
          if (n > 160) n = 160;
          memcpy(trunc, raw, n);
          trunc[n] = 0;
          hint = ng_json_escape(trunc);
        }
      }
    }
    if (ok) {
      if (hint && hint[0]) {
        asprintf(&out,
          "{\"ok\":true,\"base_url\":\"%s\",\"model\":\"%s\",\"models\":%s,\"error\":\"%s\"}",
          base_e ? base_e : "", cur ? cur : "",
          ids && ids[0] == '[' ? ids : "[]", hint);
      } else {
        asprintf(&out,
          "{\"ok\":true,\"base_url\":\"%s\",\"model\":\"%s\",\"models\":%s}",
          base_e ? base_e : "", cur ? cur : "",
          ids && ids[0] == '[' ? ids : "[]");
      }
    } else {
      char *err = hint ? hint : ng_json_escape(raw ? raw : "fetch failed");
      hint = NULL; /* ownership moved */
      asprintf(&out,
        "{\"ok\":false,\"base_url\":\"%s\",\"model\":\"%s\",\"models\":[],\"error\":\"%s\"}",
        base_e ? base_e : "", cur ? cur : "", err ? err : "fetch failed");
      free(err);
    }
    free(hint);
    http_response(cfd, 200, "application/json", out ? out : "{}", out ? strlen(out) : 2);
    free(out); free(raw); free(ids); free(cur); free(base_e);
    free(req); close(cfd); return;
  }

  /* Settings: select backend (grok | local) + optional base/model */
  if ((is_post || is_put) && strcmp(path, "/api/settings") == 0) {
    if (!require_peer_auth(cfd, req, 1)) { free(req); close(cfd); return; }
    if (!agent) {
      http_json(cfd, 500, "{\"error\":\"no agent\"}");
      free(req); close(cfd); return;
    }
    char *body = strstr(req, "\r\n\r\n");
    body = body ? body + 4 : "";
    char *backend = ng_json_get_string(body, "backend");
    char *base = ng_json_get_string(body, "base_url");
    if (!base) base = ng_json_get_string(body, "base");
    char *model = ng_json_get_string(body, "model");
    if (!backend && !base && !(model && model[0])) {
      free(backend); free(base); free(model);
      http_json(cfd, 400, "{\"error\":\"need backend (grok|local), base_url, or model\"}");
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
    ng_log("settings: backend=%s base=%s model=%s",
           ng_agent_backend_kind(agent),
           agent->base_url ? agent->base_url : "?",
           agent->model ? agent->model : "?");
    char *be = ng_json_escape(agent->base_url ? agent->base_url : "");
    char *me = ng_json_escape(agent->model ? agent->model : "");
    char *out = NULL;
    asprintf(&out,
      "{\"ok\":true,\"backend\":\"%s\",\"base_url\":\"%s\",\"model\":\"%s\","
      "\"needs_browser\":%s,\"signed_in\":%s}",
      ng_agent_backend_kind(agent),
      be ? be : "",
      me ? me : "",
      ng_agent_needs_browser_session(agent) ? "true" : "false",
      (ng_agent_needs_browser_session(agent) && session && ng_session_valid(session))
        ? "true" : (ng_agent_needs_browser_session(agent) ? "false" : "true"));
    http_response(cfd, 200, "application/json", out ? out : "{}", out ? strlen(out) : 2);
    free(out); free(be); free(me);
    free(backend); free(base); free(model);
    free(req); close(cfd); return;
  }


  if (is_post && strcmp(path, "/api/backend") == 0) {
    if (!require_peer_auth(cfd, req, 1)) { free(req); close(cfd); return; }
    /* alias: switch backend; same as /api/settings */
    if (!agent) {
      http_json(cfd, 500, "{\"error\":\"no agent\"}");
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
    char body[768];
    int n = snprintf(body, sizeof body,
      "{\"ok\":true,\"backend\":\"%s\",\"base_url\":\"%s\",\"model\":\"%s\",\"needs_browser\":%s}",
      kind, be ? be : "", me ? me : "",
      ng_agent_needs_browser_session(agent) ? "true" : "false");
    http_response(cfd, 200, "application/json", body, (size_t)n);
    free(be); free(me);
    free(req); close(cfd); return;
  }

  if (is_get && strcmp(path, "/api/log") == 0) {
    char *tail = ng_read_log_tail(16 * 1024);
    char *esc = ng_json_escape(tail ? tail : "");
    char *body = NULL;
    asprintf(&body, "{\"log\":\"%s\"}", esc ? esc : "");
    http_response(cfd, 200, "application/json", body ? body : "{}", body ? strlen(body) : 2);
    free(tail); free(esc); free(body);
    free(req); close(cfd); return;
  }

  /* Active multi-step task board (task_plan / task_done tools) */
  if (is_get && (strcmp(path, "/api/task") == 0 || strcmp(path, "/peer/v1/task") == 0)) {
    if (!require_peer_auth(cfd, req, 1)) { free(req); close(cfd); return; }
    char tpath[700];
    snprintf(tpath, sizeof tpath, "%s/tasks/active.json", ng_workdir());
    char *raw = ng_read_file(tpath, NULL);
    if (!raw || !raw[0]) {
      free(raw);
      http_json(cfd, 200, "{\"ok\":true,\"open\":false,\"task\":null}");
    } else {
      char *body = NULL;
      asprintf(&body, "{\"ok\":true,\"open\":true,\"task\":%s}", raw);
      http_response(cfd, 200, "application/json", body ? body : "{}", body ? strlen(body) : 2);
      free(body);
      free(raw);
    }
    free(req); close(cfd); return;
  }

  /* ---- Outbound MCP servers (agent connects TO remote MCPs) ---- */
  if (is_get && (strcmp(path, "/api/mcp/servers") == 0 ||
                 strcmp(path, "/peer/v1/mcp/servers") == 0)) {
    if (!require_peer_auth(cfd, req, 1)) { free(req); close(cfd); return; }
    char *body = ng_mcp_servers_list_json();
    http_response(cfd, 200, "application/json", body ? body : "{}", body ? strlen(body) : 2);
    free(body);
    free(req); close(cfd); return;
  }
  if (is_post && (strcmp(path, "/api/mcp/servers") == 0 ||
                  strcmp(path, "/peer/v1/mcp/servers") == 0)) {
    if (!require_peer_auth(cfd, req, 1)) { free(req); close(cfd); return; }
    char *body = strstr(req, "\r\n\r\n");
    body = body ? body + 4 : "";
    int rc = ng_mcp_servers_save_raw(body);
    if (rc != 0) {
      http_json(cfd, 400, "{\"ok\":false,\"error\":\"invalid mcp_servers json\"}");
    } else {
      char *list = ng_mcp_servers_list_json();
      http_response(cfd, 200, "application/json", list ? list : "{\"ok\":true}",
                    list ? strlen(list) : 11);
      free(list);
    }
    free(req); close(cfd); return;
  }
  if (is_post && (strcmp(path, "/api/mcp/probe") == 0 ||
                  strcmp(path, "/peer/v1/mcp/probe") == 0)) {
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
    if ((!prompt || !prompt[0]) && !has_img) {
      free(prompt); free(image_b64); free(image_mime); free(images_json);
      http_json(cfd, 400, "{\"error\":\"missing prompt or image/attachments\"}");
      free(req); close(cfd); return;
    }
    if (!prompt) prompt = strdup("");
    /* Outer shell always: @! shell. Local/llama backend: no browser session. */
    int shell_only = (prompt[0] == '@' && prompt[1] == '!' && !has_img);
    int need_browser = agent && ng_agent_needs_browser_session(agent);
    if (!shell_only && need_browser && (!session || !ng_session_valid(session))) {
      if (session && session->login_pending) ng_session_poll_login(session);
      if (!session || !ng_session_valid(session)) {
        free(prompt); free(image_b64); free(image_mime); free(images_json);
        http_json(cfd, 401, "{\"error\":\"Grok backend needs activation link, or use --offline / @! cmd\",\"need_login\":true}");
        free(req); close(cfd); return;
      }
    }
    /* Real-time typing: stream=true → SSE deltas (final free-text only). */
    int want_stream = (strstr(body, "\"stream\":true") != NULL)
                   || (strstr(body, "\"stream\": true") != NULL);
    if (want_stream && !shell_only) {
      char hdr[256];
      int hn = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n");
      send_all(cfd, hdr, (size_t)hn);
      chat_sse_ud ud = { .fd = cfd };
      char *reply = ng_agent_run_attachments(agent, prompt, image_b64, image_mime,
                                             images_json, 1, chat_sse_delta, &ud);
      char *esc = ng_json_escape(reply ? reply : "");
      char *fin = NULL;
      if (asprintf(&fin, "data: {\"done\":true,\"reply\":\"%s\"}\n\n", esc ? esc : "") > 0 && fin) {
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
    asprintf(&jb, "{\"reply\":\"%s\"}", esc ? esc : "");
    http_response(cfd, 200, "application/json", jb ? jb : "{}", jb ? strlen(jb) : 2);
    free(prompt); free(image_b64); free(image_mime); free(images_json);
    free(reply); free(esc); free(jb);
    free(req); close(cfd); return;
  }


  if (is_get && (strcmp(path, "/api/v1/resources") == 0 || strcmp(path, "/peer/v1/resources") == 0)) {
    char *body = ng_resources_json();
    http_response(cfd, 200, "application/json", body ? body : "{}", body ? strlen(body) : 2);
    free(body);
    free(req); close(cfd); return;
  }

  /* ---- Peer bus for other agents / sessions ---- */
  if (is_get && strcmp(path, "/peer/v1/health") == 0) {
    char body[256];
    int n = snprintf(body, sizeof body,
      "{\"ok\":true,\"service\":\"nanobot-peer\",\"version\":\"%s\","
      "\"role\":\"session-bus\"}", NG_VERSION);
    http_response(cfd, 200, "application/json", body, (size_t)n);
    free(req); close(cfd); return;
  }

  if (is_get && (strcmp(path, "/peer/v1/info") == 0 || strcmp(path, "/peer/v1/hello") == 0)) {
    int signed_in = session && ng_session_valid(session);
    char body[512];
    int n = snprintf(body, sizeof body,
      "{\"ok\":true,\"service\":\"nanobot-peer\",\"version\":\"%s\","
      "\"signed_in\":%s,\"model\":\"%s\",\"workdir\":\"%s\","
      "\"tools\":[\"prompt\",\"shell\"],"
      "\"endpoints\":["
      "\"GET /peer/v1/health\","
      "\"GET /peer/v1/info\","
      "\"POST /peer/v1/prompt\","
      "\"POST /peer/v1/shell\""
      "]}",
      NG_VERSION,
      signed_in ? "true" : "false",
      agent->model ? agent->model : "",
      ng_workdir());
    http_response(cfd, 200, "application/json", body, (size_t)n);
    free(req); close(cfd); return;
  }

  /* Control plane: shell / watcher / ui — persisted in $HOME/settings */
  if (is_get && strcmp(path, "/peer/v1/control") == 0) {
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
    char body[512];
    int n = snprintf(body, sizeof body,
      "{\"ok\":true,\"shell_enabled\":%s,\"watcher_enabled\":%s,\"ui_enabled\":%s,"
      "\"www\":\"%s\",\"settings\":\"%s\",\"status\":\"%s\","
      "\"persist\":true,\"note\":\"settings survive reboot under NANOBOT_HOME/settings\"}",
      shell_on ? "true" : "false", watch_on ? "true" : "false", ui_on ? "true" : "false",
      www ? www : (cfg->www_root ? cfg->www_root : ""),
      ng_settings_path(),
      (session && ng_session_valid(session)) ? "online" : "offline");
    http_response(cfd, 200, "application/json", body, (size_t)n);
    free(www);
    free(req); close(cfd); return;
  }

  if (is_post && strcmp(path, "/peer/v1/control") == 0) {
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
    if (!svc || !act) {
      free(svc); free(act);
      http_json(cfd, 400, "{\"error\":\"need service shell|watcher|ui and action on|off\"}");
      free(req); close(cfd); return;
    }
    char pathf[640];
    int ok = 0;
    int on = (!strcmp(act, "on") || !strcmp(act, "enable"));
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
      http_json(cfd, 400, "{\"error\":\"unknown service (shell|watcher|ui)\"}");
      free(req); close(cfd); return;
    }
    char *jb = NULL;
    asprintf(&jb,
      "{\"ok\":%s,\"service\":\"%s\",\"action\":\"%s\",\"persist\":true,"
      "\"settings\":\"%s\",\"note\":\"reboot uses run.sh + settings file\"}",
      ok ? "true" : "false", svc, act, ng_settings_path());
    http_response(cfd, ok ? 200 : 400, "application/json", jb ? jb : "{}", jb ? strlen(jb) : 2);
    free(svc); free(act); free(jb);
    free(req); close(cfd); return;
  }

  /* Async jobs: accept work immediately, poll result — keeps peer responsive */
  if (is_post && (strcmp(path, "/peer/v1/jobs") == 0 || strcmp(path, "/peer/v1/job") == 0)) {
    if (!require_peer_auth(cfd, req, 0)) { free(req); close(cfd); return; }
    char *body = strstr(req, "\r\n\r\n");
    body = body ? body + 4 : "";
    char *prompt = ng_json_get_string(body, "prompt");
    char *cmd = ng_json_get_string(body, "command");
    if (!cmd) cmd = ng_json_get_string(body, "cmd");
    char *kind = ng_json_get_string(body, "kind"); /* prompt|shell|watcher */
    if (!kind) {
      if (prompt) kind = strdup("prompt");
      else if (cmd) kind = strdup("shell");
      else kind = strdup("prompt");
    }
    if ((!prompt || !prompt[0]) && (!cmd || !cmd[0])) {
      free(prompt); free(cmd); free(kind);
      http_json(cfd, 400, "{\"error\":\"need prompt or command\"}");
      free(req); close(cfd); return;
    }
    char jdir[640];
    snprintf(jdir, sizeof jdir, "%s/jobs", ng_workdir());
    mkdir(jdir, 0755);
    char id[32];
    snprintf(id, sizeof id, "%ld%04d", (long)time(NULL), (int)(getpid() % 10000));
    char meta[768];
    int mn = snprintf(meta, sizeof meta,
      "{\"id\":\"%s\",\"status\":\"queued\",\"kind\":\"%s\"}\n", id, kind);
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
      char runm[256];
      int rn = snprintf(runm, sizeof runm,
        "{\"id\":\"%s\",\"status\":\"running\",\"kind\":\"%s\"}\n", id, kind);
      ng_write_file(mpath, runm, (size_t)rn);
      ng_hub_event("job.running", "id", id, "kind", kind);
      char *payload = ng_read_file(pp, NULL);
      if (!strcmp(kind, "shell") || (cmd && cmd[0] && !prompt)) {
        ng_cmd_result cr = ng_run_command(payload ? payload : "",
          agent->timeout_sec > 0 ? agent->timeout_sec : 60);
        char *esc = ng_json_escape(cr.output ? cr.output : "");
        char *jb = NULL;
        asprintf(&jb,
          "{\"id\":\"%s\",\"status\":\"done\",\"kind\":\"shell\",\"exit\":%d,\"output\":\"%s\"}",
          id, cr.exit_code, esc ? esc : "");
        if (jb) { ng_write_file(mpath, jb, strlen(jb)); free(jb); }
        free(esc);
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
          "{\"id\":\"%s\",\"status\":\"done\",\"kind\":\"watcher\",\"enabled\":true,\"prompt\":\"%s\"}",
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
          "{\"id\":\"%s\",\"status\":\"done\",\"kind\":\"prompt\",\"reply\":\"%s\"}",
          id, esc ? esc : "");
        if (jb) { ng_write_file(mpath, jb, strlen(jb)); free(jb); }
        free(esc); free(reply); free(payload);
        ng_hub_event("job.done", "id", id, "kind", "prompt");
      }
      _exit(0);
    }
    char *ack = NULL;
    asprintf(&ack,
      "{\"ok\":true,\"id\":\"%s\",\"status\":\"queued\",\"poll\":\"/peer/v1/jobs/%s\"}",
      id, id);
    http_response(cfd, 202, "application/json", ack ? ack : "{}", ack ? strlen(ack) : 2);
    free(prompt); free(cmd); free(kind); free(ack);
    free(req); close(cfd); return;
  }

  if (is_get && strncmp(path, "/peer/v1/jobs/", 14) == 0) {
    if (!require_peer_auth(cfd, req, 0)) { free(req); close(cfd); return; }
    const char *id = path + 14;
    /* job ids are digits only (time+pid) */
    if (!id[0] || strchr(id, '/') || strstr(id, "..")) {
      http_json(cfd, 400, "{\"error\":\"bad id\"}");
      free(req); close(cfd); return;
    }
    for (const char *p = id; *p; p++) {
      if (!isdigit((unsigned char)*p)) {
        http_json(cfd, 400, "{\"error\":\"bad id\"}");
        free(req); close(cfd); return;
      }
    }
    char mpath[700];
    snprintf(mpath, sizeof mpath, "%s/jobs/%s.json", ng_workdir(), id);
    size_t blen = 0;
    char *body = ng_read_file(mpath, &blen);
    if (!body) {
      http_json(cfd, 404, "{\"error\":\"job not found\"}");
      free(req); close(cfd); return;
    }
    http_response(cfd, 200, "application/json", body, blen);
    free(body);
    free(req); close(cfd); return;
  }

  if (is_post && strcmp(path, "/peer/v1/prompt") == 0) {
    if (!require_peer_auth(cfd, req, 0)) { free(req); close(cfd); return; }

    {
      int need_browser = agent && ng_agent_needs_browser_session(agent);
      if (need_browser && (!session || !ng_session_valid(session))) {
        http_json(cfd, 401, "{\"error\":\"Grok session not active; use --offline for llama or open /activate\",\"need_login\":true}");
        free(req); close(cfd); return;
      }
    }
    char *body = strstr(req, "\r\n\r\n");
    body = body ? body + 4 : "";
    char *prompt = ng_json_get_string(body, "prompt");
    if (!prompt) prompt = ng_json_get_string(body, "message");
    if (!prompt) prompt = ng_json_get_string(body, "q");
    if (!prompt) {
      http_json(cfd, 400, "{\"error\":\"missing prompt\"}");
      free(req); close(cfd); return;
    }
    ng_log("peer: prompt from remote session: %.200s", prompt);
    char *reply = ng_agent_run(agent, prompt);
    char *esc = ng_json_escape(reply ? reply : "");
    char *jb = NULL;
    asprintf(&jb, "{\"ok\":true,\"reply\":\"%s\",\"source\":\"nanobot-peer\"}", esc ? esc : "");
    http_response(cfd, 200, "application/json", jb ? jb : "{}", jb ? strlen(jb) : 2);
    free(prompt); free(reply); free(esc); free(jb);
    free(req); close(cfd); return;
  }

  if (is_post && strcmp(path, "/peer/v1/shell") == 0) {
    if (!require_peer_auth(cfd, req, 0)) { free(req); close(cfd); return; }

    char *body = strstr(req, "\r\n\r\n");
    body = body ? body + 4 : "";
    char *cmd = ng_json_get_string(body, "command");
    if (!cmd) cmd = ng_json_get_string(body, "cmd");
    if (!cmd) {
      http_json(cfd, 400, "{\"error\":\"missing command\"}");
      free(req); close(cfd); return;
    }
    ng_log("peer: shell from remote session: %.80s", cmd);
    ng_cmd_result cr = ng_run_command(cmd, agent->timeout_sec > 0 ? agent->timeout_sec : 60);
    char *esc = ng_json_escape(cr.output ? cr.output : "");
    char *jb = NULL;
    int need_appr = (cr.exit_code == 425);
    char *aid = NULL;
    if (need_appr && cr.output) {
      const char *p = strstr(cr.output, "approval_id=");
      if (p) {
        p += 12;
        size_t n = 0;
        while (p[n] && p[n] != '\n' && p[n] != '\r' && n < 32) n++;
        aid = malloc(n+1); if(aid){memcpy(aid,p,n);aid[n]=0;}
      }
    }
    if (need_appr) {
      asprintf(&jb,
        "{\"ok\":false,\"exit\":425,\"need_approval\":true,\"approval_id\":\"%s\","
        "\"output\":\"%s\",\"source\":\"nanobot-peer\"}",
        aid ? aid : "", esc ? esc : "");
    } else {
      asprintf(&jb,
        "{\"ok\":%s,\"exit\":%d,\"output\":\"%s\",\"source\":\"nanobot-peer\"}",
        cr.exit_code == 0 ? "true" : "false", cr.exit_code, esc ? esc : "");
    }
    http_response(cfd, 200, "application/json", jb ? jb : "{}", jb ? strlen(jb) : 2);
    free(cmd); free(esc); free(jb); free(aid);
    ng_cmd_result_free(&cr);
    free(req); close(cfd); return;
  }

  /* Shell security: gate password + pending approvals */
  if (is_get && (strcmp(path, "/api/shell/approvals") == 0
                 || strcmp(path, "/peer/v1/shell/approvals") == 0)) {
    if (!require_peer_auth(cfd, req, 1)) { free(req); close(cfd); return; }
    char *list = ng_shell_approval_list_json();
    char *out = NULL;
    asprintf(&out, "{\"ok\":true,\"gate_configured\":%s,\"pending\":%s}",
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
      http_json(cfd, rc == 0 ? 200 : 400,
                rc == 0 ? "{\"ok\":true,\"gate\":\"set\"}"
                        : "{\"ok\":false,\"error\":\"set failed (min 4 chars)\"}");
    } else if (action && !strcmp(action, "verify") && pw) {
      int ok = ng_shell_gate_verify_password(pw);
      http_json(cfd, 200, ok ? "{\"ok\":true,\"valid\":true}" : "{\"ok\":true,\"valid\":false}");
    } else {
      http_json(cfd, 400, "{\"error\":\"need action set|verify + password\"}");
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
      http_json(cfd, 200, "{\"ok\":true,\"status\":\"rejected\"}");
      free(id); free(pw); free(action);
      free(req); close(cfd); return;
    }
    char *cmd = NULL;
    int rc = ng_shell_approval_approve(id, pw, &cmd);
    if (rc == 0 && cmd) {
      ng_cmd_result cr = ng_run_command_approved(cmd,
          agent && agent->timeout_sec > 0 ? agent->timeout_sec : 60);
      char *esc = ng_json_escape(cr.output ? cr.output : "");
      char *jb = NULL;
      asprintf(&jb,
        "{\"ok\":%s,\"exit\":%d,\"output\":\"%s\",\"approved\":true,\"command\":\"%s\"}",
        cr.exit_code == 0 ? "true" : "false", cr.exit_code,
        esc ? esc : "", cmd);
      http_response(cfd, 200, "application/json", jb ? jb : "{}", jb ? strlen(jb) : 2);
      free(esc); free(jb);
      ng_cmd_result_free(&cr);
    } else {
      const char *err = rc == -3 ? "gate password not configured"
                       : rc == -4 ? "invalid password"
                       : rc == -2 ? "not pending"
                       : "approval failed";
      char *jb = NULL;
      asprintf(&jb, "{\"ok\":false,\"error\":\"%s\"}", err);
      http_response(cfd, 403, "application/json", jb ? jb : "{}", jb ? strlen(jb) : 2);
      free(jb);
    }
    free(id); free(pw); free(action); free(cmd);
    free(req); close(cfd); return;
  }


  http_text(cfd, 404, "not found\n");
  free(req);
  close(cfd);
}

static int g_live_children = 0;

static void reap_children(void) {
  int st;
  pid_t p;
  while ((p = waitpid(-1, &st, WNOHANG)) > 0) {
    if (g_live_children > 0) g_live_children--;
  }
}

static void on_sigchld(int s) {
  (void)s;
  /* actual waitpid in serve loop to keep handler async-signal-safe */
}

int ng_http_serve(ng_http_cfg *cfg) {
  signal(SIGPIPE, SIG_IGN);
  signal(SIGCHLD, on_sigchld);
  int sfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sfd < 0) { ng_log("socket: %s", strerror(errno)); return -1; }
  int on = 1;
  setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons((uint16_t)cfg->port);
  if (bind(sfd, (struct sockaddr *)&addr, sizeof addr) != 0) {
    ng_log("bind :%d: %s", cfg->port, strerror(errno));
    close(sfd);
    return -1;
  }
  if (listen(sfd, 64) != 0) {
    ng_log("listen: %s", strerror(errno));
    close(sfd);
    return -1;
  }
  ng_log("http listening on 0.0.0.0:%d (concurrent fork, max %d)",
         cfg->port, ng_http_max_children());

  g_live_children = 0;
  while (!cfg->stop) {
    reap_children();
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
      if (errno == EINTR) { reap_children(); continue; }
      ng_log("accept: %s", strerror(errno));
      break;
    }

    /* Refresh tokens + pending device login from secure files (fork-safe). */
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
  }
  for (;;) {
    int st = 0;
    if (waitpid(-1, &st, WNOHANG) <= 0) break;
  }
  close(sfd);
  return 0;
}
