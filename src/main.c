/* Moved to apps/nanobot/main.c — do not compile from here. */
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

static void print_banner(int port, ng_session *sess, const char *www_root) {
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
  fprintf(stderr, "  nanobot %s — CLI + peer/JSON API + optional MCP\n", NG_VERSION);
  fprintf(stderr, "  ─────────────────────────────────────────────\n");
  fprintf(stderr, "  Peer / JSON API:\n");
  fprintf(stderr, "       GET  http://%s:%d/peer/v1/info\n", host, port);
  fprintf(stderr, "       POST http://%s:%d/peer/v1/prompt  {\"prompt\":\"...\"}\n", host, port);
  fprintf(stderr, "       POST http://%s:%d/peer/v1/shell   {\"command\":\"...\"}\n", host, port);
  fprintf(stderr, "       POST http://%s:%d/peer/v1/jobs    (async)\n", host, port);
  fprintf(stderr, "  MCP (optional): nanobot --mcp  |  scripts/peer_mcp_bridge.py\n");
  fprintf(stderr, "  One-shot: nanobot -p 'prompt'  |  @! shell without session\n");
  if (www_root && www_root[0]) {
    fprintf(stderr, "  Static files: http://%s:%d/  ← %s\n", host, port, www_root);
  }
  if (sess && sess->verification_uri_complete) {
    fprintf(stderr, "\n  Device login (browser):\n");
    fprintf(stderr, "       %s\n", sess->verification_uri_complete);
    if (sess->user_code)
      fprintf(stderr, "     code: %s\n", sess->user_code);
  } else if (sess && sess->verification_uri) {
    fprintf(stderr, "\n  Device login:\n");
    fprintf(stderr, "       %s\n", sess->verification_uri);
    if (sess->user_code)
      fprintf(stderr, "     code: %s\n", sess->user_code);
  }
  fprintf(stderr, "  ─────────────────────────────────────────────\n");
  fprintf(stderr, "  Auth: web device-code (/activate, --login) for cloud provider,\n        or --offline / --base-url for local OpenAI-compatible.\n\n");

  if (sess && sess->verification_uri_complete)
    printf("%s\n", sess->verification_uri_complete);
  else if (sess && sess->verification_uri)
    printf("%s\n", sess->verification_uri);
  else if (www_root && www_root[0])
    printf("http://%s:%d/\n", host, port);
  else
    printf("http://%s:%d/peer/v1/info\n", host, port);
  fflush(stdout);
}

static void usage(const char *argv0) {
  fprintf(stderr,
    "nanobot %s — CLI + peer/JSON API + optional MCP\n\n"
    "  CLI:     -p prompt, @! shell, peer/JSON on --port\n"
    "  MCP:     --mcp (stdio)\n"
    "  Static:  --www DIR  (optional static file root; operator-supplied)\n\n"
    "LLM backend: Grok device-code (optional) OR --offline llama.cpp / OpenAI-compatible.\n\n"
    "Usage:\n"
    "  %s                      # peer/API on :8787\n"
    "  %s --www DIR            # same + optional static files\n"
    "  %s --offline            # local OpenAI API\n"
    "  %s --llama              # same as --offline\n"
    "  %s --base-url URL       # e.g. http://127.0.0.1:8080/v1\n"
    "  %s --model NAME\n"
    "  %s --login              # force browser device login\n"
    "  %s --mcp                # MCP on stdio\n"
    "  %s -p 'prompt'          # one-shot\n"
    "  %s --port N  --home DIR\n\n"
    "Env: NANOBOT_BASE_URL NANOBOT_MODEL NANOBOT_HOME NANOBOT_WWW\n"
    "     NANOBOT_PEER_TOKEN   peer mutating routes\n",
    NG_VERSION, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0);
}

int main(int argc, char **argv) {
  int port = NG_DEFAULT_PORT;
  int mode_mcp = 0;
  int force_login = 0;
  int force_offline = 0;
  char *oneshot = NULL;
  const char *cli_base = NULL;
  const char *cli_model = NULL;
  const char *www_root = getenv("NANOBOT_WWW");
  static char home_buf[640];
  static char www_buf[640];
  const char *home = getenv("NANOBOT_HOME");
  if (!home || !home[0]) {
    const char *h = getenv("HOME");
    if (h && h[0]) {
      snprintf(home_buf, sizeof home_buf, "%s/.nanobot", h);
      home = home_buf;
    } else {
      home = "/tmp/nanobot";
    }
  }

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      usage(argv[0]); return 0;
    } else if (strcmp(argv[i], "--mcp") == 0 || strcmp(argv[i], "mcp") == 0) {
      mode_mcp = 1;
    } else if (strcmp(argv[i], "--login") == 0) {
      force_login = 1;
    } else if (strcmp(argv[i], "--offline") == 0 || strcmp(argv[i], "--llama") == 0) {
      force_offline = 1;
    } else if ((strcmp(argv[i], "--base-url") == 0 || strcmp(argv[i], "--base") == 0) && i + 1 < argc) {
      cli_base = argv[++i];
    } else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
      cli_model = argv[++i];
    } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      port = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--home") == 0 && i + 1 < argc) {
      home = argv[++i];
    } else if ((strcmp(argv[i], "--www") == 0 || strcmp(argv[i], "--ui") == 0) && i + 1 < argc) {
      www_root = argv[++i];
    } else if (strcmp(argv[i], "--www") == 0 || strcmp(argv[i], "--ui") == 0) {
      /* bare flag: $NANOBOT_HOME/www only */
      snprintf(www_buf, sizeof www_buf, "%s/www", home);
      if (access(www_buf, R_OK) == 0) www_root = www_buf;
      else {
        fprintf(stderr, "--www: no $NANOBOT_HOME/www (pass --www DIR or NANOBOT_WWW)\n");
        return 2;
      }
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

  /* Load persisted settings (survives reboot under NANOBOT_HOME/settings).
   * CLI / env win over file. */
  {
    char *ui = ng_settings_get("UI");
    char *sw = ng_settings_get("WWW");
    char *sp = ng_settings_get("PORT");
    if ((!www_root || !www_root[0]) && ui) {
      int want = 0;
      if (!strcasecmp(ui, "on") || !strcmp(ui, "1") || !strcasecmp(ui, "true") ||
          !strcasecmp(ui, "yes"))
        want = 1;
      if (want) {
        if (sw && sw[0]) {
          snprintf(www_buf, sizeof www_buf, "%s", sw);
          www_root = www_buf;
        } else {
          snprintf(www_buf, sizeof www_buf, "%s/www", home);
          if (access(www_buf, R_OK) == 0) www_root = www_buf;
        }
      }
    }
    if (sp && sp[0] && port == NG_DEFAULT_PORT) {
      int p = atoi(sp);
      if (p > 0 && p < 65536) port = p;
    }
    free(ui); free(sw); free(sp);
  }

  /* normalize www path: DIR with index.html, or DIR/www/index.html */
  if (www_root && www_root[0]) {
    char probe[700];
    snprintf(probe, sizeof probe, "%s/index.html", www_root);
    if (access(probe, R_OK) != 0) {
      snprintf(probe, sizeof probe, "%s/www/index.html", www_root);
      if (access(probe, R_OK) == 0) {
        snprintf(www_buf, sizeof www_buf, "%s/www", www_root);
        www_root = www_buf;
      } else {
        fprintf(stderr, "  warn: --www %s has no index.html (static off)\n", www_root);
        www_root = NULL;
      }
    }
  }

  /* Persist optional static root so restart keeps it */
  if (www_root && www_root[0]) {
    ng_settings_set("UI", "on");
    ng_settings_set("WWW", www_root);
  }
  char logpath[640];
  snprintf(logpath, sizeof logpath, "%s/nanobot.log", home);
  ng_log_init(logpath);
  ng_cli_version_init();
  ng_limits_init();
  ng_memory_init();
  fprintf(stderr, "  limits: lean=%s turns=%d children=%d out=%zu log=%zu\n",
          ng_is_lean() ? "yes" : "no", ng_max_turns(), ng_http_max_children(),
          ng_out_max(), ng_log_max());

  ng_session session;
  ng_session_init(&session);
  ng_session_load(&session);

  ng_agent_cfg agent;
  ng_agent_cfg_init(&agent);
  agent.session = &session;
  char envpath[640];
  snprintf(envpath, sizeof envpath, "%s/env", home);
  ng_agent_load_env(&agent, envpath);

  if (force_offline && !cli_base)
    ng_agent_set_local_backend(&agent, NG_DEFAULT_LOCAL_BASE,
                               cli_model ? cli_model : NG_DEFAULT_LOCAL_MODEL);
  if (cli_base)
    ng_agent_set_local_backend(&agent, cli_base, cli_model ? cli_model : agent.model);
  else if (cli_model) {
    free(agent.model);
    agent.model = strdup(cli_model);
  }

  int need_browser = ng_agent_needs_browser_session(&agent);
  fprintf(stderr, "  backend: %s  base=%s  model=%s\n",
          ng_agent_backend_kind(&agent),
          agent.base_url ? agent.base_url : "?",
          agent.model ? agent.model : "?");

  if (mode_mcp) {
    if (need_browser) {
      if (!ng_session_valid(&session)) {
        fprintf(stderr, "nanobot --mcp (cloud backend) needs a browser session.\n"
                        "Run: nanobot --login   or use --offline / --base-url for llama.cpp\n");
        if (ng_session_login_blocking(&session) != 0) {
          ng_session_free(&session);
          ng_agent_cfg_free(&agent);
          return 1;
        }
      } else {
        ng_session_ensure(&session);
      }
    }
    int rc = ng_mcp_stdio_run(&agent);
    ng_session_free(&session);
    ng_agent_cfg_free(&agent);
    return rc;
  }

  if (oneshot) {
    int is_shell = oneshot[0] == '@' && oneshot[1] == '!';
    if (need_browser && !is_shell) {
      if (!ng_session_valid(&session)) {
        fprintf(stderr, "No cloud session. Use nanobot --login, or --offline for llama.cpp,\n"
                        "or @! <cmd> for shell without a model.\n");
        ng_session_free(&session);
        ng_agent_cfg_free(&agent);
        return 1;
      }
      ng_session_ensure(&session);
    }
    char *reply = ng_agent_run(&agent, oneshot);
    puts(reply ? reply : "");
    free(reply);
    ng_session_free(&session);
    ng_agent_cfg_free(&agent);
    return 0;
  }

  /* Peer listener always up. Browser login only for optional cloud backend. */
  if (force_login && need_browser) {
    ng_session_clear(&session);
    fprintf(stderr, "  --login: open /activate after server starts\n");
  } else if (need_browser && ng_session_valid(&session)) {
    if (ng_session_ensure(&session) != 0)
      fprintf(stderr, "  Session refresh failed; use /activate when ready\n");
  } else if (need_browser) {
    fprintf(stderr,
            "  No cloud session yet — peer up; activate via /activate\n"
            "  Or restart with --offline for llama.cpp (no browser)\n");
  } else {
    fprintf(stderr, "  Local/OpenAI-compatible backend — no browser session required\n");
  }

  /* lab peer token (optional shared secret for other cloud sessions) */
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
  print_banner(port, &session, www_root);

  ng_http_cfg http = {
    .port = port,
    .agent = &agent,
    .session = &session,
    .stop = 0,
    .www_root = www_root,
  };
  while (!g_stop) {
    http.stop = g_stop;
    if (ng_http_serve(&http) != 0) break;
    break;
  }

  ng_session_free(&session);
  ng_agent_cfg_free(&agent);
  return 0;
}
