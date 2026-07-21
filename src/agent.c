#include "agent.h"
#include "memory.h"
#include "shell.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>

void ng_agent_cfg_init(ng_agent_cfg *c) {
  memset(c, 0, sizeof *c);
  c->base_url = strdup(NG_DEFAULT_BASE);
  c->model = strdup(NG_DEFAULT_MODEL);
  c->max_turns = ng_max_turns();
  c->timeout_sec = NG_CMD_TIMEOUT_SEC;
}

void ng_agent_cfg_free(ng_agent_cfg *c) {
  free(c->base_url); free(c->model);
  memset(c, 0, sizeof *c);
}

void ng_agent_load_env(ng_agent_cfg *c, const char *env_path) {
  if (env_path) {
    char *b = ng_slurp_env_file(env_path, "NANOBOT_BASE_URL");
    if (b) { free(c->base_url); c->base_url = b; }
    char *m = ng_slurp_env_file(env_path, "NANOBOT_MODEL");
    if (m) { free(c->model); c->model = m; }
  }
  char *b2 = ng_getenv_dup("NANOBOT_BASE_URL");
  if (b2) { free(c->base_url); c->base_url = b2; }
  char *m2 = ng_getenv_dup("NANOBOT_MODEL");
  if (m2) { free(c->model); c->model = m2; }
}

static int is_grok_endpoint(const char *url) {
  if (!url) return 0;
  if (strstr(url, "grok.com")) return 1;
  if (strstr(url, "api.x.ai")) return 1;
  if (strstr(url, "x.ai")) return 1;
  return 0;
}

int ng_agent_is_grok_backend(const ng_agent_cfg *c) {
  return c && is_grok_endpoint(c->base_url);
}

int ng_agent_needs_browser_session(const ng_agent_cfg *c) {
  return ng_agent_is_grok_backend(c);
}

const char *ng_agent_backend_kind(const ng_agent_cfg *c) {
  if (!c || !c->base_url) return "unknown";
  if (is_grok_endpoint(c->base_url)) return "grok";
  return "openai_compatible"; /* llama.cpp, local proxies, etc. */
}

void ng_agent_set_local_backend(ng_agent_cfg *c, const char *base_url, const char *model) {
  if (!c) return;
  free(c->base_url);
  c->base_url = strdup(base_url && base_url[0] ? base_url : NG_DEFAULT_LOCAL_BASE);
  if (model && model[0]) {
    free(c->model);
    c->model = strdup(model);
  } else if (!c->model) {
    c->model = strdup(NG_DEFAULT_LOCAL_MODEL);
  }
}

void ng_agent_set_grok_backend(ng_agent_cfg *c, const char *model) {
  if (!c) return;
  free(c->base_url);
  c->base_url = strdup(NG_DEFAULT_BASE);
  free(c->model);
  c->model = strdup(model && model[0] ? model : NG_DEFAULT_MODEL);
}

int ng_agent_save_env(const ng_agent_cfg *c) {
  if (!c || !c->base_url) return -1;
  char path[640];
  snprintf(path, sizeof path, "%s/env", ng_workdir());
  char buf[1024];
  int n = snprintf(buf, sizeof buf,
    "# written by nanobot\n"
    "NANOBOT_BASE_URL=%s\n"
    "NANOBOT_MODEL=%s\n",
    c->base_url,
    c->model ? c->model : "");
  if (n < 0 || n >= (int)sizeof buf) return -1;
  return ng_write_file(path, buf, (size_t)n);
}

/* OpenAI-compatible POST. Grok cloud headers when needed; plain Bearer for llama.cpp. */
static char *curl_post_json(const char *url, const char *bearer, const char *body) {
  char tmpl[] = "/tmp/ng_req_XXXXXX";
  int fd = mkstemp(tmpl);
  if (fd < 0) return strdup("mkstemp failed");
  write(fd, body, strlen(body));
  close(fd);

  char outtmpl[] = "/tmp/ng_resp_XXXXXX";
  int ofd = mkstemp(outtmpl);
  if (ofd < 0) { unlink(tmpl); return strdup("mkstemp out failed"); }
  close(ofd);

  char errtmpl[] = "/tmp/ng_cerr_XXXXXX";
  int efd = mkstemp(errtmpl);
  if (efd < 0) { unlink(tmpl); unlink(outtmpl); return strdup("mkstemp err failed"); }
  close(efd);

  char auth[1600];
  if (bearer && bearer[0])
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", bearer);
  else
    snprintf(auth, sizeof auth, "Authorization: Bearer none");

  char dataarg[80];
  snprintf(dataarg, sizeof dataarg, "@%s", tmpl);
  char ua[96];
  snprintf(ua, sizeof ua, "User-Agent: %s", ng_cli_user_agent());
  char verhdr[80];
  snprintf(verhdr, sizeof verhdr, "x-grok-client-version: %s", ng_cli_version());
  int grok = is_grok_endpoint(url);

  pid_t p2 = fork();
  if (p2 == 0) {
    int er = open(errtmpl, O_WRONLY | O_TRUNC);
    if (er >= 0) { dup2(er, STDERR_FILENO); close(er); }
    char *argv[40];
    int a = 0;
    argv[a++] = "curl";
    argv[a++] = "-sS";
    argv[a++] = "--max-time";
    argv[a++] = "45";
    argv[a++] = "--connect-timeout";
    argv[a++] = "12";
    argv[a++] = "-H";
    argv[a++] = "Content-Type: application/json";
    if (bearer && bearer[0]) {
      argv[a++] = "-H";
      argv[a++] = auth;
    }
    if (grok) {
      argv[a++] = "-H";
      argv[a++] = "X-XAI-Token-Auth: " NG_AUTH_TOKEN_HDR;
      argv[a++] = "-H";
      argv[a++] = "x-grok-client-mode: headless";
      argv[a++] = "-H";
      argv[a++] = "x-grok-client-surface: cli";
      argv[a++] = "-H";
      argv[a++] = verhdr;
      argv[a++] = "-H";
      argv[a++] = ua;
    } else {
      /* llama.cpp / OpenAI-compatible */
      argv[a++] = "-H";
      argv[a++] = "User-Agent: nanobot/" NG_VERSION;
    }
    argv[a++] = "--data-binary";
    argv[a++] = dataarg;
    argv[a++] = "-o";
    argv[a++] = outtmpl;
    argv[a++] = (char *)url;
    argv[a++] = NULL;
    execvp("curl", argv);
    _exit(127);
  }
  int st = 0;
  waitpid(p2, &st, 0);
  unlink(tmpl);
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
    char *err = ng_read_file(outtmpl, NULL);
    char *cerr = ng_read_file(errtmpl, NULL);
    unlink(outtmpl);
    unlink(errtmpl);
    if (err && err[0]) { free(cerr); return err; }
    free(err);
    if (cerr && cerr[0]) {
      char *out = NULL;
      asprintf(&out, "curl failed: %s", cerr);
      free(cerr);
      return out ? out : strdup("curl failed talking to API");
    }
    free(cerr);
    return strdup("curl failed talking to API");
  }
  unlink(errtmpl);
  char *resp = ng_read_file(outtmpl, NULL);
  unlink(outtmpl);
  return resp ? resp : strdup("");
}



/* GET with same auth headers as chat (session or API key). */
static char *curl_get_url(const char *url, const char *bearer, int grok_headers) {
  char outtmpl[] = "/tmp/ng_get_XXXXXX";
  int ofd = mkstemp(outtmpl);
  if (ofd < 0) return strdup("mkstemp out failed");
  close(ofd);
  char errtmpl[] = "/tmp/ng_gerr_XXXXXX";
  int efd = mkstemp(errtmpl);
  if (efd < 0) { unlink(outtmpl); return strdup("mkstemp err failed"); }
  close(efd);

  char auth[1600];
  if (bearer && bearer[0])
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", bearer);
  else
    snprintf(auth, sizeof auth, "Authorization: Bearer none");
  char ua[96];
  snprintf(ua, sizeof ua, "User-Agent: %s", ng_cli_user_agent());
  char verhdr[80];
  snprintf(verhdr, sizeof verhdr, "x-grok-client-version: %s", ng_cli_version());

  pid_t p2 = fork();
  if (p2 == 0) {
    int er = open(errtmpl, O_WRONLY | O_TRUNC);
    if (er >= 0) { dup2(er, STDERR_FILENO); close(er); }
    char *argv[40];
    int a = 0;
    argv[a++] = "curl";
    argv[a++] = "-sS";
    argv[a++] = "--max-time";
    argv[a++] = "30";
    argv[a++] = "--connect-timeout";
    argv[a++] = "12";
    if (bearer && bearer[0]) {
      argv[a++] = "-H";
      argv[a++] = auth;
    }
    if (grok_headers) {
      argv[a++] = "-H";
      argv[a++] = "X-XAI-Token-Auth: " NG_AUTH_TOKEN_HDR;
      argv[a++] = "-H";
      argv[a++] = "x-grok-client-mode: headless";
      argv[a++] = "-H";
      argv[a++] = "x-grok-client-surface: cli";
      argv[a++] = "-H";
      argv[a++] = verhdr;
      argv[a++] = "-H";
      argv[a++] = ua;
    } else {
      argv[a++] = "-H";
      argv[a++] = "User-Agent: nanobot/" NG_VERSION;
    }
    argv[a++] = "-o";
    argv[a++] = outtmpl;
    argv[a++] = (char *)url;
    argv[a++] = NULL;
    execvp("curl", argv);
    _exit(127);
  }
  int st = 0;
  waitpid(p2, &st, 0);
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
    char *err = ng_read_file(outtmpl, NULL);
    char *cerr = ng_read_file(errtmpl, NULL);
    unlink(outtmpl); unlink(errtmpl);
    if (err && err[0]) { free(cerr); return err; }
    free(err);
    if (cerr && cerr[0]) {
      char *out = NULL;
      asprintf(&out, "curl failed: %s", cerr);
      free(cerr);
      return out ? out : strdup("curl failed listing models");
    }
    free(cerr);
    return strdup("curl failed listing models");
  }
  unlink(errtmpl);
  char *resp = ng_read_file(outtmpl, NULL);
  unlink(outtmpl);
  return resp ? resp : strdup("");
}

char *ng_agent_fetch_models_json(ng_agent_cfg *c) {
  if (!c || !c->base_url || !c->base_url[0])
    return strdup("{\"error\":\"no base_url\"}");
  char url[768];
  /* Allow override list URL (grok-build: GROK_MODELS_LIST_URL) */
  const char *list = getenv("NANOBOT_MODELS_LIST_URL");
  if (list && list[0])
    snprintf(url, sizeof url, "%s", list);
  else {
    size_t n = strlen(c->base_url);
    if (n > 0 && c->base_url[n - 1] == '/')
      snprintf(url, sizeof url, "%smodels", c->base_url);
    else
      snprintf(url, sizeof url, "%s/models", c->base_url);
  }
  const char *bearer = NULL;
  char *tok_owned = NULL;
  int grok = is_grok_endpoint(c->base_url);
  if (grok && c->session) {
    bearer = ng_session_bearer(c->session);
  }
  if (!bearer || !bearer[0]) {
    tok_owned = ng_getenv_dup("NANOBOT_API_KEY");
    if (!tok_owned) tok_owned = ng_getenv_dup("OPENAI_API_KEY");
    if (!tok_owned) tok_owned = ng_getenv_dup("XAI_API_KEY");
    bearer = tok_owned;
  }
  char *body = curl_get_url(url, bearer, grok);
  free(tok_owned);
  return body;
}

/* Extract "id" fields from OpenAI {"data":[{"id":"..."},...]} or flat list. */
char *ng_agent_models_ids_json(const char *models_body) {
  if (!models_body || !models_body[0]) return strdup("[]");
  size_t cap = 4096, len = 1;
  char *out = malloc(cap);
  if (!out) return strdup("[]");
  out[0] = '[';
  out[1] = 0;
  int first = 1;
  const char *p = models_body;
  while ((p = strstr(p, "\"id\"")) != NULL) {
    p += 4;
    while (*p == ' ' || *p == '\t' || *p == ':' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') continue;
    p++;
    const char *e = p;
    while (*e && *e != '"') {
      if (*e == '\\' && e[1]) e += 2;
      else e++;
    }
    if (*e != '"') break;
    size_t idlen = (size_t)(e - p);
    if (idlen == 0 || idlen > 200) { p = e + 1; continue; }
    /* skip non-model ids like "list" object fields if any */
    if (idlen == 4 && !strncmp(p, "list", 4)) { p = e + 1; continue; }
    size_t need = len + idlen + 8;
    if (need >= cap) {
      cap = need * 2;
      char *n = realloc(out, cap);
      if (!n) break;
      out = n;
    }
    if (!first) out[len++] = ',';
    first = 0;
    out[len++] = '"';
    memcpy(out + len, p, idlen);
    len += idlen;
    out[len++] = '"';
    out[len] = 0;
    p = e + 1;
  }
  if (len + 2 >= cap) {
    char *n = realloc(out, len + 4);
    if (n) out = n;
  }
  out[len++] = ']';
  out[len] = 0;
  return out;
}

void ng_agent_select_model(ng_agent_cfg *c, const char *model) {
  if (!c || !model || !model[0]) return;
  free(c->model);
  c->model = strdup(model);
  ng_agent_save_env(c);
}

/* Parse OpenAI SSE "data: {json}" lines; call on_delta for each content delta.
 * Returns malloc'd full assistant text accumulated. */
static char *consume_sse_stream(FILE *fp, ng_stream_fn on_delta, void *ud) {
  char *acc = strdup("");
  if (!acc) return NULL;
  char line[8192];
  while (fgets(line, sizeof line, fp)) {
    char *s = line;
    while (*s == ' ' || *s == '\t') s++;
    if (strncmp(s, "data:", 5) != 0) continue;
    s += 5;
    while (*s == ' ' || *s == '\t') s++;
    /* strip CR/LF */
    size_t L = strlen(s);
    while (L && (s[L-1] == '\n' || s[L-1] == '\r')) s[--L] = 0;
    if (!L || strcmp(s, "[DONE]") == 0) continue;
    char *piece = ng_json_get_string(s, "content");
    /* prefer delta.content path: look for "delta" then content */
    if (!piece || !piece[0]) {
      free(piece);
      const char *d = strstr(s, "\"delta\"");
      piece = d ? ng_json_get_string(d, "content") : NULL;
    }
    if (piece && piece[0]) {
      size_t pl = strlen(piece);
      if (on_delta) on_delta(ud, piece, pl);
      size_t al = strlen(acc);
      char *n = realloc(acc, al + pl + 1);
      if (!n) { free(piece); return acc; }
      memcpy(n + al, piece, pl + 1);
      acc = n;
    }
    free(piece);
  }
  return acc;
}

/* Streaming POST (curl -N). Used for final text turns only. */
static char *curl_post_json_stream(const char *url, const char *bearer, const char *body,
                                   ng_stream_fn on_delta, void *ud) {
  char tmpl[] = "/tmp/ng_req_XXXXXX";
  int fd = mkstemp(tmpl);
  if (fd < 0) return strdup("mkstemp failed");
  write(fd, body, strlen(body));
  close(fd);

  int pipefd[2];
  if (pipe(pipefd) != 0) { unlink(tmpl); return strdup("pipe failed"); }

  char auth[1600];
  if (bearer && bearer[0])
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", bearer);
  else
    auth[0] = 0;
  char dataarg[80];
  snprintf(dataarg, sizeof dataarg, "@%s", tmpl);
  char ua[96];
  snprintf(ua, sizeof ua, "User-Agent: %s", ng_cli_user_agent());
  char verhdr[80];
  snprintf(verhdr, sizeof verhdr, "x-grok-client-version: %s", ng_cli_version());
  int grok = is_grok_endpoint(url);

  pid_t p2 = fork();
  if (p2 == 0) {
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    close(pipefd[1]);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
    char *argv[48];
    int a = 0;
    argv[a++] = "curl";
    argv[a++] = "-sS";
    argv[a++] = "-N"; /* no buffer */
    argv[a++] = "--max-time";
    argv[a++] = "120";
    argv[a++] = "--connect-timeout";
    argv[a++] = "12";
    argv[a++] = "-H";
    argv[a++] = "Content-Type: application/json";
    argv[a++] = "-H";
    argv[a++] = "Accept: text/event-stream";
    if (bearer && bearer[0]) {
      argv[a++] = "-H";
      argv[a++] = auth;
    }
    if (grok) {
      argv[a++] = "-H";
      argv[a++] = "X-XAI-Token-Auth: " NG_AUTH_TOKEN_HDR;
      argv[a++] = "-H";
      argv[a++] = "x-grok-client-mode: headless";
      argv[a++] = "-H";
      argv[a++] = "x-grok-client-surface: cli";
      argv[a++] = "-H";
      argv[a++] = verhdr;
      argv[a++] = "-H";
      argv[a++] = ua;
    } else {
      argv[a++] = "-H";
      argv[a++] = "User-Agent: nanobot/" NG_VERSION;
    }
    argv[a++] = "--data-binary";
    argv[a++] = dataarg;
    argv[a++] = (char *)url;
    argv[a++] = NULL;
    execvp("curl", argv);
    _exit(127);
  }
  close(pipefd[1]);
  FILE *fp = fdopen(pipefd[0], "r");
  char *acc = NULL;
  if (fp) {
    acc = consume_sse_stream(fp, on_delta, ud);
    fclose(fp);
  } else {
    close(pipefd[0]);
  }
  int st = 0;
  waitpid(p2, &st, 0);
  unlink(tmpl);
  if (!acc || !acc[0]) {
    free(acc);
    /* fallback: empty stream — caller may retry non-stream */
    return NULL;
  }
  return acc;
}

static char *extract_command_arg(const char *args_json) {
  char *c = ng_json_get_string(args_json, "command");
  if (c) return c;
  return strdup(args_json ? args_json : "");
}

/* Safe messages array builder (avoids broken hist embedding). */
static int msg_append(char **msgs, const char *role, const char *content) {
  char *esc = ng_json_escape(content ? content : "");
  if (!esc) return -1;
  char *piece = NULL;
  int need_comma = (*msgs && (*msgs)[0] && (*msgs)[0] != '[');
  /* *msgs holds body without outer [] yet — we store inner parts starting empty */
  if (!*msgs) {
    asprintf(msgs, "{\"role\":\"%s\",\"content\":\"%s\"}", role, esc);
  } else {
    asprintf(&piece, "%s,{\"role\":\"%s\",\"content\":\"%s\"}", *msgs, role, esc);
    free(*msgs);
    *msgs = piece;
  }
  free(esc);
  (void)need_comma;
  return *msgs ? 0 : -1;
}

static char *run_shell_direct(ng_agent_cfg *c, const char *cmd) {
  while (*cmd == ' ' || *cmd == '\t') cmd++;
  if (!cmd[0]) return strdup("usage: @! <shell command>");
  ng_log("agent: @! shell: %.200s", cmd);
  ng_cmd_result cr = ng_run_command(cmd, c->timeout_sec);
  char *out = NULL;
  asprintf(&out, "exit=%d\n%s", cr.exit_code, cr.output ? cr.output : "");
  ng_cmd_result_free(&cr);
  return out ? out : strdup("(no output)");
}

char *ng_agent_run(ng_agent_cfg *c, const char *user_prompt) {
  return ng_agent_run_ex(c, user_prompt, 0, NULL, NULL);
}

char *ng_agent_run_ex(ng_agent_cfg *c, const char *user_prompt,
                     int stream_final, ng_stream_fn on_delta, void *userdata) {
  if (!user_prompt || !user_prompt[0]) return strdup("(empty prompt)");

  /* Direct shell: @! command  (offline, no LLM, works without an LLM) */
  if (user_prompt[0] == '@' && user_prompt[1] == '!') {
    char *r = run_shell_direct(c, user_prompt + 2);
    /* lightweight memory of shell for context */
    if (r) ng_memory_record_exchange(user_prompt, r);
    return r;
  }

  int grok = is_grok_endpoint(c->base_url);
  const char *bearer = NULL;
  if (grok) {
    if (!c->session || ng_session_ensure(c->session) != 0) {
      return strdup("Not signed in. Open the activation link nanobot printed "
                    "(open Connect Grok / activation link in a browser) to attach a session. "
                    "Or use --offline / @! <cmd> without Grok.");
    }
    bearer = ng_session_bearer(c->session);
    if (!bearer) {
      return strdup("No Grok session. Re-run nanobot and open the browser activation link.");
    }
  } else {
    /* llama.cpp / OpenAI: optional API key from env file or env */
    char envpath[640];
    snprintf(envpath, sizeof envpath, "%s/env", ng_workdir());
    char *key = ng_slurp_env_file(envpath, "NANOBOT_API_KEY");
    if (!key) key = ng_slurp_env_file(envpath, "OPENAI_API_KEY");
    if (!key) key = ng_getenv_dup("NANOBOT_API_KEY");
    if (!key) key = ng_getenv_dup("OPENAI_API_KEY");
    bearer = key; /* may be NULL for open local llama.cpp */
    /* leak key intentionally until end — free at return paths is hard; store on stack via static free later */
    (void)key;
  }

  ng_log("agent: user: %.200s", user_prompt);

  /* Compact memory: always-on core identity + recent turns (pruned). */
  char *sys = ng_memory_system_prompt();
  char *inner = NULL;
  if (msg_append(&inner, "system", sys ? sys : "You are nanobot, a tiny standalone agent.") != 0) {
    free(sys);
    return strdup("oom building messages");
  }
  free(sys);

  /* Recent history as discrete user/assistant messages only */
  {
    char path[640];
    snprintf(path, sizeof path, "%s/memory/recent.jsonl", ng_workdir());
    size_t len = 0;
    char *raw = ng_read_file(path, &len);
    if (raw && len) {
      char *p = raw;
      int count = 0;
      while (*p && count < NG_MEM_MAX_RECENT_TURNS * 2) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        if (p[0] == '{') {
          char *role = ng_json_get_string(p, "role");
          char *content = ng_json_get_string(p, "content");
          if (role && content &&
              (strcmp(role, "user") == 0 || strcmp(role, "assistant") == 0) &&
              content[0] &&
              strncmp(content, "API error:", 10) != 0 &&
              strncmp(content, "(no content)", 12) != 0 &&
              strncmp(content, "Failed to parse", 15) != 0) {
            msg_append(&inner, role, content);
            count++;
          }
          free(role); free(content);
        }
        if (!nl) break;
        p = nl + 1;
      }
    }
    free(raw);
  }

  msg_append(&inner, "user", user_prompt);

  char *messages = NULL;
  asprintf(&messages, "[%s]", inner ? inner : "");
  free(inner);
  if (!messages) return strdup("oom messages");

  /* Tools: OpenAI-style. Disable with NANOBOT_TOOLS=0.
   * Concurrent HTTP forks are fine; we always reserve a final no-tools turn. */
  int use_tools = 1;
  {
    const char *t = getenv("NANOBOT_TOOLS");
    if (t && (t[0] == '0' || t[0] == 'n' || t[0] == 'N' || t[0] == 'f' || t[0] == 'F'))
      use_tools = 0;
  }
  /* Short social prompts: answer in text, no tool thrash */
  {
    const char *p = user_prompt;
    while (*p == ' ' || *p == '\t') p++;
    size_t pl = strlen(p);
    if (pl > 0 && pl < 48 && p[0] != '@' && !strchr(p, '\n')) {
      char low[64];
      size_t i;
      for (i = 0; i < pl && i < sizeof low - 1; i++) {
        char c = p[i];
        low[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
      }
      low[i] = 0;
      if (!strcmp(low, "hello") || !strcmp(low, "hi") || !strcmp(low, "hey") ||
          !strcmp(low, "thanks") || !strcmp(low, "thank you") || !strcmp(low, "ping") ||
          !strcmp(low, "ok") || !strcmp(low, "yes") || !strcmp(low, "no") ||
          !strncmp(low, "hello ", 6) || !strncmp(low, "hi ", 3))
        use_tools = 0;
    }
  }

  const char *tools =
    "[{\"type\":\"function\",\"function\":{"
    "\"name\":\"run_terminal_command\","
    "\"description\":\"Run ONE shell command when you need live device data. Prefer short answers without tools for greetings.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"command\":{\"type\":\"string\",\"description\":\"shell command\"}"
    "},\"required\":[\"command\"]}}}]";

  char *final = NULL;
  char *last_tool_out = NULL; /* fallback text if model never speaks */
  int version_retries = 0;
  int max_turns = c->max_turns > 0 ? c->max_turns : ng_max_turns();
  if (max_turns < 2) max_turns = 2;

  for (int turn = 0; turn < max_turns; turn++) {
    if (grok) {
      if (ng_session_ensure(c->session) != 0) {
        free(messages); free(last_tool_out);
        return strdup("Grok session expired. Restart nanobot and re-activate in the browser.");
      }
      bearer = ng_session_bearer(c->session);
    }

    /* Last turn always no tools so the model must produce a final answer. */
    int tools_now = use_tools && (turn < max_turns - 1);
    char *body = NULL;
    if (tools_now) {
      asprintf(&body,
        "{\"model\":\"%s\",\"messages\":%s,\"tools\":%s,\"tool_choice\":\"auto\","
        "\"stream\":false}",
        c->model, messages, tools);
    } else {
      /* Nudge finalization after tools */
      if (last_tool_out && turn == max_turns - 1) {
        size_t ml = strlen(messages);
        if (ml && messages[ml - 1] == ']') messages[ml - 1] = 0;
        char *nudge = NULL;
        asprintf(&nudge,
          "%s,{\"role\":\"user\",\"content\":\"Using the tool results above, reply to the user now in plain text. No more tools.\"}]",
          messages);
        free(messages);
        messages = nudge;
      }
      int want_stream = stream_final && on_delta;
      asprintf(&body,
        "{\"model\":\"%s\",\"messages\":%s,\"stream\":%s}",
        c->model, messages, want_stream ? "true" : "false");
    }

    char url[512];
    snprintf(url, sizeof url, "%s/chat/completions", c->base_url);

    int want_stream = stream_final && !tools_now && on_delta;
    ng_log("agent: turn %d/%d POST %s tools=%d stream=%d",
           turn + 1, max_turns, url, tools_now, want_stream);

    char *resp = NULL;
    if (want_stream) {
      char *streamed = curl_post_json_stream(url, bearer, body, on_delta, userdata);
      free(body);
      body = NULL;
      if (streamed && streamed[0]) {
        final = streamed;
        free(messages);
        free(last_tool_out);
        if (final && strncmp(final, "API error:", 10) != 0) {
          ng_memory_record_exchange(user_prompt, final);
        }
        ng_log("agent: final (streamed): %.300s", final);
        return final;
      }
      free(streamed);
      asprintf(&body,
        "{\"model\":\"%s\",\"messages\":%s,\"stream\":false}",
        c->model, messages);
      ng_log("agent: stream empty — fallback non-stream");
      resp = curl_post_json(url, bearer, body);
      free(body);
      body = NULL;
    } else {
      resp = curl_post_json(url, bearer, body);
      free(body);
      body = NULL;
    }

    if (!resp) {
      free(messages); free(last_tool_out);
      return strdup("no response from API");
    }

    if (strstr(resp, "\"error\"") || strstr(resp, "Failed to parse")) {
      if (grok && version_retries < 3 && ng_cli_version_handle_error(resp)) {
        version_retries++;
        free(resp);
        turn--;
        continue;
      }
      if (tools_now && (strstr(resp, "tool") || strstr(resp, "tools") ||
                        strstr(resp, "unknown field") || strstr(resp, "not supported"))) {
        ng_log("agent: tools rejected — retry without tools");
        use_tools = 0;
        free(resp);
        turn--;
        continue;
      }
      char *em = ng_json_get_string(resp, "message");
      if (!em) em = ng_json_get_string(resp, "error");
      char *out = NULL;
      asprintf(&out, "API error: %s\n%.600s", em ? em : "?", resp);
      free(em); free(resp); free(messages); free(last_tool_out);
      return out;
    }

    char *tname = NULL, *targs = NULL, *tid = NULL;
    if (tools_now && ng_json_first_tool_call(resp, &tname, &targs, &tid)) {
      ng_log("agent: tool %s id=%s args=%.200s", tname, tid, targs);
      char *cmd = NULL;
      if (strcmp(tname, "run_terminal_command") == 0 ||
          strcmp(tname, "run_terminal_cmd") == 0 ||
          strcmp(tname, "shell") == 0 ||
          strcmp(tname, "bash") == 0) {
        cmd = extract_command_arg(targs);
      } else {
        asprintf(&cmd, "echo 'unknown tool %s'", tname);
      }
      ng_cmd_result cr = ng_run_command(cmd, c->timeout_sec);
      ng_log("agent: exit=%d out=%.200s", cr.exit_code, cr.output ? cr.output : "");

      free(last_tool_out);
      asprintf(&last_tool_out, "exit=%d\n%s", cr.exit_code, cr.output ? cr.output : "");
      char *esc_out = ng_json_escape(last_tool_out);
      char *esc_args = ng_json_escape(targs);

      size_t ml = strlen(messages);
      if (ml && messages[ml - 1] == ']') messages[ml - 1] = 0;
      char *new_messages = NULL;
      asprintf(&new_messages,
        "%s,{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{\"id\":\"%s\","
        "\"type\":\"function\",\"function\":{\"name\":\"%s\",\"arguments\":\"%s\"}}]},"
        "{\"role\":\"tool\",\"tool_call_id\":\"%s\",\"content\":\"%s\"}]",
        messages, tid, tname, esc_args, tid, esc_out);
      free(messages);
      messages = new_messages;
      free(esc_args); free(esc_out);
      free(tname); free(targs); free(tid); free(cmd);
      ng_cmd_result_free(&cr);
      free(resp);
      continue;
    }

    final = ng_json_message_content(resp);
    if (final && final[0]) {
      free(resp);
      break;
    }
    free(final);
    final = NULL;
    free(resp);
    /* empty content with no tools — force next iteration without tools */
    use_tools = 0;
  }

  /* Always produce a user-visible final answer */
  if (!final || !final[0]) {
    free(final);
    if (last_tool_out && last_tool_out[0]) {
      asprintf(&final,
               "%s",
               last_tool_out);
      /* trim huge tool dumps */
      if (final && strlen(final) > 1200) {
        final[1200] = 0;
        char *f2 = NULL;
        asprintf(&f2, "%s…", final);
        free(final);
        final = f2;
      }
      ng_log("agent: final from last tool output (model never wrote text)");
    } else {
      final = strdup("(no reply from model — try again or @! shell)");
    }
  }

  free(messages);
  free(last_tool_out);
  if (final && strncmp(final, "API error:", 10) != 0 &&
      strncmp(final, "(no content)", 12) != 0 &&
      strncmp(final, "curl failed", 11) != 0 &&
      strncmp(final, "(no reply", 9) != 0) {
    ng_memory_record_exchange(user_prompt, final);
  }
  ng_log("agent: final: %.300s", final);
  return final;
}
