#include "agent.h"
#include "auth.h"
#include "http.h"
#include "mcp.h"
#include "memory.h"
#include "hub_local.h"
#include "util.h"
#include "shell.h"
#include "braincube_plugin.h"
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
    "nanobot %s — CLI agent (HTTP peer is opt-in only)\n\n"
    "  CLI (default product path — no open port):\n"
    "    -p prompt | --no-stream | @! shell\n"
    "    --auth-status | --auth-start [--force] | --auth-poll\n"
    "    --login          browser device-code (blocking; then exit)\n"
    "    --offline | --base-url URL | --model NAME | --models\n"
    "    --mcp            MCP on stdio (no HTTP)\n\n"
    "  Optional HTTP (MCP bridge / LAN share — explicit):\n"
    "    --port N         listen (default bind 127.0.0.1 only)\n"
    "    --lan            bind 0.0.0.0 (requires peer_token for mutate)\n"
    "    --hub | --port-out M | --www DIR\n\n"
    "Build features: MCP=%d AUTH=%d PEER=%d HUB=%d SHELL=%d PROVIDERS=%d\n\n"
    "Examples:\n"
    "  %s --login\n"
    "  %s --auth-status\n"
    "  %s -p 'hello'\n"
    "  %s --offline --base-url http://127.0.0.1:8080/v1 -p '…'\n"
    "  %s --mcp\n"
    "  %s --port 8787          # loopback API only\n"
    "  %s --port 8787 --lan    # intentional LAN (token-gated)\n\n"
    "Env: NANOBOT_HOME  NANOBOT_PEER_TOKEN  NANOBOT_OUT_TOKEN\n"
    "Auth sealed under peer_token KDF. No server unless --port.\n",
    NG_VERSION,
    NANOBOT_ENABLE_MCP, NANOBOT_ENABLE_AUTH, NANOBOT_ENABLE_PEER,
    NANOBOT_ENABLE_HUB, NANOBOT_ENABLE_SHELL, NANOBOT_ENABLE_PROVIDERS,
    argv0, argv0, argv0, argv0, argv0, argv0, argv0);
}

/* Machine-readable auth for UI / app CLI wrapper (stdout = one JSON line).
 * poll_state: none|pending|signed_in|expired|denied|error|throttled
 * Cross-device browser approval is normal — pending until token, not "cancelled". */
static void print_auth_json(const ng_session *s, const ng_agent_cfg *agent,
                            const char *poll_state, const char *poll_error) {
  int need_browser = agent && ng_agent_needs_browser_session(agent);
  int signed_in = need_browser ? (s && ng_session_valid(s)) : 1;
  int pending = s && s->login_pending;
  const char *backend = agent ? ng_agent_backend_kind(agent) : "unknown";
  const char *state = poll_state;
  if (!state || !state[0]) {
    if (signed_in) state = "signed_in";
    else if (pending) state = "pending";
    else state = "none";
  }
  char *vu = NULL, *vuc = NULL, *uc = NULL, *base_esc = NULL, *model_esc = NULL;
  char *err_esc = NULL;
  long deadline = (s && s->device_deadline) ? (long)s->device_deadline : 0;
  if (s && need_browser) {
    if (s->verification_uri) vu = ng_json_escape(s->verification_uri);
    if (s->verification_uri_complete) vuc = ng_json_escape(s->verification_uri_complete);
    if (s->user_code) uc = ng_json_escape(s->user_code);
  }
  if (agent && agent->base_url) base_esc = ng_json_escape(agent->base_url);
  if (agent && agent->model) model_esc = ng_json_escape(agent->model);
  if (poll_error && poll_error[0]) err_esc = ng_json_escape(poll_error);
  printf(
    "{\"ok\":true,\"version\":\"%s\",\"signed_in\":%s,\"login_pending\":%s,"
    "\"login_required\":%s,\"needs_browser\":%s,\"user_code\":\"%s\","
    "\"verification_uri\":\"%s\",\"verification_uri_complete\":\"%s\","
    "\"backend\":\"%s\",\"base_url\":\"%s\",\"model\":\"%s\","
    "\"auth\":\"%s\",\"workdir\":\"%s\",\"transport\":\"cli\","
    "\"poll_state\":\"%s\",\"error\":\"%s\",\"device_deadline\":%ld,"
    "\"cross_device_ok\":true}\n",
    NG_VERSION,
    signed_in ? "true" : "false",
    pending ? "true" : "false",
    (need_browser && !signed_in) ? "true" : "false",
    need_browser ? "true" : "false",
    uc ? uc : "",
    vu ? vu : "",
    vuc ? vuc : "",
    backend,
    base_esc ? base_esc : "",
    model_esc ? model_esc : "",
    need_browser ? "browser_device_code" : "local_openai_compatible",
    ng_workdir(),
    state,
    err_esc ? err_esc : "",
    deadline);
  free(vu); free(vuc); free(uc); free(base_esc); free(model_esc); free(err_esc);
  fflush(stdout);
}

int main(int argc, char **argv) {
  /* port < 0 means HTTP peer not requested (CLI-first product path). */
  int port = -1;
  int want_peer = 0;
  int bind_lan = 0;
  int mode_mcp = 0;
  int force_login = 0;
  int force_offline = 0;
  int list_models = 0;
  int auth_status = 0;
  int auth_start = 0;
  int auth_poll = 0;
  int auth_force = 0;
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
    } else if (strcmp(argv[i], "--auth-status") == 0) {
      auth_status = 1;
    } else if (strcmp(argv[i], "--auth-start") == 0) {
      auth_start = 1;
    } else if (strcmp(argv[i], "--auth-poll") == 0) {
      auth_poll = 1;
    } else if (strcmp(argv[i], "--force") == 0) {
      auth_force = 1;
    } else if (strcmp(argv[i], "--import-grok-cli") == 0) {
      /* handled after home set; mark via env for simplicity */
      setenv("NANOBOT_IMPORT_GROK_CLI", "1", 1);
    } else if (strcmp(argv[i], "--offline") == 0 || strcmp(argv[i], "--llama") == 0) {
      force_offline = 1;
    } else if ((strcmp(argv[i], "--base-url") == 0 || strcmp(argv[i], "--base") == 0) && i + 1 < argc) {
      cli_base = argv[++i];
    } else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
      cli_model = argv[++i];
    } else if (strcmp(argv[i], "--models") == 0 || strcmp(argv[i], "models") == 0) {
      list_models = 1;
    } else if ((strcmp(argv[i], "--port") == 0 || strcmp(argv[i], "--port-in") == 0) &&
               i + 1 < argc) {
      port = atoi(argv[++i]);
      want_peer = 1;
    } else if (strcmp(argv[i], "--peer") == 0) {
      want_peer = 1;
      if (port < 0) port = NG_DEFAULT_PORT;
    } else if (strcmp(argv[i], "--lan") == 0 || strcmp(argv[i], "--bind-any") == 0) {
      bind_lan = 1;
    } else if (strcmp(argv[i], "--port-out") == 0 && i + 1 < argc) {
      port_out = atoi(argv[++i]);
      hub_mode = 1;
      want_peer = 1;
    } else if (strcmp(argv[i], "--hub") == 0) {
      hub_mode = 1;
      want_peer = 1;
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
  if (want_peer && port < 0) port = NG_DEFAULT_PORT;
  if (hub_mode && !want_peer) {
    want_peer = 1;
    if (port < 0) port = NG_DEFAULT_PORT;
  }

  ng_set_workdir(home);
  mkdir(home, 0755);
  ng_shell_ensure_policy_files();

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
    /* Settings PORT only applies when peer was explicitly requested. */
    if (want_peer && sp && sp[0] && port == NG_DEFAULT_PORT) {
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
  /* BrainCube: parent continuous learn (fork workers only serve snapshots). */
  fprintf(stderr, "  braincube: available=%s\n", ng_bc_available() ? "yes" : "no");
  ng_bc_boot_parent();
  fprintf(stderr, "  braincube: continuous=%s\n", ng_bc_continuous() ? "on" : "off");

  ng_session session;
  ng_session_init(&session);
  ng_session_load(&session);
  /* If no sealed session yet, grab Grok Build CLI token when installed. */
#if NANOBOT_ENABLE_AUTH
  if (!force_offline && !ng_session_valid(&session)) {
    int imp = ng_session_try_import_grok_cli(&session);
    if (imp == 1)
      fprintf(stderr, "  auth: imported Grok Build CLI session from ~/.grok/auth.json\n");
    else if (imp == 0 && !ng_session_valid(&session))
      fprintf(stderr, "  auth: no Grok Build CLI token — use --login or browser /activate\n");
  }
#endif

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
  if (!auth_status && !auth_start && !auth_poll)
    fprintf(stderr, "  backend: %s  base=%s  model=%s\n",
            ng_agent_backend_kind(&agent),
            agent.base_url ? agent.base_url : "?",
            agent.model ? agent.model : "?");

  /* --- CLI auth (JSON on stdout; no HTTP) — product UI path --- */
  if (auth_status || auth_start || auth_poll) {
#if !NANOBOT_ENABLE_AUTH
    printf("{\"ok\":false,\"error\":\"AUTH disabled in build\"}\n");
    ng_session_free(&session);
    ng_agent_cfg_free(&agent);
    return 2;
#else
    /* Seal key needs peer_token file even without HTTP. */
    {
      char pt[640];
      snprintf(pt, sizeof pt, "%s/peer_token", home);
      if (access(pt, R_OK) != 0) {
        unsigned char raw[16];
        char tok[33];
        if (nb_random_bytes(raw, sizeof raw) == 0 &&
            nb_hex_encode(raw, sizeof raw, tok, sizeof tok) == 0) {
          char line[64];
          int n = snprintf(line, sizeof line, "token=%s\n", tok);
          (void)nb_write_secret_file(pt, line, (size_t)n);
          nb_secure_wipe(raw, sizeof raw);
          nb_secure_wipe(tok, sizeof tok);
        }
      }
    }
    {
      const char *poll_state = NULL;
      const char *poll_error = NULL;
      if (auth_start) {
        if (auth_force || !ng_session_valid(&session)) {
          if (auth_force) ng_session_clear(&session);
          else ng_session_load_pending(&session);
          if (!session.login_pending || auth_force) {
            if (ng_session_start_device_login(&session) != 0) {
              printf("{\"ok\":false,\"error\":\"device login failed (network/DNS?)\","
                     "\"poll_state\":\"error\",\"cross_device_ok\":true}\n");
              ng_session_free(&session);
              ng_agent_cfg_free(&agent);
              return 1;
            }
          }
        }
        poll_state = ng_session_valid(&session) ? "signed_in"
          : (session.login_pending ? "pending" : "none");
      }
      if (auth_poll || (auth_status && session.login_pending)) {
        /* Always reload pending so a fresh CLI process sees device_login
         * sealed by --auth-start (same home). Browser may be on another device. */
        (void)ng_session_load_pending(&session);
        if (!session.login_pending || !session.device_code) {
          if (ng_session_valid(&session)) {
            poll_state = "signed_in";
          } else {
            poll_state = "none";
            poll_error = "no_pending";
          }
        } else {
          int pr = ng_session_poll_login(&session);
          if (pr == 1) {
            fprintf(stderr, "  auth: browser approved (cli poll; any device OK)\n");
            poll_state = "signed_in";
          } else if (pr == 0) {
            poll_state = "pending"; /* authorization_pending / throttle / transport */
          } else {
            /* pr < 0: expired, denied, or hard error — pending cleared in poll */
            if (!session.login_pending) {
              poll_state = "expired";
              poll_error = "device_code_expired_or_denied";
            } else {
              poll_state = "error";
              poll_error = "poll_failed";
            }
          }
        }
      }
      if (auth_status && need_browser && !ng_session_valid(&session) && !session.login_pending)
        (void)ng_session_ensure(&session);
      if (ng_session_valid(&session)) poll_state = "signed_in";
      else if (session.login_pending && (!poll_state || !strcmp(poll_state, "none")))
        poll_state = "pending";
      print_auth_json(&session, &agent, poll_state, poll_error);
    }
    ng_session_free(&session);
    ng_agent_cfg_free(&agent);
    return 0;
#endif
  }

  if (list_models) {
    if (need_browser && ng_session_valid(&session))
      ng_session_ensure(&session);
    char *raw = ng_agent_fetch_models_json(&agent);
    char *ids = ng_agent_models_ids_json(raw);
    int ok = 0;
    if (ids && ids[0] == '[' && strcmp(ids, "[]") != 0) {
      const char *p = ids + 1;
      while (*p) {
        while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r') p++;
        if (*p == ']') break;
        if (*p == '"') {
          p++;
          const char *e = p;
          while (*e && *e != '"') e++;
          fwrite(p, 1, (size_t)(e - p), stdout);
          fputc('\n', stdout);
          ok = 1;
          p = (*e == '"') ? e + 1 : e;
        } else break;
      }
    }
    if (!ok && raw) {
      fputs(raw, stdout);
      if (raw[0] && raw[strlen(raw) - 1] != '\n') fputc('\n', stdout);
    }
    free(raw);
    free(ids);
    ng_session_free(&session);
    ng_agent_cfg_free(&agent);
    return 0;
  }


  if (mode_mcp) {
#if !NANOBOT_ENABLE_MCP
    fprintf(stderr, "nanobot: built without MCP (NANOBOT_ENABLE_MCP=0)\n");
    ng_session_free(&session);
    ng_agent_cfg_free(&agent);
    return 2;
#else
    if (need_browser) {
#if !NANOBOT_ENABLE_AUTH
      fprintf(stderr, "nanobot: Grok backend needs AUTH; rebuild with NANOBOT_ENABLE_AUTH=ON\n"
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
        fprintf(stderr, "No Grok session. Use nanobot --login, or --offline for llama.cpp,\n"
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

  /* --login without --port: blocking browser device-code, then exit (no HTTP). */
  if (force_login && need_browser && !want_peer) {
#if !NANOBOT_ENABLE_AUTH
    fprintf(stderr, "  --login: AUTH disabled in this build\n");
    ng_session_free(&session);
    ng_agent_cfg_free(&agent);
    return 2;
#else
    if (ng_session_login_blocking(&session) != 0) {
      ng_session_free(&session);
      ng_agent_cfg_free(&agent);
      return 1;
    }
    print_auth_json(&session, &agent, "signed_in", NULL);
    ng_session_free(&session);
    ng_agent_cfg_free(&agent);
    return 0;
#endif
  }

  if (force_login && need_browser && want_peer) {
#if !NANOBOT_ENABLE_AUTH
    fprintf(stderr, "  --login: AUTH disabled in this build\n");
#else
    ng_session_clear(&session);
    (void)ng_session_start_device_login(&session);
    fprintf(stderr, "  --login: device-code started; peer will poll\n");
#endif
  } else if (need_browser && ng_session_valid(&session)) {
#if NANOBOT_ENABLE_AUTH
    if (ng_session_ensure(&session) != 0)
      fprintf(stderr, "  Session refresh failed; use --login or --auth-start\n");
#endif
  } else if (need_browser && !want_peer) {
    fprintf(stderr,
            "  No cloud session. Run: nanobot --login\n"
            "  Or: nanobot --auth-start  (JSON for UI) then --auth-poll\n"
            "  Or: --offline / --base-url for local llama (no browser)\n"
            "  HTTP peer is opt-in: --port N (loopback) or --port N --lan\n");
    ng_session_free(&session);
    ng_agent_cfg_free(&agent);
    return 2;
  } else if (need_browser) {
    fprintf(stderr,
            "  No cloud session — peer will accept /activate or --login first\n");
  } else {
    fprintf(stderr, "  Local/OpenAI-compatible backend — no browser session required\n");
  }

  /* No --port / --peer / --hub: CLI-only. Do not open a network socket. */
  if (!want_peer) {
    if (oneshot || mode_mcp || list_models || force_login) {
      /* already handled above */
    }
    fprintf(stderr,
            "nanobot: nothing to do.\n"
            "  Chat:   nanobot -p 'prompt'\n"
            "  Auth:   nanobot --login | --auth-status | --auth-start\n"
            "  MCP:    nanobot --mcp\n"
            "  HTTP:   nanobot --port 8787          # 127.0.0.1 only\n"
            "          nanobot --port 8787 --lan    # intentional LAN\n");
    usage(argv[0]);
    ng_session_free(&session);
    ng_agent_cfg_free(&agent);
    return 2;
  }

  /* Peer token only when HTTP is actually started. */
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

  {
    int master_present = 0;
    const char *env_m = getenv("NANOBOT_LABAUTH_MASTER");
    if (env_m && env_m[0] && access(env_m, R_OK) == 0)
      master_present = 1;
    if (!master_present) {
      char alt[700];
      snprintf(alt, sizeof alt, "%s/labauth/master.key", home);
      if (access(alt, R_OK) == 0) master_present = 1;
    }
    if (master_present) {
      fprintf(stderr, "  optional master key present (NANOBOT_LABAUTH_MASTER or $HOME/labauth/)\n");
    }
    const char *req_la = getenv("NANOBOT_REQUIRE_LABAUTH");
    if (req_la && (!strcmp(req_la, "1") || !strcasecmp(req_la, "true") ||
                   !strcasecmp(req_la, "yes"))) {
      if (!master_present) {
        fprintf(stderr,
                "nanobot: NANOBOT_REQUIRE_LABAUTH=1 but master key not found\n"
                "  set NANOBOT_LABAUTH_MASTER=/path/to/master.key\n"
                "  or place $NANOBOT_HOME/labauth/master.key\n");
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

  if (port <= 0) port = NG_DEFAULT_PORT;
  if (bind_lan)
    fprintf(stderr, "  WARN: --lan binds 0.0.0.0 — mutate routes need peer_token\n");
  else
    fprintf(stderr, "  HTTP bind: 127.0.0.1:%d (not LAN-visible)\n", port);

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
    .bind_lan = bind_lan,
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
