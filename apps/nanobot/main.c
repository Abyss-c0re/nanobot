#include "agent.h"
#include "auth.h"
#include "http.h"
#include "mcp.h"
#include "memory.h"
#include "hub_local.h"
#include "util.h"
#include <nanobot/crypto.h>
#include <nanobot/os.h>
#include <sys/wait.h>
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

/* CMake sets NANOBOT_ENABLE_*; default all-on for ad-hoc compiles */
#ifndef NANOBOT_ENABLE_MCP
#define NANOBOT_ENABLE_MCP 1
#endif
#ifndef NANOBOT_ENABLE_AUTH
#define NANOBOT_ENABLE_AUTH 1
#endif
#ifndef NANOBOT_ENABLE_PEER
#define NANOBOT_ENABLE_PEER 1
#endif
#ifndef NANOBOT_ENABLE_HUB
#define NANOBOT_ENABLE_HUB 1
#endif
#ifndef NANOBOT_ENABLE_SHELL
#define NANOBOT_ENABLE_SHELL 1
#endif
#ifndef NANOBOT_ENABLE_PROVIDERS
#define NANOBOT_ENABLE_PROVIDERS 1
#endif

static volatile int g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

/* CLI real-time token printer */
static void cli_stream_delta(void *ud, const char *chunk, size_t n) {
  (void)ud;
  if (!chunk || !n) return;
  fwrite(chunk, 1, n, stdout);
  fflush(stdout);
}

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
  fprintf(stderr, "  nanobot %s — CLI stream + hub IN/OUT + MCP\n", NG_VERSION);
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
  fprintf(stderr, "  Auth: browser device-code, or --offline for llama.cpp.\n\n");

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
    "nanobot %s — standalone agent host (no external product required)\n\n"
    "  CLI:   -p prompt | --no-stream | @! shell\n"
    "  Peer:  --port N  (HTTP /peer /api; self-auth via /activate or --login)\n"
    "  Hub:   --hub | --port-out M\n"
    "  MCP:   --mcp (stdio)\n"
    "  Auth:  --login  device-code (sealed under peer_token KDF)\n"
    "  Local: --offline | --base-url URL | --model NAME\n"
    "  Static: --www DIR (optional)\n\n"
    "Build features: MCP=%d AUTH=%d PEER=%d HUB=%d SHELL=%d PROVIDERS=%d\n"
    "  cmake -DNANOBOT_ENABLE_MCP=OFF … etc.\n\n"
    "Usage:\n"
    "  %s --port 8787\n"
    "  %s --hub\n"
    "  %s -p 'hello'\n"
    "  %s --offline -p '…'\n"
    "  %s --login | --mcp | --www DIR | --home DIR\n\n"
    "Env: NANOBOT_HOME  NANOBOT_PEER_TOKEN  NANOBOT_OUT_TOKEN\n"
    "     NANOBOT_MASTER_KEY=path (optional; never hard-required)\n"
    "Auth is self-contained: peer_token auto-created under NANOBOT_HOME.\n",
    NG_VERSION,
    NANOBOT_ENABLE_MCP, NANOBOT_ENABLE_AUTH, NANOBOT_ENABLE_PEER,
    NANOBOT_ENABLE_HUB, NANOBOT_ENABLE_SHELL, NANOBOT_ENABLE_PROVIDERS,
    argv0, argv0, argv0, argv0, argv0);
}

int main(int argc, char **argv) {
  int port = NG_DEFAULT_PORT;
  int mode_mcp = 0;
  int force_login = 0;
  int force_offline = 0;
  int want_stream = 1;
  int hub_mode = 0;
  int port_out = 0;
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
    } else if ((strcmp(argv[i], "--port") == 0 || strcmp(argv[i], "--port-in") == 0) &&
               i + 1 < argc) {
      port = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--port-out") == 0 && i + 1 < argc) {
      port_out = atoi(argv[++i]);
      hub_mode = 1;
    } else if (strcmp(argv[i], "--hub") == 0) {
      hub_mode = 1;
      if (port_out <= 0) port_out = 0; /* set after port known */
    } else if (strcmp(argv[i], "--stream") == 0) {
      want_stream = 1;
    } else if (strcmp(argv[i], "--no-stream") == 0) {
      want_stream = 0;
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
#if !NANOBOT_ENABLE_MCP
    fprintf(stderr, "nanobot: built without MCP (NANOBOT_ENABLE_MCP=0)\n");
    ng_session_free(&session);
    ng_agent_cfg_free(&agent);
    return 2;
#else
    if (need_browser) {
#if !NANOBOT_ENABLE_AUTH
      fprintf(stderr, "nanobot: cloud backend needs AUTH; rebuild with NANOBOT_ENABLE_AUTH=ON\n"
                      "  or use --offline / --base-url\n");
      ng_session_free(&session);
      ng_agent_cfg_free(&agent);
      return 2;
#else
      if (!ng_session_valid(&session)) {
        fprintf(stderr, "nanobot --mcp (cloud backend) needs a session.\n"
                        "Run: nanobot --login   or use --offline / --base-url\n");
        if (ng_session_login_blocking(&session) != 0) {
          ng_session_free(&session);
          ng_agent_cfg_free(&agent);
          return 1;
        }
      } else {
        ng_session_ensure(&session);
      }
#endif
    }
    int rc = ng_mcp_stdio_run(&agent);
    ng_session_free(&session);
    ng_agent_cfg_free(&agent);
    return rc;
#endif
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
    if (is_shell || !want_stream) {
      char *reply = ng_agent_run(&agent, oneshot);
      puts(reply ? reply : "");
      free(reply);
    } else {
      char *reply = ng_agent_run_ex(&agent, oneshot, 1, cli_stream_delta, NULL);
      fputc('\n', stdout);
      free(reply);
    }
    ng_session_free(&session);
    ng_agent_cfg_free(&agent);
    return 0;
  }

  /* Auth is self-contained: device-code via --login or GET /activate (if PEER).
   * Standalone agent host; no external product required. */
  if (force_login && need_browser) {
#if !NANOBOT_ENABLE_AUTH
    fprintf(stderr, "  --login: AUTH disabled in this build\n");
#else
    ng_session_clear(&session);
    fprintf(stderr, "  --login: open /activate after server starts (or poll device flow)\n");
#endif
  } else if (need_browser && ng_session_valid(&session)) {
#if NANOBOT_ENABLE_AUTH
    if (ng_session_ensure(&session) != 0)
      fprintf(stderr, "  Session refresh failed; use /activate or --login\n");
#endif
  } else if (need_browser) {
    fprintf(stderr,
            "  No cloud session — peer up; activate via /activate or: nanobot --login\n"
            "  Or --offline / --base-url for local OpenAI-compatible (no browser)\n");
  } else {
    fprintf(stderr, "  Local/OpenAI-compatible backend — no browser session required\n");
  }

  /* Peer token: nanobot-owned secret under NANOBOT_HOME (create if missing).
   * Do not rotate/delete on deploy — only create when absent. */
  {
    char pt[640];
    snprintf(pt, sizeof pt, "%s/peer_token", home);
    if (access(pt, R_OK) != 0) {
      unsigned char raw[16];
      char tok[33];
      if (nb_random_bytes(raw, sizeof raw) != 0 ||
          nb_hex_encode(raw, sizeof raw, tok, sizeof tok) != 0) {
        fprintf(stderr, "  warn: failed to create peer token (CSPRNG)\n");
      } else {
        char line[64];
        int n = snprintf(line, sizeof line, "token=%s\n", tok);
        if (nb_write_secret_file(pt, line, (size_t)n) == 0) {
          fprintf(stderr, "  Peer token created: %s\n", pt);
          fprintf(stderr, "  Header: X-Nanobot-Peer-Token: %s\n", tok);
        }
        nb_secure_wipe(raw, sizeof raw);
        nb_secure_wipe(tok, sizeof tok);
      }
    } else {
      fprintf(stderr, "  Peer token file: %s (unchanged)\n", pt);
    }
  }

  /* Optional external master file (path via env only — no product hardcodes). */
  {
    int master_present = 0;
    const char *env_m = getenv("NANOBOT_MASTER_KEY");
    if (!env_m || !env_m[0]) env_m = getenv("NANOBOT_MASTER_KEY"); /* legacy env */
    if (env_m && env_m[0] && access(env_m, R_OK) == 0)
      master_present = 1;
    if (!master_present) {
      char alt[700];
      snprintf(alt, sizeof alt, "%s/master.key", home);
      if (access(alt, R_OK) == 0) master_present = 1;
    }
    if (master_present) {
      fprintf(stderr, "  optional master key present (NANOBOT_MASTER_KEY or $NANOBOT_HOME/)\n");
    }
    const char *req_la = getenv("NANOBOT_REQUIRE_MASTER");
    if (!req_la || !req_la[0]) req_la = getenv("NANOBOT_REQUIRE_MASTER");
    if (req_la && (!strcmp(req_la, "1") || !strcasecmp(req_la, "true") ||
                   !strcasecmp(req_la, "yes"))) {
      if (!master_present) {
        fprintf(stderr,
                "nanobot: NANOBOT_REQUIRE_MASTER=1 but master key not found\n"
                "  set NANOBOT_MASTER_KEY=/path/to/master.key\n"
                "  or place $NANOBOT_HOME/master.key\n");
        ng_session_free(&session);
        ng_agent_cfg_free(&agent);
        return 1;
      }
    }
  }

#if !NANOBOT_ENABLE_PEER
  fprintf(stderr, "nanobot: built without PEER HTTP (NANOBOT_ENABLE_PEER=0)\n");
  ng_session_free(&session);
  ng_agent_cfg_free(&agent);
  return 2;
#endif

  signal(SIGINT, on_sig);
  signal(SIGTERM, on_sig);
  print_banner(port, &session, www_root);

  if (hub_mode) {
#if !NANOBOT_ENABLE_HUB
    fprintf(stderr, "  warn: --hub ignored (NANOBOT_ENABLE_HUB=0)\n");
#else
    if (port_out <= 0) port_out = port + 1;
    fprintf(stderr, "  hub: IN(WRITE)=:%d  OUT(READ)=:%d  (docs/HUB.md)\n", port, port_out);
    pid_t out_pid = fork();
    if (out_pid == 0) {
      const char *ot = getenv("NANOBOT_OUT_TOKEN");
      ng_hub_out_cfg oc = {.port_out = port_out, .stop = &g_stop, .out_token = ot};
      ng_hub_out_serve(&oc);
      _exit(0);
    }
    if (out_pid > 0)
      ng_hub_event("hub.start", "role", "in", "out_port", "");
#endif
  }

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
  g_stop = 1;

  ng_session_free(&session);
  ng_agent_cfg_free(&agent);
  return 0;
}
