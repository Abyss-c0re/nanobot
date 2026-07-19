#include "agent.h"
#include "auth.h"
#include "http.h"
#include "mcp.h"
#include "memory.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <ifaddrs.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>

static volatile int g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

static void print_banner(int port, ng_session *sess) {
  char host[256] = "127.0.0.1";
  struct ifaddrs *ifaddr = NULL;
  if (getifaddrs(&ifaddr) == 0) {
    for (struct ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
      if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
      struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
      const char *ip = inet_ntoa(sin->sin_addr);
      if (ip && strncmp(ip, "127.", 4) != 0) {
        snprintf(host, sizeof host, "%s", ip);
        break;
      }
    }
    freeifaddrs(ifaddr);
  }

  fprintf(stderr, "\n");
  fprintf(stderr, "  nanobot %s\n", NG_VERSION);
  fprintf(stderr, "  ─────────────────────────────────────────────\n");
  fprintf(stderr, "  1) Open UI on BlackCube (then activate Grok in browser):\n");
  fprintf(stderr, "       http://%s:%d/\n", host, port);
  fprintf(stderr, "     or activate directly:\n");
  fprintf(stderr, "       http://%s:%d/activate\n", host, port);
  if (sess && sess->verification_uri_complete) {
    fprintf(stderr, "\n  2) Grok session activation (browser login):\n");
    fprintf(stderr, "       %s\n", sess->verification_uri_complete);
    if (sess->user_code)
      fprintf(stderr, "     code: %s\n", sess->user_code);
  } else if (sess && sess->verification_uri) {
    fprintf(stderr, "\n  2) Grok session activation:\n");
    fprintf(stderr, "       %s\n", sess->verification_uri);
    if (sess->user_code)
      fprintf(stderr, "     code: %s\n", sess->user_code);
  }
  fprintf(stderr, "\n  SSH tunnel (optional):\n");
  fprintf(stderr, "       ssh -N -L %d:127.0.0.1:%d USER@HOST\n", port, port);
  fprintf(stderr, "       http://127.0.0.1:%d/\n", port);
  fprintf(stderr, "  ─────────────────────────────────────────────\n");
  fprintf(stderr, "  Auth: browser device-code only (no API keys).\n");
  fprintf(stderr, "  Titan2: one line → Enter after session is active.\n");
  fprintf(stderr, "\n  Peer bus for other Grok sessions (lab private nets):\n");
  fprintf(stderr, "       GET  http://%s:%d/peer/v1/info\n", host, port);
  fprintf(stderr, "       POST http://%s:%d/peer/v1/prompt  {\"prompt\":\"...\"}\n", host, port);
  fprintf(stderr, "       POST http://%s:%d/peer/v1/shell   {\"command\":\"...\"}\n", host, port);
  fprintf(stderr, "  MCP bridge on BlackCube: nanobot/scripts/peer_mcp_bridge.py\n\n");

  /* Primary copy-paste line: prefer Grok activation URL, else UI */
  if (sess && sess->verification_uri_complete)
    printf("%s\n", sess->verification_uri_complete);
  else if (sess && sess->verification_uri)
    printf("%s\n", sess->verification_uri);
  else
    printf("http://%s:%d/\n", host, port);
  fflush(stdout);
}

static void usage(const char *argv0) {
  fprintf(stderr,
    "nanobot %s — tiny Grok agent (browser session + CLI tool + MCP)\n\n"
    "Usage:\n"
    "  %s                 # start UI; print browser activation link\n"
    "  %s --login         # force new browser device login, then serve\n"
    "  %s --mcp           # MCP server on stdio\n"
    "  %s -p 'prompt'     # one-shot (requires existing session)\n"
    "  %s --port N\n"
    "  %s --home DIR\n",
    NG_VERSION, argv0, argv0, argv0, argv0, argv0, argv0);
}

int main(int argc, char **argv) {
  int port = NG_DEFAULT_PORT;
  int mode_mcp = 0;
  int force_login = 0;
  char *oneshot = NULL;
  const char *home = getenv("NANOBOT_HOME");
  if (!home || !home[0]) {
    if (access("/mnt/data", W_OK) == 0) home = "/mnt/data/nanobot";
    else home = "/tmp/nanobot";
  }

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      usage(argv[0]); return 0;
    } else if (strcmp(argv[i], "--mcp") == 0 || strcmp(argv[i], "mcp") == 0) {
      mode_mcp = 1;
    } else if (strcmp(argv[i], "--login") == 0) {
      force_login = 1;
    } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      port = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--home") == 0 && i + 1 < argc) {
      home = argv[++i];
    } else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--prompt") == 0) && i + 1 < argc) {
      oneshot = argv[++i];
    } else if (strcmp(argv[i], "--version") == 0) {
      printf("nanobot %s\n", NG_VERSION); return 0;
    } else {
      fprintf(stderr, "unknown arg: %s\n", argv[i]);
      usage(argv[0]); return 2;
    }
  }

  ng_set_workdir(home);
  mkdir(home, 0755);
  char logpath[640];
  snprintf(logpath, sizeof logpath, "%s/nanobot.log", home);
  ng_log_init(logpath);
  ng_cli_version_init();
  ng_memory_init();

  ng_session session;
  ng_session_init(&session);
  ng_session_load(&session);

  ng_agent_cfg agent;
  ng_agent_cfg_init(&agent);
  agent.session = &session;
  char envpath[640];
  snprintf(envpath, sizeof envpath, "%s/env", home);
  ng_agent_load_env(&agent, envpath);

  if (mode_mcp) {
    if (!ng_session_valid(&session)) {
      fprintf(stderr, "nanobot --mcp needs a browser session.\n"
                      "Run: nanobot --login   (open the printed link on BlackCube)\n");
      if (ng_session_login_blocking(&session) != 0) {
        ng_session_free(&session);
        ng_agent_cfg_free(&agent);
        return 1;
      }
    } else {
      ng_session_ensure(&session);
    }
    int rc = ng_mcp_stdio_run(&agent);
    ng_session_free(&session);
    ng_agent_cfg_free(&agent);
    return rc;
  }

  if (oneshot) {
    if (!ng_session_valid(&session)) {
      fprintf(stderr, "No session. Run nanobot (open browser activation link) first.\n");
      ng_session_free(&session);
      ng_agent_cfg_free(&agent);
      return 1;
    }
    ng_session_ensure(&session);
    char *reply = ng_agent_run(&agent, oneshot);
    puts(reply ? reply : "");
    free(reply);
    ng_session_free(&session);
    ng_agent_cfg_free(&agent);
    return 0;
  }

  /* Peer listener: bind HTTP immediately. Browser login is on-demand
   * (UI /activate or POST /api/auth/start) so other Grok sessions can always
   * reach /peer/v1/info even without a session yet.
   */
  if (force_login) {
    ng_session_clear(&session);
    fprintf(stderr, "  --login: open /activate after server starts\n");
  } else if (ng_session_valid(&session)) {
    if (ng_session_ensure(&session) != 0)
      fprintf(stderr, "  Session refresh failed; use /activate when ready\n");
  } else {
    fprintf(stderr,
            "  No Grok session yet — peer bus up; activate via /activate\n");
  }

  /* lab peer token (optional shared secret for other Grok sessions) */
  {
    char pt[640];
    snprintf(pt, sizeof pt, "%s/peer_token", home);
    if (access(pt, R_OK) != 0) {
      srand((unsigned)time(NULL) ^ (unsigned)getpid());
      char tok[33];
      for (int i = 0; i < 32; i++) {
        const char *hex = "0123456789abcdef";
        tok[i] = hex[rand() % 16];
      }
      tok[32] = 0;
      FILE *f = fopen(pt, "w");
      if (f) {
        fprintf(f, "token=%s\n", tok);
        fclose(f);
        chmod(pt, 0600);
        fprintf(stderr, "  Peer token created: %s\n", pt);
        fprintf(stderr, "  Header: X-Nanobot-Peer-Token: %s\n", tok);
      }
    } else {
      fprintf(stderr, "  Peer token file: %s\n", pt);
    }
  }

  signal(SIGINT, on_sig);
  signal(SIGTERM, on_sig);
  print_banner(port, &session);

  ng_http_cfg http = { .port = port, .agent = &agent, .session = &session, .stop = 0 };
  while (!g_stop) {
    http.stop = g_stop;
    if (ng_http_serve(&http) != 0) break;
    break;
  }

  ng_session_free(&session);
  ng_agent_cfg_free(&agent);
  return 0;
}
