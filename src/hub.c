#include "hub_local.h"
#include "util.h"
#include <nanobot/crypto.h>
#include <nanobot/os.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <ctype.h>

static void hub_dir(char *out, size_t n) {
  snprintf(out, n, "%s/hub", ng_workdir());
  mkdir(out, 0755);
}

static void events_path(char *out, size_t n) {
  snprintf(out, n, "%s/hub/events.jsonl", ng_workdir());
}

int ng_hub_event_obj(const char *json_object) {
  if (!json_object || !json_object[0]) return -1;
  char dir[700], path[720];
  hub_dir(dir, sizeof dir);
  events_path(path, sizeof path);
  FILE *f = fopen(path, "a");
  if (!f) return -1;
  fprintf(f, "%s\n", json_object);
  fclose(f);
  return 0;
}

int ng_hub_event(const char *type, const char *k1, const char *v1,
                 const char *k2, const char *v2) {
  char *e1 = ng_json_escape(v1 ? v1 : "");
  char *e2 = ng_json_escape(v2 ? v2 : "");
  char *e0 = ng_json_escape(type ? type : "event");
  time_t t = time(NULL);
  char *line = NULL;
  if (k1 && k1[0] && k2 && k2[0])
    asprintf(&line,
      "{\"ts\":%ld,\"type\":\"%s\",\"%s\":\"%s\",\"%s\":\"%s\"}",
      (long)t, e0, k1, e1, k2, e2);
  else if (k1 && k1[0])
    asprintf(&line,
      "{\"ts\":%ld,\"type\":\"%s\",\"%s\":\"%s\"}",
      (long)t, e0, k1, e1);
  else
    asprintf(&line, "{\"ts\":%ld,\"type\":\"%s\"}", (long)t, e0);
  free(e0); free(e1); free(e2);
  int rc = ng_hub_event_obj(line ? line : "{}");
  free(line);
  return rc;
}

static int out_token_ok(const char *req, const char *expect) {
  if (!expect || !expect[0]) return 0;
  const char *h = strstr(req, "X-Nanobot-Out-Token:");
  if (!h) h = strstr(req, "x-nanobot-out-token:");
  if (!h) h = strstr(req, "X-Nanobot-Peer-Token:");
  if (!h) h = strstr(req, "x-nanobot-peer-token:");
  if (!h) return 0;
  h = strchr(h, ':');
  if (!h) return 0;
  h++;
  while (*h == ' ' || *h == '\t') h++;
  size_t nl = strcspn(h, "\r\n");
  return nb_ct_eq(h, nl, expect, strlen(expect));
}

static int client_loopback(int cfd) {
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

static void out_write(int fd, const char *s) {
  if (!s) return;
  size_t n = strlen(s);
  while (n) {
    ssize_t w = write(fd, s, n);
    if (w <= 0) break;
    s += w;
    n -= (size_t)w;
  }
}

static void handle_out_client(int cfd, const char *expect_token) {
  char req[8192];
  size_t n = 0;
  while (n + 1 < sizeof req) {
    ssize_t r = read(cfd, req + n, sizeof req - 1 - n);
    if (r <= 0) break;
    n += (size_t)r;
    req[n] = 0;
    if (strstr(req, "\r\n\r\n")) break;
  }
  req[n] = 0;

  char method[16] = "", path[512] = "";
  sscanf(req, "%15s %511s", method, path);
  int is_get = !strcmp(method, "GET");

  /* strip query */
  char *q = strchr(path, '?');
  long since = 0;
  if (q) {
    *q = 0;
    const char *qs = q + 1;
    const char *s = strstr(qs, "since=");
    if (s) since = atol(s + 6);
  }

  /* Fail-closed: non-loopback never gets free READ. No expect token → 503
   * (misconfig); wrong/missing client token → 401. Loopback may proceed. */
  {
    int loopback = client_loopback(cfd);
    int has_expect = expect_token && expect_token[0];
    if (!loopback) {
      if (!has_expect) {
        const char *body =
          "HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\n"
          "Connection: close\r\n\r\n"
          "{\"error\":\"hub OUT token not configured; refusing non-loopback "
          "(set peer_token or NANOBOT_OUT_TOKEN)\"}";
        out_write(cfd, body);
        close(cfd);
        return;
      }
      if (!out_token_ok(req, expect_token)) {
        const char *body =
          "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\n"
          "Connection: close\r\n\r\n"
          "{\"error\":\"need X-Nanobot-Out-Token or peer token (READ)\"}";
        out_write(cfd, body);
        close(cfd);
        return;
      }
    }
  }

  if (is_get && (!strcmp(path, "/hub/v1/health") || !strcmp(path, "/"))) {
    out_write(cfd,
      "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
      "Connection: close\r\n\r\n"
      "{\"ok\":true,\"role\":\"out\",\"service\":\"nanobot-hub\"}");
    close(cfd);
    return;
  }

  if (is_get && !strcmp(path, "/hub/v1/events")) {
    out_write(cfd,
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/event-stream\r\n"
      "Cache-Control: no-cache\r\n"
      "Connection: keep-alive\r\n\r\n");
    out_write(cfd, ": nanobot hub out\n\n");

    char path_ev[720];
    events_path(path_ev, sizeof path_ev);
    long offset = since;
    int ticks = 0;
    while (ticks < 3600) { /* ~1h max stream */
      FILE *f = fopen(path_ev, "r");
      if (f) {
        if (offset > 0) fseek(f, offset, SEEK_SET);
        char line[4096];
        while (fgets(line, sizeof line, f)) {
          size_t L = strlen(line);
          while (L && (line[L-1] == '\n' || line[L-1] == '\r')) line[--L] = 0;
          if (!L) continue;
          char frame[4300];
          snprintf(frame, sizeof frame, "data: %s\n\n", line);
          out_write(cfd, frame);
        }
        offset = ftell(f);
        fclose(f);
      }
      /* heartbeat */
      out_write(cfd, ": ping\n\n");
      sleep(1);
      ticks++;
      /* detect closed peer: try MSG_PEEK optional skip */
    }
    close(cfd);
    return;
  }

  out_write(cfd,
    "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\n"
    "Connection: close\r\n\r\n"
    "{\"error\":\"out routes: GET /hub/v1/health | /hub/v1/events\"}");
  close(cfd);
}

static void on_chld(int s) { (void)s; while (waitpid(-1, NULL, WNOHANG) > 0) {} }

int ng_hub_out_serve(ng_hub_out_cfg *cfg) {
  if (!cfg || cfg->port_out <= 0) return -1;
  signal(SIGCHLD, on_chld);
  signal(SIGPIPE, SIG_IGN);

  char *token = NULL;
  if (cfg->out_token && cfg->out_token[0])
    token = strdup(cfg->out_token);
  else {
    char p[700];
    snprintf(p, sizeof p, "%s/peer_token", ng_workdir());
    token = ng_slurp_env_file(p, "token");
    if (!token) {
      size_t bl = 0;
      char *raw = ng_read_file(p, &bl);
      if (raw && raw[0]) {
        char *line = raw;
        while (*line == ' ' || *line == '\n') line++;
        size_t n = strcspn(line, "\r\n");
        token = strndup(line, n);
      }
      free(raw);
    }
  }

  int sfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sfd < 0) { free(token); return -1; }
  int on = 1;
  setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons((uint16_t)cfg->port_out);
  if (bind(sfd, (struct sockaddr *)&addr, sizeof addr) != 0) {
    ng_log("hub OUT bind :%d: %s", cfg->port_out, strerror(errno));
    close(sfd);
    free(token);
    return -1;
  }
  listen(sfd, 32);
  ng_log("hub OUT listening on 0.0.0.0:%d (READ / events)", cfg->port_out);

  while (1) {
    if (cfg->stop && *cfg->stop) break;
    struct sockaddr_in cli;
    socklen_t cl = sizeof cli;
    int cfd = accept(sfd, (struct sockaddr *)&cli, &cl);
    if (cfd < 0) {
      if (errno == EINTR) continue;
      break;
    }
    pid_t pid = fork();
    if (pid == 0) {
      close(sfd);
      handle_out_client(cfd, token);
      _exit(0);
    }
    close(cfd);
  }
  close(sfd);
  free(token);
  return 0;
}
