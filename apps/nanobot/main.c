#include "agent.h"
#include "auth.h"
#include "http.h"
#include "mcp.h"
#include "mcp_remote.h"
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
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

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

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

/* Install stop signals without SA_RESTART so accept() wakes on SIGTERM. */
static void install_stop_signals(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_sig;
  sa.sa_flags = 0; /* no SA_RESTART */
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGINT, &sa, NULL) != 0) signal(SIGINT, on_sig);
  if (sigaction(SIGTERM, &sa, NULL) != 0) signal(SIGTERM, on_sig);
}

/* True if a peer is actually accepting on port (LISTEN).
 * Residual: bind-probe without SO_REUSEADDR returns EADDRINUSE on TIME_WAIT
 * after cool_restart SIGTERM → false already_listening while nothing listens.
 * Connect to loopback: only a live accept() succeeds; TIME_WAIT does not. */
static int peer_port_in_use(int port, int bind_lan) {
  (void)bind_lan;
  if (port <= 0 || port >= 65536) return 0;
  int sfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sfd < 0) return 0;
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 250000; /* 250ms — lab loopback is instant when live */
  setsockopt(sfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  int rc = connect(sfd, (struct sockaddr *)&addr, sizeof addr);
  int live = (rc == 0);
  close(sfd);
  return live;
}

static void print_already_listening(int port, int bind_lan) {
  const char *host = bind_lan ? "0.0.0.0" : "127.0.0.1";
  fprintf(stderr,
          "{\"schema\":\"nanobot.serve.v1\",\"ok\":true,"
          "\"action\":\"already_listening\",\"port\":%d,\"bind\":\"%s\","
          "\"product_wire\":\"smx2\",\"peer_http\":\"lab_ops_only\","
          "\"peer_http_is_product_bus\":false,\"share\":\"state_matrix_only\","
          "\"hold_flash\":1,\"llm_is_commander\":false,\"python\":0}\n",
          port, bind_lan ? "lan" : "loopback");
  printf("http://%s:%d/peer/v1/info\n", host, port);
  fflush(stdout);
}

/* CLI real-time token printer */
static void cli_stream_delta(void *ud, const char *chunk, size_t n) {
  (void)ud;
  if (!chunk || !n) return;
  fwrite(chunk, 1, n, stdout);
  fflush(stdout);
}

/*
 * Dual-wire CLI deny — machine action/error/hint only (no free-text essay).
 * Product bus remains SMX2; peer HTTP is lab/ops only; py=0.
 */
static void print_cli_err(const char *action, const char *error, const char *hint) {
  char *a = ng_json_escape(action && action[0] ? action : "cli");
  char *e = ng_json_escape(error && error[0] ? error : "deny");
  char *h = ng_json_escape(hint ? hint : "");
  fprintf(stderr,
          "{\"schema\":\"nanobot.cli.v1\",\"ok\":false,"
          "\"action\":\"%s\",\"error\":\"%s\",\"hint\":\"%s\","
          "\"product_wire\":\"smx2\",\"peer_http\":\"lab_ops_only\","
          "\"peer_http_is_product_bus\":false,\"share\":\"state_matrix_only\","
          "\"hold_flash\":1,\"llm_is_commander\":false,\"python\":0}\n",
          a ? a : "cli", e ? e : "deny", h ? h : "");
  free(a);
  free(e);
  free(h);
}

/*
 * Dual-wire ready plate — replaces free-text limits/braincube/backend dump.
 * Never dumps secrets. Product bus SMX2; peer HTTP lab/ops only; py=0.
 */
static void print_ready_plate(const ng_agent_cfg *agent, const char *auth_import) {
  char *ver = ng_json_escape(NG_VERSION);
  char *be = ng_json_escape(agent ? ng_agent_backend_kind(agent) : "unknown");
  char *base = ng_json_escape(agent && agent->base_url ? agent->base_url : "");
  char *model = ng_json_escape(agent && agent->model ? agent->model : "");
  char *ai = ng_json_escape(auth_import && auth_import[0] ? auth_import : "none");
  fprintf(stderr,
          "{\"schema\":\"nanobot.ready.v1\",\"ok\":true,\"action\":\"ready\","
          "\"version\":\"%s\",\"lean\":%s,\"max_turns\":%d,"
          "\"max_children\":%d,\"out_max\":%zu,\"log_max\":%zu,"
          "\"braincube\":%s,\"continuous\":%s,"
          "\"backend\":\"%s\",\"base_url\":\"%s\",\"model\":\"%s\","
          "\"auth_import\":\"%s\","
          "\"product_wire\":\"smx2\",\"peer_http\":\"lab_ops_only\","
          "\"peer_http_is_product_bus\":false,\"share\":\"state_matrix_only\","
          "\"hold_flash\":1,\"llm_is_commander\":false,\"python\":0}\n",
          ver ? ver : "", ng_is_lean() ? "true" : "false", ng_max_turns(),
          ng_http_max_children(), ng_out_max(), ng_log_max(),
          ng_bc_available() ? "true" : "false",
          ng_bc_continuous() ? "true" : "false", be ? be : "unknown",
          base ? base : "", model ? model : "", ai ? ai : "none");
  free(ver);
  free(be);
  free(base);
  free(model);
  free(ai);
}

/*
 * Dual-wire listen ack after HTTP bind intent (matches grokium serve plate).
 * No free-text essay · no peer_mcp_bridge.py (py=0 · native --mcp only).
 * Bind host is honest: loopback unless --lan. peer_token never dumps secret.
 */
/* Userdata for ng_http_cfg.on_listening — plate only after bind succeeds. */
typedef struct {
  int port;
  int bind_lan;
  int hub_mode;
  int port_out;
  ng_session *session;
  const char *www_root;
  const char *peer_token_state;
} listen_plate_ctx;

static void print_listen_plate(int port, int bind_lan, int hub_mode, int port_out,
                               ng_session *sess, const char *www_root,
                               const char *peer_token_state) {
  const char *host = bind_lan ? "0.0.0.0" : "127.0.0.1";
  int login_pending = sess && sess->login_pending;
  int has_www = www_root && www_root[0];
  char *ver = ng_json_escape(NG_VERSION);
  char *www_esc = has_www ? ng_json_escape(www_root) : NULL;
  char *pts = ng_json_escape(peer_token_state && peer_token_state[0]
                                 ? peer_token_state
                                 : "unknown");
  char *vu = NULL, *vuc = NULL, *uc = NULL;
  if (port <= 0) port = NG_DEFAULT_PORT;
  if (sess) {
    if (sess->verification_uri) vu = ng_json_escape(sess->verification_uri);
    if (sess->verification_uri_complete)
      vuc = ng_json_escape(sess->verification_uri_complete);
    if (sess->user_code) uc = ng_json_escape(sess->user_code);
  }
  fprintf(stderr,
          "{\"schema\":\"nanobot.serve.v1\",\"ok\":true,"
          "\"action\":\"listen\",\"version\":\"%s\","
          "\"host\":\"%s\",\"port\":%d,\"bind\":\"%s\","
          "\"control_plane\":\"peer_http\",\"mcp\":\"native_stdio\","
          "\"endpoints\":[\"/peer/v1/health\",\"/health\",\"/ready\","
          "\"/peer/v1/info\",\"/peer/v1/prompt\","
          "\"/peer/v1/shell\",\"/peer/v1/jobs\"],"
          "\"www\":%s,\"www_root\":\"%s\","
          "\"hub\":%s,\"port_out\":%d,"
          "\"peer_token\":\"%s\","
          "\"login_pending\":%s,\"user_code\":\"%s\","
          "\"verification_uri\":\"%s\",\"verification_uri_complete\":\"%s\","
          "\"product_wire\":\"smx2\",\"peer_http\":\"lab_ops_only\","
          "\"peer_http_is_product_bus\":false,\"share\":\"state_matrix_only\","
          "\"hold_flash\":1,\"llm_is_commander\":false,\"python\":0}\n",
          ver ? ver : "", host, port, bind_lan ? "lan" : "loopback",
          has_www ? "true" : "false", www_esc ? www_esc : "",
          hub_mode ? "true" : "false",
          hub_mode && port_out > 0 ? port_out : 0, pts ? pts : "unknown",
          login_pending ? "true" : "false", uc ? uc : "", vu ? vu : "",
          vuc ? vuc : "");
  /* Primary operator URL on stdout (machine token path only). */
  if (vuc && vuc[0])
    printf("%s\n", sess->verification_uri_complete);
  else if (vu && vu[0])
    printf("%s\n", sess->verification_uri);
  else if (has_www)
    printf("http://%s:%d/\n", host, port);
  else
    printf("http://%s:%d/peer/v1/info\n", host, port);
  fflush(stdout);
  free(ver);
  free(www_esc);
  free(pts);
  free(vu);
  free(vuc);
  free(uc);
}

static void ng_http_listening_plate_cb(ng_http_cfg *c) {
  listen_plate_ctx *x = (listen_plate_ctx *)(c ? c->on_listening_ud : NULL);
  if (!x) return;
  print_listen_plate(x->port, x->bind_lan, x->hub_mode, x->port_out,
                     x->session, x->www_root, x->peer_token_state);
}

/* Dual-wire --help (machine command index · no free-text multi-line dump). */
static void usage(const char *argv0) {
  char *ver = ng_json_escape(NG_VERSION);
  char *prog = ng_json_escape(argv0 && argv0[0] ? argv0 : "nanobot");
  (void)prog;
  fprintf(stderr,
          "{\"schema\":\"nanobot.cli.v1\",\"ok\":true,\"action\":\"help\","
          "\"version\":\"%s\","
          "\"content\":\"meta_only\","
          "\"commands\":\"-p|--help|--version|--login|--auth-status|"
          "--auth-start|--auth-poll|--offline|--base-url|--model|--models|"
          "--mcp|--mcp-list|--mcp-call|--order|--port|--lan|--hub|"
          "--port-out|--www|--home\","
          "\"mcp\":%d,\"auth\":%d,\"peer\":%d,\"hub\":%d,\"shell\":%d,"
          "\"providers\":%d,"
          "\"product_wire\":\"smx2\",\"peer_http\":\"lab_ops_only\","
          "\"peer_http_is_product_bus\":false,\"share\":\"state_matrix_only\","
          "\"hold_flash\":1,\"llm_is_commander\":false,\"python\":0,"
          "\"hint\":\"no_server_unless_--port|native_mcp_stdio|py=0\"}\n",
          ver ? ver : "",
          NANOBOT_ENABLE_MCP, NANOBOT_ENABLE_AUTH, NANOBOT_ENABLE_PEER,
          NANOBOT_ENABLE_HUB, NANOBOT_ENABLE_SHELL, NANOBOT_ENABLE_PROVIDERS);
  free(ver);
  free(prog);
}

/* Pure-C commander order path (NeuralCube). No Python. No LLM. */
static void order_log(const char *line) {
  char path[700];
  snprintf(path, sizeof path, "%s/orders.log", ng_workdir());
  FILE *f = fopen(path, "a");
  if (!f) return;
  time_t t = time(NULL);
  struct tm tm;
  localtime_r(&t, &tm);
  char ts[40];
  strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", &tm);
  fprintf(f, "%s %s\n", ts, line ? line : "");
  fclose(f);
}

static int run_mcp_call_cli(const char *server, const char *tool, const char *args_obj) {
#if !NANOBOT_ENABLE_MCP
  (void)server; (void)tool; (void)args_obj;
  print_cli_err("mcp_call", "mcp_disabled", "rebuild_with_NANOBOT_ENABLE_MCP=1");
  return 2;
#else
  if (!server || !server[0] || !tool || !tool[0]) {
    print_cli_err("mcp_call", "need_server_tool", "SERVER TOOL [JSON_OBJECT]");
    return 2;
  }
  const char *args = (args_obj && args_obj[0]) ? args_obj : "{}";
  if (args[0] != '{') {
    print_cli_err("mcp_call", "need_json_object", "arguments_must_be_{}");
    return 2;
  }
  char *esc_s = ng_json_escape(server);
  char *esc_t = ng_json_escape(tool);
  /* arguments field as embedded object (mcp_remote accepts raw object) */
  char *payload = NULL;
  if (asprintf(&payload,
               "{\"server\":\"%s\",\"tool\":\"%s\",\"arguments\":%s}",
               esc_s ? esc_s : server, esc_t ? esc_t : tool, args) < 0) {
    free(esc_s); free(esc_t);
    return 1;
  }
  free(esc_s); free(esc_t);
  char *out = ng_mcp_try_tool("mcp_call", payload);
  free(payload);
  if (!out) {
    print_cli_err("mcp_call", "not_handled", "check_mcp_servers.json");
    return 1;
  }
  fputs(out, stdout);
  if (out[0] && out[strlen(out) - 1] != '\n') fputc('\n', stdout);
  char logline[400];
  snprintf(logline, sizeof logline, "mcp_call server=%s tool=%s ok", server, tool);
  order_log(logline);
  /* LAST_ORDER.json pure C */
  {
    char dir[700], path[700];
    snprintf(dir, sizeof dir, "%s/nexus", ng_workdir());
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/LAST_ORDER.json", dir);
    FILE *f = fopen(path, "w");
    if (f) {
      fprintf(f, "{\"server\":\"%s\",\"tool\":\"%s\"}\n", server, tool);
      fclose(f);
    }
  }
  free(out);
  return 0;
#endif
}

static int run_order_cli(const char *target, const char *text) {
#if !NANOBOT_ENABLE_MCP
  (void)target; (void)text;
  print_cli_err("order", "mcp_disabled", "rebuild_with_NANOBOT_ENABLE_MCP=1");
  return 2;
#else
  if (!target || !target[0]) {
    print_cli_err("order", "need_target", "status|nexus|blackcube");
    return 2;
  }
  if (!strcmp(target, "status") || !strcmp(target, "stat")) {
    char *list = ng_mcp_try_tool("mcp_list", "{}");
    if (list) { fputs(list, stdout); free(list); }
    return run_mcp_call_cli("nexuscore", "nexus_status", "{}");
  }
  if (!strcmp(target, "nexus") || !strcmp(target, "nexuscore") ||
      !strcmp(target, "hive") || !strcmp(target, "core")) {
    const char *msg = (text && text[0]) ? text : "ORDER from NeuralCube Titan commander";
    char *esc = ng_json_escape(msg);
    char *args = NULL;
    if (asprintf(&args, "{\"text\":\"%s\"}", esc ? esc : msg) < 0) {
      free(esc);
      return 1;
    }
    free(esc);
    int rc = run_mcp_call_cli("nexuscore", "nexus_command", args);
    free(args);
    if (rc != 0) {
      /* soft fallback: smx tick still advances hive (dual-wire note). */
      print_cli_err("order", "nexus_command_soft_fail", "fallback_nexus_smx_tick");
      return run_mcp_call_cli("nexuscore", "nexus_smx_tick", "{}");
    }
    return 0;
  }
  if (!strcmp(target, "blackcube") || !strcmp(target, "bc") || !strcmp(target, "station")) {
    const char *sub = (text && text[0]) ? text : "ping";
    /* first word only for subcommand */
    char sub0[64];
    size_t i = 0;
    while (sub[i] && sub[i] != ' ' && i + 1 < sizeof sub0) {
      sub0[i] = sub[i];
      i++;
    }
    sub0[i] = 0;
    if (!strcmp(sub0, "prophecy") || !strcmp(sub0, "tick"))
      return run_mcp_call_cli("blackcube", "blackcube_prophecy_tick", "{}");
    if (!strcmp(sub0, "status"))
      return run_mcp_call_cli("blackcube", "blackcube_cube_status", "{}");
    return run_mcp_call_cli("blackcube", "blackcube_ping", "{}");
  }
  print_cli_err("order", "unknown_target", "status|nexus|blackcube");
  return 2;
#endif
}

/* Dual-wire auth plate for UI / app CLI wrapper (stdout = one JSON line).
 * schema nanobot.auth.v1 — machine fields only (no free-text essays).
 * poll_state: none|pending|signed_in|expired|denied|error|throttled
 * Cross-device browser approval is normal — pending until token, not "cancelled".
 * Product bus remains SMX2; CLI auth transport is lab/ops only. */
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
  char *err_esc = NULL, *wd_esc = NULL, *ver_esc = NULL, *be_esc = NULL;
  long deadline = (s && s->device_deadline) ? (long)s->device_deadline : 0;
  if (s && need_browser) {
    if (s->verification_uri) vu = ng_json_escape(s->verification_uri);
    if (s->verification_uri_complete) vuc = ng_json_escape(s->verification_uri_complete);
    if (s->user_code) uc = ng_json_escape(s->user_code);
  }
  if (agent && agent->base_url) base_esc = ng_json_escape(agent->base_url);
  if (agent && agent->model) model_esc = ng_json_escape(agent->model);
  if (poll_error && poll_error[0]) err_esc = ng_json_escape(poll_error);
  wd_esc = ng_json_escape(ng_workdir());
  ver_esc = ng_json_escape(NG_VERSION);
  be_esc = ng_json_escape(backend);
  printf(
    "{\"schema\":\"nanobot.auth.v1\",\"ok\":true,\"version\":\"%s\","
    "\"signed_in\":%s,\"login_pending\":%s,"
    "\"login_required\":%s,\"needs_browser\":%s,\"user_code\":\"%s\","
    "\"verification_uri\":\"%s\",\"verification_uri_complete\":\"%s\","
    "\"backend\":\"%s\",\"base_url\":\"%s\",\"model\":\"%s\","
    "\"auth\":\"%s\",\"workdir\":\"%s\",\"transport\":\"cli\","
    "\"poll_state\":\"%s\",\"error\":\"%s\",\"device_deadline\":%ld,"
    "\"cross_device_ok\":true,"
    "\"product_wire\":\"smx2\",\"peer_http\":\"lab_ops_only\","
    "\"peer_http_is_product_bus\":false,\"share\":\"state_matrix_only\","
    "\"hold_flash\":1,\"llm_is_commander\":false,\"python\":0}\n",
    ver_esc ? ver_esc : "",
    signed_in ? "true" : "false",
    pending ? "true" : "false",
    (need_browser && !signed_in) ? "true" : "false",
    need_browser ? "true" : "false",
    uc ? uc : "",
    vu ? vu : "",
    vuc ? vuc : "",
    be_esc ? be_esc : "",
    base_esc ? base_esc : "",
    model_esc ? model_esc : "",
    need_browser ? "browser_device_code" : "local_openai_compatible",
    wd_esc ? wd_esc : "",
    state,
    err_esc ? err_esc : "",
    deadline);
  free(vu); free(vuc); free(uc); free(base_esc); free(model_esc); free(err_esc);
  free(wd_esc); free(ver_esc); free(be_esc);
  fflush(stdout);
}

int main(int argc, char **argv) {
  /* port < 0 means HTTP peer not requested (CLI-first product path). */
  int port = -1;
  int want_peer = 0;
  int bind_lan = 0;
  int mode_mcp = 0;
  int mode_mcp_list = 0;
  int mode_mcp_call = 0;
  int mode_order = 0;
  const char *mcp_call_server = NULL;
  const char *mcp_call_tool = NULL;
  const char *mcp_call_args = NULL;
  const char *order_target = NULL;
  char order_text[2048];
  order_text[0] = 0;
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
    } else if (access("/data/local/tmp/nanobot_home", X_OK) == 0 ||
               access("/data/local/tmp/nanobot_home", R_OK) == 0) {
      /* Titan product: empty HOME on Android shell/app → lab shared home. */
      home = "/data/local/tmp/nanobot_home";
    } else {
      home = "/tmp/nanobot";
    }
  }

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      usage(argv[0]); return 0;
    } else if (strcmp(argv[i], "--mcp") == 0 || strcmp(argv[i], "mcp") == 0) {
      mode_mcp = 1;
    } else if (strcmp(argv[i], "--mcp-list") == 0 || strcmp(argv[i], "mcp-list") == 0) {
      mode_mcp_list = 1;
    } else if ((strcmp(argv[i], "--mcp-call") == 0 || strcmp(argv[i], "mcp-call") == 0) &&
               i + 2 < argc) {
      mode_mcp_call = 1;
      mcp_call_server = argv[++i];
      mcp_call_tool = argv[++i];
      if (i + 1 < argc && argv[i + 1][0] == '{')
        mcp_call_args = argv[++i];
    } else if ((strcmp(argv[i], "--order") == 0 || strcmp(argv[i], "order") == 0) &&
               i + 1 < argc) {
      mode_order = 1;
      order_target = argv[++i];
      size_t off = 0;
      for (int j = i + 1; j < argc; j++) {
        size_t n = strlen(argv[j]);
        if (off + n + 2 >= sizeof order_text) break;
        if (off) order_text[off++] = ' ';
        memcpy(order_text + off, argv[j], n);
        off += n;
        order_text[off] = 0;
      }
      break; /* remaining argv is order text */
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
        print_cli_err("www", "need_www_dir", "pass_--www_DIR|NANOBOT_WWW");
        return 2;
      }
    } else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--prompt") == 0) && i + 1 < argc) {
      oneshot = argv[++i];
    } else if (strcmp(argv[i], "--version") == 0) {
      printf("nanobot %s\n", NG_VERSION); return 0;
    } else {
      print_cli_err("cli", "unknown_arg", argv[i]);
      usage(argv[0]); return 2;
    }
  }
  if (want_peer && port < 0) port = NG_DEFAULT_PORT;
  if (hub_mode && !want_peer) {
    want_peer = 1;
    if (port < 0) port = NG_DEFAULT_PORT;
  }

  /*
   * Titan 1.8.2 residual: app-private --home /data/user/0/…/nanobot_home with
   * no sealed session while /data/local/tmp/nanobot_home still holds Grok.
   * Redirect so peer/CLI never report signed_in=false over a live shared seal.
   */
  {
    static const char *shared_home = "/data/local/tmp/nanobot_home";
    char sess_cur[700], sess_shared[700], tok_shared[700];
    int cur_app =
      (home && (strstr(home, "/data/user/") || strstr(home, "/data/data/")));
    snprintf(sess_cur, sizeof sess_cur, "%s/session", home ? home : "");
    snprintf(sess_shared, sizeof sess_shared, "%s/session", shared_home);
    snprintf(tok_shared, sizeof tok_shared, "%s/peer_token", shared_home);
    if (cur_app && access(sess_cur, R_OK) != 0 &&
        access(sess_shared, R_OK) == 0 && access(tok_shared, R_OK) == 0) {
      /* Dual-wire note only — paths not free-text essay on stderr. */
      print_cli_err("auth", "workdir_redirect", "shared_session_sot");
      home = shared_home;
    }
  }

  ng_set_workdir(home);
  mkdir(home, 0755);
  /* Mesh focus loop paths — avoid ENOENT on fresh home (reports/titan-loop). */
  {
    char p[700];
    snprintf(p, sizeof p, "%s/reports", home);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/reports/titan-loop", home);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/reports/titan-loop/pulls", home);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/jobs", home);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/approvals", home);
    mkdir(p, 0755);
  }
  ng_shell_ensure_policy_files();

  /* Pure-C MCP / commander order path — no Python, no LLM (NeuralCube). */
  if (mode_mcp_list) {
#if NANOBOT_ENABLE_MCP
    char *out = ng_mcp_try_tool("mcp_list", "{}");
    if (out) {
      fputs(out, stdout);
      if (out[0] && out[strlen(out) - 1] != '\n') fputc('\n', stdout);
      free(out);
      return 0;
    }
#endif
    print_cli_err("mcp_list", "unavailable", "NANOBOT_ENABLE_MCP|mcp_servers.json");
    return 1;
  }
  if (mode_mcp_call)
    return run_mcp_call_cli(mcp_call_server, mcp_call_tool, mcp_call_args);
  if (mode_order)
    return run_order_cli(order_target, order_text);

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

  /* Residual: ensure/restart thrash re-execed nanobot while peer live →
   * auth import + ready spam + bind EADDRINUSE. Already-up is success. */
  if (want_peer && !oneshot && !mode_mcp && !mode_mcp_list && !mode_mcp_call &&
      !mode_order && !auth_status && !auth_start && !auth_poll && port > 0 &&
      peer_port_in_use(port, bind_lan)) {
    print_already_listening(port, bind_lan);
    return 0;
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
        print_cli_err("www", "no_index_html", "static_off");
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
  /* BrainCube: parent continuous learn (fork workers only serve snapshots). */
  ng_bc_boot_parent();

  ng_session session;
  ng_session_init(&session);
  ng_session_load(&session);
  /* Import Grok Build CLI when sealed session missing/expired, or --import-grok-cli. */
  const char *auth_import = "none";
#if NANOBOT_ENABLE_AUTH
  {
    const char *fe = getenv("NANOBOT_IMPORT_GROK_CLI");
    int force_import = (fe && fe[0] && fe[0] != '0');
    if (!force_offline && (force_import || !ng_session_valid(&session))) {
      int imp = ng_session_try_import_grok_cli(&session);
      if (imp == 1)
        auth_import = "imported";
      else if (imp == 0 && !ng_session_valid(&session))
        auth_import = "missing";
    }
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
  /* Dual-wire ready (limits/braincube/backend) — no free-text dump. */
  if (!auth_status && !auth_start && !auth_poll)
    print_ready_plate(&agent, auth_import);

  /* --- CLI auth (JSON on stdout; no HTTP) — product UI path --- */
  if (auth_status || auth_start || auth_poll) {
#if !NANOBOT_ENABLE_AUTH
    /* Dual-wire auth deny — machine tokens only. */
    printf("{\"schema\":\"nanobot.auth.v1\",\"ok\":false,"
           "\"error\":\"auth_disabled\",\"poll_state\":\"error\","
           "\"cross_device_ok\":true,\"transport\":\"cli\","
           "\"product_wire\":\"smx2\",\"peer_http\":\"lab_ops_only\","
           "\"peer_http_is_product_bus\":false,"
           "\"share\":\"state_matrix_only\",\"hold_flash\":1,"
           "\"llm_is_commander\":false,\"python\":0}\n");
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
              /* Dual-wire deny — machine token only (no free-text network essay). */
              printf("{\"schema\":\"nanobot.auth.v1\",\"ok\":false,"
                     "\"error\":\"device_login_failed\",\"poll_state\":\"error\","
                     "\"cross_device_ok\":true,\"transport\":\"cli\","
                     "\"product_wire\":\"smx2\",\"peer_http\":\"lab_ops_only\","
                     "\"peer_http_is_product_bus\":false,"
                     "\"share\":\"state_matrix_only\",\"hold_flash\":1,"
                     "\"llm_is_commander\":false,\"python\":0}\n");
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
            /* Dual-wire poll_state=signed_in on stdout plate — no free-text banner. */
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
    print_cli_err("mcp", "mcp_disabled", "rebuild_with_NANOBOT_ENABLE_MCP=1");
    ng_session_free(&session);
    ng_agent_cfg_free(&agent);
    return 2;
#else
    if (need_browser) {
#if !NANOBOT_ENABLE_AUTH
      print_cli_err("mcp", "auth_disabled", "rebuild_AUTH_or_--offline");
      ng_session_free(&session);
      ng_agent_cfg_free(&agent);
      return 2;
#else
      if (!ng_session_valid(&session)) {
        print_cli_err("mcp", "need_session", "--login|browser_/activate|--offline");
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
        print_cli_err("chat", "need_session", "--login|--offline|@!_shell");
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
    print_cli_err("login", "auth_disabled", "rebuild_with_NANOBOT_ENABLE_AUTH=1");
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
    print_cli_err("login", "auth_disabled", "rebuild_with_NANOBOT_ENABLE_AUTH=1");
#else
    ng_session_clear(&session);
    (void)ng_session_start_device_login(&session);
    /* Dual-wire auth plate — peer poll owns the rest (no free-text essay). */
    print_auth_json(&session, &agent, "pending", NULL);
#endif
  } else if (need_browser && ng_session_valid(&session)) {
#if NANOBOT_ENABLE_AUTH
    if (ng_session_ensure(&session) != 0)
      print_cli_err("session", "refresh_failed", "--login|--auth-start");
#endif
  } else if (need_browser && !want_peer) {
    print_cli_err("session", "need_session",
                  "--login|--auth-start|--offline|--port");
    ng_session_free(&session);
    ng_agent_cfg_free(&agent);
    return 2;
  } else if (need_browser) {
    print_cli_err("session", "need_session_peer", "/activate|--login");
  }
  /* local OpenAI-compatible: ready plate already carries backend honesty */

  /* No --port / --peer / --hub: CLI-only. Do not open a network socket. */
  if (!want_peer) {
    if (oneshot || mode_mcp || list_models || force_login) {
      /* already handled above */
    }
    print_cli_err("cli", "nothing_to_do", "-p|--login|--mcp|--port");
    usage(argv[0]);
    ng_session_free(&session);
    ng_agent_cfg_free(&agent);
    return 2;
  }

  /* Peer token only when HTTP is actually started — never dump secret. */
  const char *peer_token_state = "missing";
  {
    char pt[640];
    snprintf(pt, sizeof pt, "%s/peer_token", home);
    if (access(pt, R_OK) != 0) {
      unsigned char raw[16];
      char tok[33];
      if (nb_random_bytes(raw, sizeof raw) != 0 ||
          nb_hex_encode(raw, sizeof raw, tok, sizeof tok) != 0) {
        peer_token_state = "create_failed";
      } else {
        char line[64];
        int n = snprintf(line, sizeof line, "token=%s\n", tok);
        if (nb_write_secret_file(pt, line, (size_t)n) == 0)
          peer_token_state = "created";
        else
          peer_token_state = "write_failed";
        nb_secure_wipe(raw, sizeof raw);
        nb_secure_wipe(tok, sizeof tok);
      }
    } else {
      peer_token_state = "present";
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
    const char *req_la = getenv("NANOBOT_REQUIRE_LABAUTH");
    if (req_la && (!strcmp(req_la, "1") || !strcasecmp(req_la, "true") ||
                   !strcasecmp(req_la, "yes"))) {
      if (!master_present) {
        /* Dual-wire deny — no free-text path dump (labauth machine tokens). */
        print_cli_err("labauth", "need_master_key",
                      "NANOBOT_LABAUTH_MASTER|labauth/master.key");
        ng_session_free(&session);
        ng_agent_cfg_free(&agent);
        return 1;
      }
    }
  }

#if !NANOBOT_ENABLE_PEER
  print_cli_err("serve", "peer_disabled", "rebuild_with_NANOBOT_ENABLE_PEER=1");
  ng_session_free(&session);
  ng_agent_cfg_free(&agent);
  return 2;
#endif

  if (port <= 0) port = NG_DEFAULT_PORT;
#if !NANOBOT_ENABLE_HUB
  if (hub_mode) {
    /* Hub not in this build — listen plate reports hub:false honestly. */
    hub_mode = 0;
  }
#else
  if (hub_mode && port_out <= 0) port_out = port + 1;
#endif

  install_stop_signals();

  if (hub_mode) {
#if NANOBOT_ENABLE_HUB
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

  /* Post-bind dual-wire plate (no false ok:true on EADDRINUSE). */
  listen_plate_ctx lctx = {
    .port = port,
    .bind_lan = bind_lan,
    .hub_mode = hub_mode,
    .port_out = port_out,
    .session = &session,
    .www_root = www_root,
    .peer_token_state = peer_token_state,
  };

  ng_http_cfg http = {
    .port = port,
    .agent = &agent,
    .session = &session,
    .stop = &g_stop, /* live flag — not a one-shot int copy */
    .www_root = www_root,
    .bind_lan = bind_lan,
    .on_listening = ng_http_listening_plate_cb,
    .on_listening_ud = &lctx,
  };
  int serve_rc = 0;
  while (!g_stop) {
    serve_rc = ng_http_serve(&http);
    if (serve_rc != 0) break;
    break;
  }
  g_stop = 1;

  ng_session_free(&session);
  ng_agent_cfg_free(&agent);
  return serve_rc != 0 ? 1 : 0;
}
