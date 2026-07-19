#include "http.h"
#include "auth.h"
#include "shell.h"
#include "util.h"
#include "www_index.h"
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
    http_response(cfd, 200, "text/plain", "", 0);
    free(req); close(cfd); return;
  }

  /* advance pending device login on any request */
  if (session && session->login_pending) {
    int pr = ng_session_poll_login(session);
    if (pr == 1) ng_log("auth: browser approved session");
  }

  if (is_get && (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)) {
    http_response(cfd, 200, "text/html; charset=utf-8", WWW_INDEX_HTML, strlen(WWW_INDEX_HTML));
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
      http_response(cfd, 503, "text/plain", "login not ready\n", 16);
    }
    free(req); close(cfd); return;
  }

  if (is_get && (strcmp(path, "/api/auth") == 0 || strcmp(path, "/api/status") == 0)) {
    int signed_in = session && ng_session_valid(session);
    char *vu = NULL, *vuc = NULL, *uc = NULL;
    if (session) {
      if (session->verification_uri) vu = ng_json_escape(session->verification_uri);
      if (session->verification_uri_complete) vuc = ng_json_escape(session->verification_uri_complete);
      if (session->user_code) uc = ng_json_escape(session->user_code);
    }
    char body[2048];
    int n = snprintf(body, sizeof body,
      "{\"ok\":true,\"version\":\"%s\",\"model\":\"%s\",\"signed_in\":%s,"
      "\"login_pending\":%s,\"user_code\":\"%s\","
      "\"verification_uri\":\"%s\",\"verification_uri_complete\":\"%s\","
      "\"workdir\":\"%s\",\"auth\":\"browser_device_code\"}",
      NG_VERSION,
      agent->model ? agent->model : "",
      signed_in ? "true" : "false",
      (session && session->login_pending) ? "true" : "false",
      uc ? uc : "",
      vu ? vu : "",
      vuc ? vuc : "",
      ng_workdir());
    http_response(cfd, 200, "application/json", body, (size_t)n);
    free(vu); free(vuc); free(uc);
    free(req); close(cfd); return;
  }

  if (is_post && strcmp(path, "/api/auth/start") == 0) {
    if (!session) {
      http_response(cfd, 500, "application/json", "{\"error\":\"no session\"}", 21);
      free(req); close(cfd); return;
    }
    if (ng_session_valid(session)) {
      http_response(cfd, 200, "application/json", "{\"ok\":true,\"signed_in\":true}", 28);
      free(req); close(cfd); return;
    }
    if (!session->login_pending) {
      if (ng_session_start_device_login(session) != 0) {
        http_response(cfd, 500, "application/json", "{\"error\":\"device login failed\"}", 30);
        free(req); close(cfd); return;
      }
    }
    char *vuc = session->verification_uri_complete
      ? ng_json_escape(session->verification_uri_complete)
      : ng_json_escape(session->verification_uri ? session->verification_uri : "");
    char *uc = ng_json_escape(session->user_code ? session->user_code : "");
    char *body = NULL;
    asprintf(&body,
      "{\"ok\":true,\"signed_in\":false,\"login_pending\":true,"
      "\"user_code\":\"%s\",\"verification_uri_complete\":\"%s\"}",
      uc ? uc : "", vuc ? vuc : "");
    http_response(cfd, 200, "application/json", body ? body : "{}", body ? strlen(body) : 2);
    free(body); free(vuc); free(uc);
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

  if (is_post && strcmp(path, "/api/chat") == 0) {
    char *body = strstr(req, "\r\n\r\n");
    body = body ? body + 4 : "";
    char *prompt = ng_json_get_string(body, "prompt");
    if (!prompt) prompt = ng_json_get_string(body, "q");
    if (!prompt) prompt = ng_json_get_string(body, "message");
    if (!prompt) {
      http_response(cfd, 400, "application/json", "{\"error\":\"missing prompt\"}", 27);
      free(req); close(cfd); return;
    }
    /* @! shell is offline — no Grok session required */
    int offline = (prompt[0] == '@' && prompt[1] == '!');
    if (!offline && (!session || !ng_session_valid(session))) {
      if (session && session->login_pending) ng_session_poll_login(session);
      if (!session || !ng_session_valid(session)) {
        free(prompt);
        http_response(cfd, 401, "application/json",
          "{\"error\":\"Open activation link, or use @! <cmd> offline\",\"need_login\":true}", 76);
        free(req); close(cfd); return;
      }
    }
    char *reply = ng_agent_run(agent, prompt);
    char *esc = ng_json_escape(reply ? reply : "");
    char *jb = NULL;
    asprintf(&jb, "{\"reply\":\"%s\"}", esc ? esc : "");
    http_response(cfd, 200, "application/json", jb ? jb : "{}", jb ? strlen(jb) : 2);
    free(prompt); free(reply); free(esc); free(jb);
    free(req); close(cfd); return;
  }


  /* ---- Peer bus for other Grok sessions (lab) ---- */
  if (is_get && strcmp(path, "/peer/v1/health") == 0) {
    char body[256];
    int n = snprintf(body, sizeof body,
      "{\"ok\":true,\"service\":\"nanobot-peer\",\"version\":\"%s\","
      "\"role\":\"robot-session-bus\"}", NG_VERSION);
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

  if (is_post && strcmp(path, "/peer/v1/prompt") == 0) {
    /* Optional peer token: NANOBOT_PEER_TOKEN / home/peer_token */
    char token_path[640];
    snprintf(token_path, sizeof token_path, "%s/peer_token", ng_workdir());
    char *need = ng_slurp_env_file(token_path, "token");
    if (!need) need = ng_getenv_dup("NANOBOT_PEER_TOKEN");
    if (need && need[0]) {
      const char *h = strstr(req, "X-Nanobot-Peer-Token:");
      if (!h) h = strstr(req, "x-nanobot-peer-token:");
      int ok = 0;
      if (h) {
        h = strchr(h, ':');
        if (h) {
          h++;
          while (*h == ' ' || *h == '\t') h++;
          size_t nl = strcspn(h, "\r\n");
          if (nl == strlen(need) && strncmp(h, need, nl) == 0) ok = 1;
        }
      }
      /* also allow token in JSON body */
      char *body0 = strstr(req, "\r\n\r\n");
      body0 = body0 ? body0 + 4 : "";
      char *bt = ng_json_get_string(body0, "peer_token");
      if (bt && strcmp(bt, need) == 0) ok = 1;
      free(bt);
      if (!ok) {
        free(need);
        http_response(cfd, 401, "application/json",
          "{\"error\":\"invalid or missing peer token\",\"need_peer_token\":true}", 62);
        free(req); close(cfd); return;
      }
    }
    free(need);

    if (!session || !ng_session_valid(session)) {
      http_response(cfd, 401, "application/json",
        "{\"error\":\"robot Grok session not active; open activation link on host browser\",\"need_login\":true}", 96);
      free(req); close(cfd); return;
    }
    char *body = strstr(req, "\r\n\r\n");
    body = body ? body + 4 : "";
    char *prompt = ng_json_get_string(body, "prompt");
    if (!prompt) prompt = ng_json_get_string(body, "message");
    if (!prompt) prompt = ng_json_get_string(body, "q");
    if (!prompt) {
      http_response(cfd, 400, "application/json", "{\"error\":\"missing prompt\"}", 27);
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
    char token_path[640];
    snprintf(token_path, sizeof token_path, "%s/peer_token", ng_workdir());
    char *need = ng_slurp_env_file(token_path, "token");
    if (!need) need = ng_getenv_dup("NANOBOT_PEER_TOKEN");
    if (need && need[0]) {
      const char *h = strstr(req, "X-Nanobot-Peer-Token:");
      if (!h) h = strstr(req, "x-nanobot-peer-token:");
      int ok = 0;
      if (h) {
        h = strchr(h, ':');
        if (h) {
          h++;
          while (*h == ' ' || *h == '\t') h++;
          size_t nl = strcspn(h, "\r\n");
          if (nl == strlen(need) && strncmp(h, need, nl) == 0) ok = 1;
        }
      }
      char *body0 = strstr(req, "\r\n\r\n");
      body0 = body0 ? body0 + 4 : "";
      char *bt = ng_json_get_string(body0, "peer_token");
      if (bt && strcmp(bt, need) == 0) ok = 1;
      free(bt);
      if (!ok) {
        free(need);
        http_response(cfd, 401, "application/json",
          "{\"error\":\"invalid or missing peer token\",\"need_peer_token\":true}", 62);
        free(req); close(cfd); return;
      }
    }
    free(need);

    char *body = strstr(req, "\r\n\r\n");
    body = body ? body + 4 : "";
    char *cmd = ng_json_get_string(body, "command");
    if (!cmd) cmd = ng_json_get_string(body, "cmd");
    if (!cmd) {
      http_response(cfd, 400, "application/json", "{\"error\":\"missing command\"}", 28);
      free(req); close(cfd); return;
    }
    ng_log("peer: shell from remote session: %.200s", cmd);
    ng_cmd_result cr = ng_run_command(cmd, agent->timeout_sec > 0 ? agent->timeout_sec : 60);
    char *esc = ng_json_escape(cr.output ? cr.output : "");
    char *jb = NULL;
    asprintf(&jb,
      "{\"ok\":%s,\"exit\":%d,\"output\":\"%s\",\"source\":\"nanobot-peer\"}",
      cr.exit_code == 0 ? "true" : "false", cr.exit_code, esc ? esc : "");
    http_response(cfd, 200, "application/json", jb ? jb : "{}", jb ? strlen(jb) : 2);
    free(cmd); free(esc); free(jb);
    ng_cmd_result_free(&cr);
    free(req); close(cfd); return;
  }


  http_response(cfd, 404, "text/plain", "not found\n", 10);
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
         cfg->port, NG_HTTP_MAX_CHILDREN);

  g_live_children = 0;
  while (!cfg->stop) {
    reap_children();
    while (g_live_children >= NG_HTTP_MAX_CHILDREN) {
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

    /* Refresh session from disk so each child sees latest tokens / login. */
    if (cfg->session) {
      ng_session_load(cfg->session);
      if (cfg->session->login_pending)
        ng_session_poll_login(cfg->session);
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
