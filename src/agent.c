#include "agent.h"
#include "memory.h"
#include "mcp_remote.h"
#include "shell.h"
#include "task.h"
#include "util.h"
#include "provider.h"
#include "ng_sched.h"
#include "subagent.h"
#include "braincell.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>

/* Nested subagent workers: no further spawning (keep robot light). */
static char *subagent_run_adapter(void *cfg, const char *prompt) {
  setenv("NANOBOT_SUBAGENT", "1", 1);
  return ng_agent_run((ng_agent_cfg *)cfg, prompt);
}

void ng_agent_apply_provider_policy(ng_agent_cfg *c) {
  ng_provider_policy pol;
  ng_provider_policy_defaults(&pol, ng_agent_backend_kind(c));
  ng_provider_policy_load_settings(&pol);
  ng_llm_sched_set_enabled(pol.llm_serial);
  /* Optional concurrency (llama parallel slots). Shared lock path by default. */
  {
    char *slots = ng_settings_get("LLM_SLOTS");
    if (slots) {
      ng_llm_sched_set_slots(atoi(slots));
      free(slots);
    }
    char *lp = ng_settings_get("LLM_LOCK");
    if (lp) {
      ng_llm_sched_set_path(lp);
      free(lp);
    }
  }
  ng_subagent_configure(pol.subagents_enabled, pol.subagents_max);
  /* Cap down to provider default only when host did not set NANOBOT_MAX_TURNS */
  if (c && pol.max_turns > 0 && !getenv("NANOBOT_MAX_TURNS")) {
    if (c->max_turns > pol.max_turns)
      c->max_turns = pol.max_turns;
  }
  if (c) {
    if (c->timeout_sec <= 0 || c->timeout_sec == NG_CMD_TIMEOUT_SEC)
      c->timeout_sec = ng_cmd_timeout_sec();
    if (c->max_turns <= 0)
      c->max_turns = ng_max_turns();
  }
}

char *ng_agent_subagent_spawn(ng_agent_cfg *c, const char *type, const char *desc,
                              const char *prompt) {
  ng_agent_apply_provider_policy(c);
  if (!ng_subagent_enabled()) return NULL;
  return ng_subagent_spawn(c, subagent_run_adapter, type, desc, prompt);
}

void ng_agent_cfg_init(ng_agent_cfg *c) {
  memset(c, 0, sizeof *c);
  c->base_url = strdup(NG_DEFAULT_BASE);
  c->model = strdup(NG_DEFAULT_MODEL);
  c->max_turns = ng_max_turns();
  c->timeout_sec = ng_cmd_timeout_sec();
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
static char *curl_post_json_unlocked(const char *url, const char *bearer, const char *body);

/* Dual-wire transport deny — machine error token only (no free-text curl essays). */
static char *transport_err(const char *error) {
  char *out = NULL;
  const char *e = error && error[0] ? error : "api_transport";
  asprintf(&out,
           "{\"schema\":\"nanobot.transport.v1\",\"ok\":false,"
           "\"error\":\"%s\",\"python\":0}",
           e);
  return out ? out
             : strdup("{\"schema\":\"nanobot.transport.v1\",\"ok\":false,"
                      "\"error\":\"oom\",\"python\":0}");
}

typedef struct { const char *url; const char *bearer; const char *body; } curl_job_t;
static char *curl_post_json_job(void *ud) {
  curl_job_t *j = (curl_job_t *)ud;
  return curl_post_json_unlocked(j->url, j->bearer, j->body);
}

static char *curl_post_json(const char *url, const char *bearer, const char *body) {
  curl_job_t j = { url, bearer, body };
  return ng_llm_sched_run(curl_post_json_job, &j);
}

static char *curl_post_json_unlocked(const char *url, const char *bearer, const char *body) {
  char tmpl[640], outtmpl[640], errtmpl[640];
  int fd = ng_mkstemp_home(tmpl, sizeof tmpl, "ng_req_");
  if (fd < 0) return transport_err("mkstemp");
  write(fd, body, strlen(body));
  close(fd);

  int ofd = ng_mkstemp_home(outtmpl, sizeof outtmpl, "ng_resp_");
  if (ofd < 0) { unlink(tmpl); return transport_err("mkstemp_out"); }
  close(ofd);

  int efd = ng_mkstemp_home(errtmpl, sizeof errtmpl, "ng_cerr_");
  if (efd < 0) {
    unlink(tmpl);
    unlink(outtmpl);
    return transport_err("mkstemp_err");
  }
  close(efd);

  char auth[1600];
  if (bearer && bearer[0])
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", bearer);
  else
    snprintf(auth, sizeof auth, "Authorization: Bearer none");

  char dataarg[700];
  snprintf(dataarg, sizeof dataarg, "@%s", tmpl);
  char ua[96];
  snprintf(ua, sizeof ua, "User-Agent: %s", ng_cli_user_agent());
  char verhdr[80];
  snprintf(verhdr, sizeof verhdr, "x-grok-client-version: %s", ng_cli_version());
  int grok = is_grok_endpoint(url);

  pid_t p2 = fork();
  if (p2 < 0) {
    unlink(tmpl); unlink(outtmpl); unlink(errtmpl);
    ng_log("agent: curl fork failed: %s", strerror(errno));
    return transport_err("curl_fork");
  }
  if (p2 == 0) {
    int er = open(errtmpl, O_WRONLY | O_TRUNC);
    if (er >= 0) { dup2(er, STDERR_FILENO); close(er); }
    char *argv[40];
    int a = 0;
    argv[a++] = "curl";
    argv[a++] = "-sS";
    argv[a++] = "--max-time";
    argv[a++] = (char *)ng_http_timeout_arg();
    argv[a++] = "--connect-timeout";
    argv[a++] = "30";
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
  if (waitpid(p2, &st, 0) < 0) {
    unlink(tmpl); unlink(outtmpl); unlink(errtmpl);
    return transport_err("curl_waitpid");
  }
  unlink(tmpl);
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
    char *err = ng_read_file(outtmpl, NULL);
    char *cerr = ng_read_file(errtmpl, NULL);
    unlink(outtmpl);
    unlink(errtmpl);
    if (err && err[0]) { free(cerr); return err; }
    free(err);
    /* Log curl stderr free-text; never surface it on the product wire. */
    if (cerr && cerr[0])
      ng_log("agent: curl stderr: %.200s", cerr);
    free(cerr);
    return transport_err("curl_failed");
  }
  unlink(errtmpl);
  char *resp = ng_read_file(outtmpl, NULL);
  unlink(outtmpl);
  return resp ? resp : strdup("");
}



/* GET with same auth headers as chat (session or API key). */
static char *curl_get_url(const char *url, const char *bearer, int grok_headers) {
  char outtmpl[640], errtmpl[640];
  int ofd = ng_mkstemp_home(outtmpl, sizeof outtmpl, "ng_get_");
  if (ofd < 0) return transport_err("mkstemp_out");
  close(ofd);
  int efd = ng_mkstemp_home(errtmpl, sizeof errtmpl, "ng_gerr_");
  if (efd < 0) {
    unlink(outtmpl);
    return transport_err("mkstemp_err");
  }
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
  if (p2 < 0) {
    unlink(outtmpl); unlink(errtmpl);
    return transport_err("curl_fork");
  }
  if (p2 == 0) {
    int er = open(errtmpl, O_WRONLY | O_TRUNC);
    if (er >= 0) { dup2(er, STDERR_FILENO); close(er); }
    char *argv[40];
    int a = 0;
    argv[a++] = "curl";
    argv[a++] = "-sS";
    argv[a++] = "--max-time";
    argv[a++] = (char *)ng_http_timeout_arg();
    argv[a++] = "--connect-timeout";
    argv[a++] = "30";
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
  if (waitpid(p2, &st, 0) < 0) {
    unlink(outtmpl); unlink(errtmpl);
    return transport_err("curl_waitpid");
  }
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
    char *err = ng_read_file(outtmpl, NULL);
    char *cerr = ng_read_file(errtmpl, NULL);
    unlink(outtmpl); unlink(errtmpl);
    if (err && err[0]) { free(cerr); return err; }
    free(err);
    if (cerr && cerr[0])
      ng_log("agent: models curl stderr: %.200s", cerr);
    free(cerr);
    return transport_err("curl_failed");
  }
  unlink(errtmpl);
  char *resp = ng_read_file(outtmpl, NULL);
  unlink(outtmpl);
  return resp ? resp : strdup("");
}

char *ng_agent_fetch_models_json(ng_agent_cfg *c) {
  if (!c || !c->base_url || !c->base_url[0])
    return transport_err("no_base_url");
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
    /* skip non-model ids: OpenAI object type, and Grok *reasoning effort*
     * levels (high/medium/low) that appear as nested "id" in /v1/models. */
    if (idlen == 4 && !strncmp(p, "list", 4)) { p = e + 1; continue; }
    if (idlen == 4 && (!strncmp(p, "high", 4) || !strncmp(p, "auto", 4) ||
                       !strncmp(p, "none", 4))) { p = e + 1; continue; }
    if (idlen == 3 && !strncmp(p, "low", 3)) { p = e + 1; continue; }
    if (idlen == 6 && !strncmp(p, "medium", 6)) { p = e + 1; continue; }
    if (idlen == 5 && !strncmp(p, "model", 5)) { p = e + 1; continue; }
    if (idlen == 6 && !strncmp(p, "object", 6)) { p = e + 1; continue; }
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

/* Emit structured SSE event for clients (tool / thinking). Prefix 0x1e for http layer. */
static void stream_evt(ng_stream_fn on_delta, void *ud, const char *json) {
  if (!on_delta || !json || !json[0]) return;
  size_t n = strlen(json);
  char *buf = (char *)malloc(n + 2);
  if (!buf) return;
  buf[0] = (char)0x1e;
  memcpy(buf + 1, json, n + 1);
  on_delta(ud, buf, n + 1);
  free(buf);
}

/* Parse OpenAI SSE "data: {json}" lines; call on_delta for each content delta.
 * Also surfaces reasoning/thinking as structured events for spoiler UI.
 * Returns malloc'd full assistant text accumulated.
 *
 * Anti-spam: many local servers re-send *cumulative* content each event
 * (full text so far). Prefer delta.content; if a piece is a prefix extension
 * of the accumulator, only emit/append the new suffix. Cap length/events.
 */
/* True if tail of buf (len bl) is a near-repeat of an earlier window (stutter). */
static int stream_stutter_tail(const char *buf, size_t bl) {
  size_t w, i, hits;
  if (!buf || bl < 96) return 0;
  /* Window = last ~1/3 of text, clamped */
  w = bl / 3;
  if (w < 48) w = 48;
  if (w > 240) w = 240;
  if (bl < w * 2) return 0;
  /* Count how many times the final window appears in the whole buffer */
  hits = 0;
  for (i = 0; i + w <= bl; i++) {
    if (memcmp(buf + i, buf + (bl - w), w) == 0) {
      hits++;
      if (hits >= 3) return 1;
      i += w / 2; /* stride */
    }
  }
  /* Also: second half equals first half (pure A+A) */
  if (bl >= 120 && (bl % 2) == 0 &&
      memcmp(buf, buf + bl / 2, bl / 2) == 0)
    return 1;
  if (bl >= 120 && bl % 2 == 1 &&
      memcmp(buf, buf + bl / 2, bl / 2) == 0)
    return 1;
  return 0;
}

/* Drop leaked thinking blocks / lone close tags from a stream piece. */
static void stream_strip_think_inplace(char *s) {
  static const char *tags[] = {
      "think", "thinking", "reasoning", "redacted_reasoning",
      "redacted_thinking", NULL};
  int i;
  char *p;
  if (!s || !s[0]) return;
  for (i = 0; tags[i]; i++) {
    char open[48], close[48];
    char *a;
    snprintf(open, sizeof open, "<%s>", tags[i]);
    snprintf(close, sizeof close, "</%s>", tags[i]);
    a = strstr(s, open);
    while (a) {
      char *b = strstr(a, close);
      if (!b) {
        *a = 0; /* unclosed — drop rest (anti think-leak spam) */
        break;
      }
      b += strlen(close);
      memmove(a, b, strlen(b) + 1);
      a = strstr(s, open);
    }
    /* Lone close tag: keep only text after last close (model restarted answer) */
    while ((p = strstr(s, close)) != NULL) {
      char *after = p + strlen(close);
      while (*after == '\n' || *after == '\r' || *after == ' ') after++;
      memmove(s, after, strlen(after) + 1);
    }
  }
}

/* Collapse A+A+A… repeated blocks in final streamed text. */
static char *stream_dedupe_final(char *acc) {
  size_t n, w, best_w = 0;
  if (!acc) return acc;
  stream_strip_think_inplace(acc);
  n = strlen(acc);
  if (n < 96) return acc;
  /* Find largest w where text is k>=2 copies of prefix length w */
  for (w = n / 2; w >= 40; w--) {
    size_t k = 0, pos = 0;
    if (n < w * 2) continue;
    while (pos + w <= n && memcmp(acc + pos, acc, w) == 0) {
      k++;
      pos += w;
      /* allow one newline between copies */
      if (pos < n && (acc[pos] == '\n' || acc[pos] == '\r')) {
        while (pos < n && (acc[pos] == '\n' || acc[pos] == '\r')) pos++;
      }
    }
    if (k >= 2 && pos >= n - 2) {
      best_w = w;
      break;
    }
  }
  if (best_w > 0) {
    acc[best_w] = 0;
    ng_log("agent: stream dedupe collapsed to %zu bytes (was %zu)", best_w, n);
  } else if (stream_stutter_tail(acc, n)) {
    /* Keep first ~half when stutter detected */
    size_t keep = n / 2;
    while (keep > 40 && acc[keep] && acc[keep] != '\n') keep--;
    if (keep < 40) keep = n / 2;
    acc[keep] = 0;
    ng_log("agent: stream stutter trim to %zu bytes", keep);
  }
  return acc;
}

static char *consume_sse_stream(FILE *fp, ng_stream_fn on_delta, void *ud) {
  char *acc = strdup("");
  size_t acc_len = 0;
  size_t n_events = 0;
  size_t n_emits = 0;
  int spam_stop = 0;
  const size_t MAX_ACC = 256 * 1024;
  /* Hard caps: runaway proxies / model loops must not spam UI or pin llama. */
  const size_t MAX_EVENTS = 8000;
  const size_t MAX_EMITS = 4000;
  if (!acc) return NULL;
  char line[8192];
  while (fgets(line, sizeof line, fp)) {
    char *s = line;
    char *piece = NULL;
    int is_snapshot = 0;
    size_t pl, al;
    const char *emit = NULL;

    if (spam_stop) {
      /* Drain remainder of SSE without emitting — free llama sooner. */
      continue;
    }
    if (++n_events > MAX_EVENTS) {
      ng_log("agent: stream event cap %zu — stop (anti-spam)", MAX_EVENTS);
      break;
    }
    while (*s == ' ' || *s == '\t') s++;
    if (strncmp(s, "data:", 5) != 0) continue;
    s += 5;
    while (*s == ' ' || *s == '\t') s++;
    /* strip CR/LF */
    {
      size_t L = strlen(s);
      while (L && (s[L - 1] == '\n' || s[L - 1] == '\r')) s[--L] = 0;
      if (!L || strcmp(s, "[DONE]") == 0) continue;
    }

    /* Grok / OpenAI reasoning in delta */
    {
      const char *d = strstr(s, "\"delta\"");
      char *think = NULL;
      if (d) {
        think = ng_json_get_string(d, "reasoning_content");
        if (!think || !think[0]) {
          free(think);
          think = ng_json_get_string(d, "reasoning");
        }
        if (!think || !think[0]) {
          free(think);
          think = ng_json_get_string(d, "thinking");
        }
      }
      if (!think || !think[0]) {
        free(think);
        think = ng_json_get_string(s, "reasoning_content");
      }
      /* Thinking spam: only emit structured events when NANOBOT_SHOW_THINKING=1.
       * Default: suppress entirely (models still reason server-side). */
      if (think && think[0]) {
        const char *st = getenv("NANOBOT_SHOW_THINKING");
        int show = st && (st[0] == '1' || st[0] == 'y' || st[0] == 'Y' ||
                          st[0] == 't');
        if (show) {
          char *esc = ng_json_escape(think);
          char *ev = NULL;
          if (esc &&
              asprintf(&ev, "{\"type\":\"thinking\",\"text\":\"%s\"}", esc) > 0 &&
              ev)
            stream_evt(on_delta, ud, ev);
          free(esc);
          free(ev);
        }
      }
      free(think);
    }

    /* Prefer delta.content — first "content" key is often message.content
     * (full cumulative snapshot) and would re-append the whole reply. */
    {
      const char *d = strstr(s, "\"delta\"");
      if (d) piece = ng_json_get_string(d, "content");
      if (!piece || !piece[0]) {
        free(piece);
        piece = NULL;
        {
          const char *m = strstr(s, "\"message\"");
          if (m) piece = ng_json_get_string(m, "content");
          if (piece && piece[0]) is_snapshot = 1;
        }
      }
      if (!piece || !piece[0]) {
        free(piece);
        piece = ng_json_get_string(s, "content");
        if (piece && piece[0]) is_snapshot = 1; /* treat as snapshot when not under delta */
      }
    }

    /* Strip thinking tags leaking into content (paired + lone close). */
    if (piece && piece[0]) stream_strip_think_inplace(piece);

    if (!piece) continue;
    pl = strlen(piece);
    if (pl == 0) {
      free(piece);
      continue;
    }
    al = acc_len;

    /* Exact full re-send of what we already have — skip. */
    if (al > 0 && pl == al && memcmp(piece, acc, pl) == 0) {
      free(piece);
      continue;
    }
    /* Cumulative snapshot / cumulative "delta": only take the new suffix. */
    if ((is_snapshot || (al > 0 && pl > al)) && al > 0 &&
        memcmp(piece, acc, al) == 0) {
      emit = piece + al;
      pl = strlen(emit);
      if (pl == 0) {
        free(piece);
        continue;
      }
    } else if (al > 0 && pl < al && memcmp(acc + (al - pl), piece, pl) == 0) {
      /* Piece is exactly the last pl bytes already appended — skip. */
      free(piece);
      continue;
    } else if (al > 0 && pl > 0 && pl <= al &&
               memcmp(acc, piece, pl) == 0) {
      /* Piece is a strict prefix of what we already have — skip. */
      free(piece);
      continue;
    } else if (is_snapshot && al > 0) {
      /* Snapshot that is not a prefix extension of acc — skip. */
      free(piece);
      continue;
    } else if (al > 0 && pl > 48) {
      /* Large non-token "delta" after we already have text: only accept a
       * true cumulative extension; otherwise it re-appends the whole reply
       * (main UI spam source with some local models / proxies). */
      if (pl > al && memcmp(piece, acc, al) == 0) {
        emit = piece + al;
        pl = strlen(emit);
        if (pl == 0) {
          free(piece);
          continue;
        }
      } else {
        free(piece);
        continue;
      }
    } else {
      emit = piece;
    }
    if (!emit || pl == 0) {
      free(piece);
      continue;
    }

    /* Already-have: emit is a substring we already streamed. */
    if (pl >= 24 && al >= pl) {
      size_t i;
      int found = 0;
      for (i = 0; i + pl <= al; i++) {
        if (memcmp(acc + i, emit, pl) == 0) {
          found = 1;
          break;
        }
      }
      if (found) {
        free(piece);
        continue;
      }
    }

    if (acc_len + pl > MAX_ACC) {
      ng_log("agent: stream size cap %zu — stop (anti-spam)", MAX_ACC);
      free(piece);
      break;
    }
    if (++n_emits > MAX_EMITS) {
      ng_log("agent: stream emit cap %zu — stop (anti-spam)", MAX_EMITS);
      free(piece);
      break;
    }
    if (on_delta) on_delta(ud, emit, pl);
    {
      char *n = realloc(acc, acc_len + pl + 1);
      if (!n) {
        free(piece);
        return stream_dedupe_final(acc);
      }
      memcpy(n + acc_len, emit, pl + 1);
      acc = n;
      acc_len += pl;
    }
    free(piece);
    /* Model loop / re-greeting: stop emitting further (drain SSE). */
    if (stream_stutter_tail(acc, acc_len)) {
      ng_log("agent: stream stutter detected @%zu — stop emit (anti-spam)",
             acc_len);
      spam_stop = 1;
    }
  }
  return stream_dedupe_final(acc);
}

/* Streaming POST (curl -N). Holds LLM sched lock for the whole stream so
 * fleet/hub peers cannot stampede llama while tokens are in flight. */
static char *curl_post_json_stream(const char *url, const char *bearer, const char *body,
                                   ng_stream_fn on_delta, void *ud) {
  ng_llm_sched_acquire();

  char tmpl[640];
  int fd = ng_mkstemp_home(tmpl, sizeof tmpl, "ng_req_");
  if (fd < 0) {
    ng_llm_sched_release();
    return transport_err("mkstemp");
  }
  write(fd, body, strlen(body));
  close(fd);

  int pipefd[2];
  if (pipe(pipefd) != 0) {
    unlink(tmpl);
    ng_llm_sched_release();
    return transport_err("pipe_failed");
  }

  char auth[1600];
  if (bearer && bearer[0])
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", bearer);
  else
    auth[0] = 0;
  char dataarg[700];
  snprintf(dataarg, sizeof dataarg, "@%s", tmpl);
  char ua[96];
  snprintf(ua, sizeof ua, "User-Agent: %s", ng_cli_user_agent());
  char verhdr[80];
  snprintf(verhdr, sizeof verhdr, "x-grok-client-version: %s", ng_cli_version());
  int grok = is_grok_endpoint(url);

  pid_t p2 = fork();
  if (p2 < 0) {
    close(pipefd[0]); close(pipefd[1]); unlink(tmpl);
    ng_llm_sched_release();
    return transport_err("curl_fork");
  }
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
    argv[a++] = (char *)ng_http_timeout_arg();
    argv[a++] = "--connect-timeout";
    argv[a++] = "30";
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
  if (waitpid(p2, &st, 0) < 0) {
    unlink(tmpl);
    free(acc);
    ng_llm_sched_release();
    return NULL;
  }
  unlink(tmpl);
  ng_llm_sched_release();
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

/*
 * Cosplay tool_call: model dumps XML / Grok-Build markup into *content*
 * instead of OpenAI tool_calls JSON. That path never executes — we detect
 * and promote to a real shell tool when possible.
 *
 * Examples:
 *   <tool_name>run_terminal_command</tool_name>
 *   <parameter_name>command</parameter_name>
 *   <parameter_value>reboot</parameter_value>
 *   invoke tool run_terminal_command with command is reboot
 */
static int content_looks_like_tool_cosplay(const char *s) {
  if (!s || !s[0]) return 0;
  if (strstr(s, "<tool_call") || strstr(s, "</tool_call>") ||
      strstr(s, "<tool_name>") || strstr(s, "tool_name>") ||
      strstr(s, "run_terminal_command") || strstr(s, "run_terminal_cmd") ||
      strstr(s, "parameter_value") || strstr(s, "parameter_name"))
    return 1;
  return 0;
}

/* Extract between <tag>...</tag> (first match). Caller frees. */
static char *xml_tag_text(const char *s, const char *tag) {
  if (!s || !tag) return NULL;
  char open[96], close[96];
  snprintf(open, sizeof open, "<%s>", tag);
  snprintf(close, sizeof close, "</%s>", tag);
  const char *a = strstr(s, open);
  if (!a) return NULL;
  a += strlen(open);
  const char *b = strstr(a, close);
  if (!b || b <= a) return NULL;
  size_t n = (size_t)(b - a);
  char *out = malloc(n + 1);
  if (!out) return NULL;
  memcpy(out, a, n);
  out[n] = 0;
  /* trim */
  while (n && (out[n - 1] == ' ' || out[n - 1] == '\n' || out[n - 1] == '\r' ||
               out[n - 1] == '\t'))
    out[--n] = 0;
  char *p = out;
  while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
  if (p != out) memmove(out, p, strlen(p) + 1);
  return out;
}

/* Returns 1 if cosplay tool found; *name_out / *cmd_out malloc'd. */
static int parse_content_tool_cosplay(const char *s, char **name_out, char **cmd_out) {
  if (name_out) *name_out = NULL;
  if (cmd_out) *cmd_out = NULL;
  if (!content_looks_like_tool_cosplay(s)) return 0;
  char *name = xml_tag_text(s, "tool_name");
  if (!name) {
    if (strstr(s, "run_terminal_command") || strstr(s, "run_terminal_cmd"))
      name = strdup("run_terminal_command");
    else if (strstr(s, "\"shell\"") || strstr(s, " tool shell"))
      name = strdup("shell");
  }
  char *cmd = xml_tag_text(s, "parameter_value");
  if (!cmd) {
    /* parameter_name=command then value on next tag already tried;
     * also: with command is X / command="X" */
    const char *p = strstr(s, "command is ");
    if (p) {
      p += 11;
      while (*p == ' ' || *p == '`' || *p == '"' || *p == '\'') p++;
      const char *e = p;
      while (*e && *e != '\n' && *e != '`' && *e != '"' && *e != '\'' &&
             *e != '<' && *e != ']')
        e++;
      if (e > p) {
        cmd = malloc((size_t)(e - p) + 1);
        if (cmd) {
          memcpy(cmd, p, (size_t)(e - p));
          cmd[e - p] = 0;
        }
      }
    }
  }
  if (!cmd) {
    const char *p = strstr(s, "command\":\"");
    if (p) {
      p += 10;
      const char *e = strchr(p, '"');
      if (e && e > p) {
        cmd = malloc((size_t)(e - p) + 1);
        if (cmd) {
          memcpy(cmd, p, (size_t)(e - p));
          cmd[e - p] = 0;
        }
      }
    }
  }
  if (!name || !cmd || !cmd[0]) {
    free(name);
    free(cmd);
    return 0;
  }
  if (name_out) *name_out = name;
  else free(name);
  if (cmd_out) *cmd_out = cmd;
  else free(cmd);
  return 1;
}

/* Strip cosplay markup for user-visible text (never show raw tool XML). */
static char *strip_tool_cosplay_markup(const char *s) {
  if (!s) return strdup("");
  if (!content_looks_like_tool_cosplay(s)) return strdup(s);
  /* Drop from first tool marker to end of block; keep any leading prose. */
  const char *cut = strstr(s, "<tool_call");
  if (!cut) cut = strstr(s, "<tool_name>");
  if (!cut) cut = strstr(s, "run_terminal_command");
  if (!cut) cut = strstr(s, "run_terminal_cmd");
  if (!cut) return strdup("(model emitted tool cosplay — stripped; tool was not called)");
  size_t n = (size_t)(cut - s);
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\n' || s[n - 1] == '\r')) n--;
  char *out = malloc(n + 1);
  if (!out) return strdup("");
  memcpy(out, s, n);
  out[n] = 0;
  if (!out[0]) {
    free(out);
    return strdup("(tool cosplay stripped — was not a real tool_call)");
  }
  return out;
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

/* Strip whitespace from base64 into malloc'd buffer; NULL on OOM. */
static char *b64_clean(const char *b64, size_t *out_len) {
  if (!b64) return NULL;
  size_t blen = strlen(b64);
  char *clean = malloc(blen + 1);
  if (!clean) return NULL;
  size_t j = 0;
  for (size_t i = 0; i < blen; i++) {
    char c = b64[i];
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
    clean[j++] = c;
  }
  clean[j] = 0;
  if (out_len) *out_len = j;
  return clean;
}

/* OpenAI/Grok vision: content array = text + N image_url parts.
 * images_json: [{"base64":"...","mime":"image/jpeg"}, ...] optional.
 * Falls back to single b64/mime when images_json is NULL. */
static int msg_append_user_vision(char **msgs, const char *text,
                                  const char *b64, const char *mime,
                                  const char *images_json) {
  /* Collect up to 4 images */
  char *cleans[4] = {0};
  char mimes[4][40];
  int nimg = 0;
  size_t total_b64 = 0;

  if (images_json && images_json[0] == '[') {
    const char *p = images_json;
    while (*p && nimg < 4) {
      const char *bkey = strstr(p, "\"base64\"");
      if (!bkey) break;
      char *one = ng_json_get_string(bkey - 1 > images_json ? bkey : images_json, "base64");
      /* search near bkey */
      free(one);
      one = NULL;
      {
        const char *q = strchr(bkey, ':');
        if (q) {
          while (*q && *q != '"') q++;
          if (*q == '"') {
            q++;
            const char *e = q;
            while (*e && *e != '"') {
              if (*e == '\\' && e[1]) e += 2;
              else e++;
            }
            size_t n = (size_t)(e - q);
            one = malloc(n + 1);
            if (one) {
              memcpy(one, q, n);
              one[n] = 0;
            }
            p = e;
          }
        }
      }
      char *mm = NULL;
      const char *mkey = strstr(bkey > images_json + 20 ? bkey - 80 : images_json, "\"mime\"");
      if (mkey && mkey < bkey + 200) mm = ng_json_get_string(mkey, "mime");
      if (!mm) {
        /* try after base64 */
        mkey = strstr(bkey, "\"mime\"");
        if (mkey) mm = ng_json_get_string(mkey, "mime");
      }
      if (one && one[0]) {
        size_t cl = 0;
        char *c = b64_clean(one, &cl);
        free(one);
        if (c && cl > 0 && total_b64 + cl < 3500000) {
          cleans[nimg] = c;
          snprintf(mimes[nimg], sizeof mimes[nimg], "%s",
                   (mm && !strncmp(mm, "image/", 6)) ? mm : "image/jpeg");
          total_b64 += cl;
          nimg++;
        } else {
          free(c);
        }
      } else {
        free(one);
      }
      free(mm);
      if (p && *p) p++;
      else break;
    }
  }

  if (nimg == 0 && b64 && b64[0]) {
    size_t cl = 0;
    char *c = b64_clean(b64, &cl);
    if (c && cl > 0) {
      if (cl > 2500000) {
        free(c);
        return -2;
      }
      cleans[0] = c;
      snprintf(mimes[0], sizeof mimes[0], "%s",
               (mime && !strncmp(mime, "image/", 6)) ? mime : "image/jpeg");
      nimg = 1;
    } else {
      free(c);
    }
  }

  if (nimg == 0) return msg_append(msgs, "user", text);

  char *esc = ng_json_escape(text && text[0] ? text : "What is in the attached image(s)/file(s)?");
  if (!esc) {
    for (int i = 0; i < nimg; i++) free(cleans[i]);
    return -1;
  }

  /* Build content array JSON */
  char *content = NULL;
  asprintf(&content, "[{\"type\":\"text\",\"text\":\"%s\"}", esc);
  free(esc);
  if (!content) {
    for (int i = 0; i < nimg; i++) free(cleans[i]);
    return -1;
  }
  for (int i = 0; i < nimg; i++) {
    char *next = NULL;
    asprintf(&next,
      "%s,{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:%s;base64,%s\"}}",
      content, mimes[i], cleans[i]);
    free(content);
    free(cleans[i]);
    cleans[i] = NULL;
    content = next;
    if (!content) {
      for (int k = i + 1; k < nimg; k++) free(cleans[k]);
      return -1;
    }
  }
  {
    char *closed = NULL;
    asprintf(&closed, "%s]", content);
    free(content);
    content = closed;
  }
  if (!content) return -1;

  char *piece = NULL;
  if (!*msgs) {
    asprintf(msgs, "{\"role\":\"user\",\"content\":%s}", content);
  } else {
    asprintf(&piece, "%s,{\"role\":\"user\",\"content\":%s}", *msgs, content);
    free(*msgs);
    *msgs = piece;
  }
  free(content);
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
  /* OOM path: empty machine body — host dual-wires empty tool plates. */
  return out ? out : strdup("");
}

char *ng_agent_run(ng_agent_cfg *c, const char *user_prompt) {
  return ng_agent_run_ex(c, user_prompt, 0, NULL, NULL);
}

char *ng_agent_run_ex(ng_agent_cfg *c, const char *user_prompt,
                     int stream_final, ng_stream_fn on_delta, void *userdata) {
  return ng_agent_run_attachments(c, user_prompt, NULL, NULL, NULL,
                                  stream_final, on_delta, userdata);
}

char *ng_agent_run_vision(ng_agent_cfg *c, const char *user_prompt,
                          const char *image_b64, const char *image_mime,
                          int stream_final, ng_stream_fn on_delta, void *userdata) {
  return ng_agent_run_attachments(c, user_prompt, image_b64, image_mime, NULL,
                                  stream_final, on_delta, userdata);
}

char *ng_agent_run_attachments(ng_agent_cfg *c, const char *user_prompt,
                               const char *image_b64, const char *image_mime,
                               const char *images_json,
                               int stream_final, ng_stream_fn on_delta, void *userdata) {
  int has_image = (image_b64 && image_b64[0])
               || (images_json && images_json[0] == '[' && strstr(images_json, "base64"));
  if ((!user_prompt || !user_prompt[0]) && !has_image) {
    /* Machine plate — no free-text (empty prompt) banner. */
    return strdup("{\"schema\":\"nanobot.empty_prompt.v1\",\"ok\":false,"
                  "\"error\":\"empty_prompt\",\"python\":0}");
  }
  if (!user_prompt) user_prompt = "";

  /* Direct shell: @! command  (offline, no LLM, works without an LLM) */
  if (!has_image && strncmp(user_prompt, "@!", 2) == 0) {
    char *r = run_shell_direct(c, user_prompt + 2);
    /* lightweight memory of shell for context */
    if (r) ng_memory_record_exchange(user_prompt, r);
    return r;
  }

  int grok = is_grok_endpoint(c->base_url);
  const char *bearer = NULL;
  if (grok) {
    if (!c->session || ng_session_ensure(c->session) != 0) {
      /* Machine plate — no free-text sign-in prose banner. */
      return strdup("{\"schema\":\"nanobot.auth.v1\",\"ok\":false,"
                    "\"error\":\"not_signed_in\","
                    "\"hint\":\"device_auth_or_offline\",\"python\":0}");
    }
    bearer = ng_session_bearer(c->session);
    if (!bearer) {
      return strdup("{\"schema\":\"nanobot.auth.v1\",\"ok\":false,"
                    "\"error\":\"no_session\",\"hint\":\"device_auth\","
                    "\"python\":0}");
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

  ng_log("agent: user: %.200s%s", user_prompt, has_image ? " [+image]" : "");

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
      while (*p && count < ng_memory_recent_turns() * 2) {
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

  {
    int va = msg_append_user_vision(&inner, user_prompt,
                                    has_image ? image_b64 : NULL, image_mime,
                                    images_json);
    if (va == -2) {
      free(inner);
      return strdup("image too large (max ~2MB). Resize and retry.");
    }
    if (va != 0) {
      free(inner);
      return strdup("oom building vision message");
    }
  }

  char *messages = NULL;
  asprintf(&messages, "[%s]", inner ? inner : "");
  free(inner);
  if (!messages) return strdup("oom messages");

  /* Memory: never store raw base64 — caption only */
  char *mem_user = NULL;
  if (has_image)
    asprintf(&mem_user, "[image attached] %s", user_prompt[0] ? user_prompt : "(no caption)");
  else
    mem_user = strdup(user_prompt);

  /* Tools: OpenAI-style.
   * Explicit NANOBOT_TOOLS=0/1 (env or $NANOBOT_HOME/env) always wins.
   * Local llama default: tools ON when NANOBOT_TOOLS=1 or NANOBOT_LOCAL_TOOLS=1
   * (desktop hosts like Grokium set this). Otherwise local defaults OFF so
   * tiny embedded peers stay pure-chat unless opted in.
   * Cloud: tools ON by default. */
  int use_tools = 1;
  int tools_explicit = 0;
  {
    char ep[640];
    snprintf(ep, sizeof ep, "%s/env", ng_workdir());
    char *from_file = ng_slurp_env_file(ep, "NANOBOT_TOOLS");
    const char *t = from_file;
    if (!t || !t[0]) t = getenv("NANOBOT_TOOLS");
    if (t && t[0]) {
      tools_explicit = 1;
      if (t[0] == '0' || t[0] == 'n' || t[0] == 'N' || t[0] == 'f' || t[0] == 'F')
        use_tools = 0;
      else if (t[0] == '1' || t[0] == 'y' || t[0] == 'Y' || t[0] == 't' || t[0] == 'T')
        use_tools = 1;
    }
    free(from_file);
  }
  int is_local = c && c->base_url &&
                 (strstr(c->base_url, "127.0.0.1") || strstr(c->base_url, "localhost") ||
                  strstr(c->base_url, "0.0.0.0"));
  if (is_local && !tools_explicit) {
    const char *lt = getenv("NANOBOT_LOCAL_TOOLS");
    if (lt && (lt[0] == '1' || lt[0] == 'y' || lt[0] == 'Y'))
      use_tools = 1;
    else
      use_tools = 0; /* embedded/local pure-chat unless host opts in */
  }
  if (has_image) use_tools = 0; /* vision turn: answer image first, no tool thrash */
  /* Short social / trivial prompts: no tool thrash (even if tools enabled) */
  if (use_tools) {
    const char *p = user_prompt;
    while (*p == ' ' || *p == '\t') p++;
    size_t pl = strlen(p);
    if (pl > 0 && pl < 48 && p[0] != '@' && !strchr(p, '\n')) {
      char low[64];
      size_t i;
      for (i = 0; i < pl && i < sizeof low - 1; i++) {
        char ch = p[i];
        low[i] = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch;
      }
      low[i] = 0;
      if (!strcmp(low, "hello") || !strcmp(low, "hi") || !strcmp(low, "hey") ||
          !strcmp(low, "thanks") || !strcmp(low, "thank you") || !strcmp(low, "ping") ||
          !strcmp(low, "ok") || !strcmp(low, "yes") || !strcmp(low, "no") ||
          !strncmp(low, "hello ", 6) || !strncmp(low, "hi ", 3) ||
          (pl <= 16 && strspn(p, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ ") == pl))
        use_tools = 0;
    }
  }

  /* Provider policy: serial LLM (local default), subagent budget (Grok max 8). */
  ng_agent_apply_provider_policy(c);
  /* Nested subagent worker: keep shell tools (real multiagent work) but never
   * spawn further subagents / heavy task board / MCP fan-out. */
  int is_subworker = getenv("NANOBOT_SUBAGENT") != NULL;

  /* BrainCube core + braincell hive for coding (skip inside subagents). */
  if (!is_subworker && !has_image && use_tools && ng_braincell_enabled()) {
    char *hive = ng_braincell_try_coding(c, user_prompt, stream_final, on_delta,
                                         userdata);
    if (hive) {
      if (hive[0] && strncmp(hive, "API ", 4) != 0)
        ng_memory_record_exchange(mem_user ? mem_user : user_prompt, hive);
      free(mem_user);
      return hive;
    }
  }

  /* Base shell tool + task board + light subagents + braincells + optional MCP. */
  char *mcp_frag = is_subworker ? strdup("") : ng_mcp_openai_tools_fragment();
  char *task_frag = is_subworker ? strdup("") : ng_task_openai_tools_fragment();
  char *sub_frag = is_subworker ? strdup("") : ng_subagent_openai_tools_fragment();
  char *bc_frag = is_subworker ? strdup("") : ng_braincell_openai_tools_fragment();
  char *tools = NULL;
  asprintf(&tools,
    "[{\"type\":\"function\",\"function\":{"
    "\"name\":\"run_terminal_command\","
    "\"description\":\"Run ONE shell command on the user's machine. "
    "Use for real actions: open apps/terminals, edit files, git, builds, tests, "
    "systemctl --user, list dirs. "
    "Long jobs: prefer nohup … >log 2>&1 & then poll with tail/log, or a single "
    "foreground command that may take many minutes (timeout is generous). "
    "GUI: nohup alacritty >/dev/null 2>&1 &. "
    "Prefer short answers without tools for greets.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"command\":{\"type\":\"string\",\"description\":\"shell command\"}"
    "},\"required\":[\"command\"]}}}%s%s%s%s]",
    task_frag && task_frag[0] ? task_frag : "",
    sub_frag && sub_frag[0] ? sub_frag : "",
    bc_frag && bc_frag[0] ? bc_frag : "",
    mcp_frag && mcp_frag[0] ? mcp_frag : "");
  free(mcp_frag);
  free(task_frag);
  free(sub_frag);
  free(bc_frag);
  /* Subworkers need a few tool rounds to actually inspect the environment. */
  if (is_subworker && c->max_turns < 8)
    c->max_turns = 8;
  if (!tools) {
    tools = strdup(
      "[{\"type\":\"function\",\"function\":{"
      "\"name\":\"run_terminal_command\","
      "\"description\":\"Run ONE shell command\","
      "\"parameters\":{\"type\":\"object\",\"properties\":{"
      "\"command\":{\"type\":\"string\"}},\"required\":[\"command\"]}}}]");
  }

  char *final = NULL;
  char *last_tool_out = NULL; /* fallback text if model never speaks */
  int version_retries = 0;
  int max_turns = c->max_turns > 0 ? c->max_turns : ng_max_turns();
  if (max_turns < 2) max_turns = 2;
  /* Open tasks get extra turns and hard ceiling so multi-step work can finish. */
  int hard_max = ng_task_hard_max_turns();
  if (ng_task_is_open() && max_turns < hard_max)
    max_turns = hard_max;
  int task_reminders = 0;

  for (int turn = 0; turn < max_turns; turn++) {
    if (grok) {
      if (ng_session_ensure(c->session) != 0) {
        free(messages); free(last_tool_out); free(tools); free(mem_user);
        /* Machine plate — no free-text session-expired banner. */
        return strdup("{\"schema\":\"nanobot.auth.v1\",\"ok\":false,"
                      "\"error\":\"session_expired\","
                      "\"hint\":\"device_auth\",\"python\":0}");
      }
      bearer = ng_session_bearer(c->session);
    }

    /* Self-reminder: open task stays in the model's face every turn. */
    if (use_tools && ng_task_is_open() && task_reminders < 40) {
      char *rem = ng_task_reminder_text();
      if (rem && rem[0]) {
        size_t ml = strlen(messages);
        if (ml && messages[ml - 1] == ']') messages[ml - 1] = 0;
        char *esc = ng_json_escape(rem);
        char *nmsg = NULL;
        asprintf(&nmsg,
          "%s,{\"role\":\"user\",\"content\":\"%s\"}]",
          messages,
          esc ? esc
              : "{\"schema\":\"nanobot.task_reminder.v1\",\"ok\":true,"
                "\"must\":\"continue\",\"python\":0}");
        free(messages);
        messages = nmsg;
        free(esc);
        task_reminders++;
      }
      free(rem);
    }

    /* Last turn: no tools UNLESS an open task still needs work and we have budget. */
    int open_task = ng_task_is_open();
    int tools_now = use_tools && (turn < max_turns - 1 || (open_task && turn < hard_max - 1));
    if (open_task && turn >= hard_max - 1)
      tools_now = 0; /* forced stop approaching hard ceiling */
    char *body = NULL;
    if (tools_now) {
      asprintf(&body,
        "{\"model\":\"%s\",\"messages\":%s,\"tools\":%s,\"tool_choice\":\"auto\","
        "\"stream\":false}",
        c->model, messages, tools);
    } else {
      /* Nudge finalization after tools */
      if (last_tool_out && (turn >= max_turns - 1 || !open_task)) {
        size_t ml = strlen(messages);
        if (ml && messages[ml - 1] == ']') messages[ml - 1] = 0;
        char *nudge = NULL;
        if (open_task) {
          asprintf(&nudge,
            "%s,{\"role\":\"user\",\"content\":\"Turn budget nearly exhausted and task still open. "
            "Either make one last tool attempt, call task_block with why you are stuck, "
            "or if the goal is met call task_done. Then reply to the user in plain text.\"}]",
            messages);
        } else {
          asprintf(&nudge,
            "%s,{\"role\":\"user\",\"content\":\"Using the tool results above, reply to the user now in plain text. No more tools.\"}]",
            messages);
        }
        free(messages);
        messages = nudge;
      }
      int want_stream = stream_final && on_delta;
      /* Cap completion length so a looping local model cannot pin llama forever.
       * Short social prompts get a tighter cap. */
      {
        int max_tok = 2048;
        size_t upl = user_prompt ? strlen(user_prompt) : 0;
        if (upl > 0 && upl < 48) max_tok = 512;
        else if (upl < 200) max_tok = 1024;
        asprintf(&body,
          "{\"model\":\"%s\",\"messages\":%s,\"stream\":%s,\"max_tokens\":%d}",
          c->model, messages, want_stream ? "true" : "false", max_tok);
      }
    }

    char url[512];
    snprintf(url, sizeof url, "%s/chat/completions", c->base_url);

    int want_stream = stream_final && !tools_now && on_delta;
    int live_streamed = 0; /* set if SSE tokens already went to on_delta this turn */
    ng_log("agent: turn %d/%d POST %s tools=%d stream=%d",
           turn + 1, max_turns, url, tools_now, want_stream);

    char *resp = NULL;
    if (want_stream) {
      char *streamed = curl_post_json_stream(url, bearer, body, on_delta, userdata);
      free(body);
      body = NULL;
      if (streamed && streamed[0]) {
        final = streamed;
        live_streamed = 1;
        free(messages);
        free(last_tool_out);
        free(tools);
        if (final && strncmp(final, "API error:", 10) != 0) {
          ng_memory_record_exchange(mem_user ? mem_user : user_prompt, final);
        }
        ng_log("agent: final (streamed): %.300s", final);
        free(mem_user);
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
      free(messages); free(last_tool_out); free(tools); free(mem_user);
      return transport_err("no_response");
    }
    if (!resp[0]) {
      ng_log("agent: empty API body turn %d/%d tools=%d", turn + 1, max_turns, tools_now);
      free(resp);
      use_tools = 0;
      continue;
    }
    /* Transport plates already dual-wire; legacy free-text curl bodies → plate. */
    if (strstr(resp, "\"schema\":\"nanobot.transport.v1\"")) {
      free(messages); free(last_tool_out); free(tools); free(mem_user);
      return resp;
    }
    if (!strchr(resp, '{') &&
        (strstr(resp, "curl ") || strstr(resp, "fork failed") ||
         strstr(resp, "mkstemp") || strstr(resp, "waitpid"))) {
      ng_log("agent: API transport error: %.200s", resp);
      free(resp); free(messages); free(last_tool_out); free(tools); free(mem_user);
      return transport_err("api_transport");
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
      /* Dual-wire API error — free-text body stays in logs only. */
      ng_log("agent: API error: %.120s body=%.200s", em ? em : "?", resp);
      free(em); free(resp); free(messages); free(last_tool_out); free(tools); free(mem_user);
      return strdup("{\"schema\":\"nanobot.api_error.v1\",\"ok\":false,"
                    "\"error\":\"api_error\",\"python\":0}");
    }

    char *tname = NULL, *targs = NULL, *tid = NULL;
    if (tools_now && ng_json_first_tool_call(resp, &tname, &targs, &tid)) {
      ng_log("agent: tool %s id=%s args=%.200s", tname, tid, targs);
      /* Live tool tracking for streaming clients (spoiler UI) */
      if (stream_final && on_delta) {
        char *ea = ng_json_escape(targs ? targs : "");
        char *en = ng_json_escape(tname ? tname : "tool");
        char *ei = ng_json_escape(tid ? tid : "");
        char *ev = NULL;
        if (asprintf(&ev,
              "{\"type\":\"tool\",\"phase\":\"start\",\"id\":\"%s\","
              "\"name\":\"%s\",\"args\":\"%s\"}",
              ei ? ei : "", en ? en : "", ea ? ea : "") > 0 && ev)
          stream_evt(on_delta, userdata, ev);
        free(ea); free(en); free(ei); free(ev);
      }
      char *cmd = NULL;
      char *task_out = ng_task_try_tool(tname, targs);
      char *sub_out = task_out ? NULL
        : ng_subagent_try_tool(c, subagent_run_adapter, tname, targs);
      char *bc_out = (task_out || sub_out) ? NULL : ng_braincell_try_tool(c, tname, targs);
      char *mcp_out = (task_out || sub_out || bc_out) ? NULL : ng_mcp_try_tool(tname, targs);
      ng_cmd_result cr;
      memset(&cr, 0, sizeof cr);
      if (task_out) {
        cr.exit_code = 0;
        cr.output = task_out;
        ng_log("agent: task tool %s out=%.200s", tname, task_out);
        /* Task open → stretch turn budget so self-reminder loop can finish. */
        if (ng_task_is_open() && max_turns < hard_max) {
          max_turns = hard_max;
          ng_log("agent: open task — max_turns raised to %d", max_turns);
        }
      } else if (sub_out) {
        cr.exit_code = 0;
        cr.output = sub_out;
        ng_log("agent: subagent tool %s out=%.200s", tname, sub_out);
      } else if (bc_out) {
        cr.exit_code = 0;
        cr.output = bc_out;
        ng_log("agent: braincell tool %s out=%.200s", tname, bc_out);
      } else if (mcp_out) {
        cr.exit_code = 0;
        cr.output = mcp_out; /* owned; free via ng_cmd_result_free or manual */
        ng_log("agent: mcp tool done out=%.200s", mcp_out);
      } else if (strcmp(tname, "run_terminal_command") == 0 ||
          strcmp(tname, "run_terminal_cmd") == 0 ||
          strcmp(tname, "shell") == 0 ||
          strcmp(tname, "bash") == 0) {
        cmd = extract_command_arg(targs);
        cr = ng_run_command(cmd, c->timeout_sec);
        ng_log("agent: exit=%d out=%.200s", cr.exit_code, cr.output ? cr.output : "");
      } else {
        asprintf(&cmd, "echo 'unknown tool %s'", tname);
        cr = ng_run_command(cmd, c->timeout_sec);
        ng_log("agent: exit=%d out=%.200s", cr.exit_code, cr.output ? cr.output : "");
      }

      free(last_tool_out);
      asprintf(&last_tool_out, "exit=%d\n%s", cr.exit_code, cr.output ? cr.output : "");
      /* Cap what the model sees (keep head+tail of large dumps) */
      char *tool_for_model = NULL;
      {
        size_t L = last_tool_out ? strlen(last_tool_out) : 0;
        if (L <= 6000)
          tool_for_model = strdup(last_tool_out ? last_tool_out : "");
        else {
          asprintf(&tool_for_model,
                   "%.2500s\n\n…[%zu bytes total, middle omitted]…\n\n%s",
                   last_tool_out, L, last_tool_out + L - 2500);
        }
      }
      char *esc_out = ng_json_escape(tool_for_model ? tool_for_model : "");
      char *esc_args = ng_json_escape(targs);
      char *esc_tid = ng_json_escape(tid ? tid : "call");
      char *esc_tname = ng_json_escape(tname ? tname : "run_terminal_command");

      if (stream_final && on_delta) {
        char *en = ng_json_escape(tname ? tname : "tool");
        char *ei = ng_json_escape(tid ? tid : "");
        /* Cap tool output in SSE so UI stays light */
        char out_cap[2400];
        snprintf(out_cap, sizeof out_cap, "%.2200s", last_tool_out ? last_tool_out : "");
        char *eo = ng_json_escape(out_cap);
        char *ev = NULL;
        if (asprintf(&ev,
              "{\"type\":\"tool\",\"phase\":\"done\",\"id\":\"%s\","
              "\"name\":\"%s\",\"exit\":%d,\"output\":\"%s\"}",
              ei ? ei : "", en ? en : "", cr.exit_code, eo ? eo : "") > 0 && ev)
          stream_evt(on_delta, userdata, ev);
        free(en); free(ei); free(eo); free(ev);
      }

      size_t ml = strlen(messages);
      if (ml && messages[ml - 1] == ']') messages[ml - 1] = 0;
      char *new_messages = NULL;
      /* role=tool content = real stdout/stderr. Extra user nudge forces the model
       * to present results on the next turn instead of going silent. */
      asprintf(&new_messages,
        "%s,{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{\"id\":\"%s\","
        "\"type\":\"function\",\"function\":{\"name\":\"%s\",\"arguments\":\"%s\"}}]},"
        "{\"role\":\"tool\",\"tool_call_id\":\"%s\",\"content\":\"%s\"},"
        "{\"role\":\"user\",\"content\":\"Tool finished (exit code in content above). "
        "Present the results to the user in plain text now: summarize what happened, "
        "quote important output lines, and answer their request. "
        "Do not claim you cannot run commands. Only call another tool if still needed.\"}]",
        messages,
        esc_tid ? esc_tid : "call",
        esc_tname ? esc_tname : "run_terminal_command",
        esc_args ? esc_args : "{}",
        esc_tid ? esc_tid : "call",
        esc_out ? esc_out : "");
      free(messages);
      messages = new_messages;
      free(esc_args); free(esc_out); free(esc_tid); free(esc_tname);
      free(tool_for_model);
      free(tname); free(targs); free(tid); free(cmd);
      if (task_out || mcp_out || bc_out) {
        /* cr.output owned by task/mcp/braincell string; free without cmd_result_free double free */
        free(cr.output);
        cr.output = NULL;
      } else {
        ng_cmd_result_free(&cr);
      }
      free(resp);
      /* Prefer a presentation turn next if budget is tight; grow for long work */
      if (turn >= max_turns - 2 && max_turns < hard_max) {
        max_turns += 4;
        if (max_turns > hard_max) max_turns = hard_max;
        ng_log("agent: extend max_turns to %d (tool loop)", max_turns);
      }
      continue;
    }

    final = ng_json_message_content(resp);
    if (final && final[0]) {
      /* Cosplay tool_call in *content* (not tool_calls JSON) — never leave
       * unexecuted markup as the user reply. Promote to real shell tool. */
      char *cos_name = NULL, *cos_cmd = NULL;
      if (use_tools && tools_now &&
          parse_content_tool_cosplay(final, &cos_name, &cos_cmd)) {
        ng_log("agent: COSPLAY tool in content (not API tool_calls) name=%s cmd=%.200s",
               cos_name, cos_cmd);
        if (stream_final && on_delta) {
          char *ev = NULL;
          char *en = ng_json_escape(cos_name);
          char *ea = ng_json_escape(cos_cmd);
          if (asprintf(&ev,
                "{\"type\":\"tool\",\"phase\":\"start\",\"id\":\"cosplay\","
                "\"name\":\"%s\",\"args\":\"{\\\"command\\\":\\\"%s\\\"}\","
                "\"cosplay\":true}",
                en ? en : "shell", ea ? ea : "") > 0 && ev)
            stream_evt(on_delta, userdata, ev);
          free(en); free(ea); free(ev);
        }
        ng_cmd_result cr;
        memset(&cr, 0, sizeof cr);
        if (strcmp(cos_name, "run_terminal_command") == 0 ||
            strcmp(cos_name, "run_terminal_cmd") == 0 ||
            strcmp(cos_name, "shell") == 0 ||
            strcmp(cos_name, "bash") == 0) {
          cr = ng_run_command(cos_cmd, c->timeout_sec);
          ng_log("agent: cosplay exec exit=%d out=%.200s",
                 cr.exit_code, cr.output ? cr.output : "");
        } else {
          asprintf(&cr.output, "unknown cosplay tool %s (not executed)", cos_name);
          cr.exit_code = 1;
        }
        free(last_tool_out);
        asprintf(&last_tool_out,
                 "exit=%d\n%s\n(note: model wrote tool_call in text; "
                 "agent recovered and ran via shell policy)",
                 cr.exit_code, cr.output ? cr.output : "");
        if (stream_final && on_delta) {
          char out_cap[900];
          snprintf(out_cap, sizeof out_cap, "%.800s", last_tool_out ? last_tool_out : "");
          char *eo = ng_json_escape(out_cap);
          char *en = ng_json_escape(cos_name);
          char *ev = NULL;
          if (asprintf(&ev,
                "{\"type\":\"tool\",\"phase\":\"done\",\"id\":\"cosplay\","
                "\"name\":\"%s\",\"exit\":%d,\"output\":\"%s\",\"cosplay\":true}",
                en ? en : "shell", cr.exit_code, eo ? eo : "") > 0 && ev)
            stream_evt(on_delta, userdata, ev);
          free(en); free(eo); free(ev);
        }
        /* Feed tool result back so next turn is plain text for the user. */
        size_t ml = strlen(messages);
        if (ml && messages[ml - 1] == ']') messages[ml - 1] = 0;
        char *esc_out = ng_json_escape(last_tool_out);
        char *esc_cmd = ng_json_escape(cos_cmd);
        char *esc_name = ng_json_escape(cos_name);
        char *new_messages = NULL;
        asprintf(&new_messages,
          "%s,{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{\"id\":\"cosplay1\","
          "\"type\":\"function\",\"function\":{\"name\":\"%s\","
          "\"arguments\":\"{\\\"command\\\":\\\"%s\\\"}\"}}]},"
          "{\"role\":\"tool\",\"tool_call_id\":\"cosplay1\",\"content\":\"%s\"},"
          "{\"role\":\"user\",\"content\":\"Tool result above was recovered from "
          "illegal text tool_call cosplay. Reply to the user in plain text only. "
          "Never emit tool_call XML or run_terminal_command markup.\"}]",
          messages, esc_name ? esc_name : "shell", esc_cmd ? esc_cmd : "",
          esc_out ? esc_out : "");
        free(messages);
        messages = new_messages;
        free(esc_out); free(esc_cmd); free(esc_name);
        ng_cmd_result_free(&cr);
        free(cos_name); free(cos_cmd);
        free(final); final = NULL;
        free(resp);
        continue;
      }
      free(cos_name); free(cos_cmd);
      /* Residual cosplay we could not execute — never show raw markup. */
      if (content_looks_like_tool_cosplay(final)) {
        char *clean = strip_tool_cosplay_markup(final);
        free(final);
        char *honest = NULL;
        asprintf(&honest,
          "%s\n\n(Tool was NOT called — model emitted tool markup as text. "
          "Use @! <cmd> for real shell, or retry.)",
          (clean && clean[0]) ? clean : "(empty)");
        free(clean);
        final = honest;
      }
      /* If a multi-step task is still open, do not stop — remind and continue. */
      if (use_tools && ng_task_is_open() && turn < hard_max - 1) {
        ng_log("agent: model tried to finish but task still open — continue");
        size_t ml = strlen(messages);
        if (ml && messages[ml - 1] == ']') messages[ml - 1] = 0;
        char *esc = ng_json_escape(final);
        char *nmsg = NULL;
        asprintf(&nmsg,
          "%s,{\"role\":\"assistant\",\"content\":\"%s\"},"
          "{\"role\":\"user\",\"content\":\"Task is still OPEN (not task_done / task_block). "
          "Continue working: tools, task_step_done, then task_done or task_block.\"}]",
          messages, esc ? esc : "");
        free(messages);
        messages = nmsg;
        free(esc);
        free(final);
        final = NULL;
        free(resp);
        if (max_turns < hard_max) max_turns = hard_max;
        continue;
      }
      /* Non-stream completion still feeds SSE clients (tools turns, or empty
       * upstream stream fallback) so the UI can type/render progressively. */
      if (stream_final && on_delta && !live_streamed) {
        const char *p = final;
        size_t total = 0;
        while (*p) {
          size_t n = 0;
          while (p[n] && n < 24) n++;
          if (n == 24) {
            size_t b = n;
            while (b > 8 && p[b] && p[b] != ' ' && p[b] != '\n') b--;
            if (b > 8) n = b + 1;
          }
          on_delta(userdata, p, n);
          total += n;
          p += n;
        }
        ng_log("agent: synthetic stream %zu bytes to client", total);
      }
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
      /* Model skipped the presentation turn — still surface the command result. */
      asprintf(&final,
               "Command finished. Output:\n\n```\n%.4000s\n```\n",
               last_tool_out);
      ng_log("agent: final from last tool output (model never wrote text)");
    } else {
      ng_log("agent: empty final after %d turns (check curl/session/network)", max_turns);
      /* Dual-wire empty reply — no free-text peer/restart essay. */
      final = transport_err("empty_reply");
    }
  }

  free(messages);
  free(last_tool_out);
  free(tools);
  if (final && strncmp(final, "API error:", 10) != 0 &&
      strncmp(final, "(no content)", 12) != 0 &&
      strncmp(final, "curl failed", 11) != 0 &&
      strncmp(final, "(no reply", 9) != 0 &&
      !strstr(final, "\"schema\":\"nanobot.transport.v1\"") &&
      !strstr(final, "\"schema\":\"nanobot.api_error.v1\"")) {
    ng_memory_record_exchange(mem_user ? mem_user : user_prompt, final);
  }
  ng_log("agent: final: %.300s", final);
  free(mem_user);
  return final;
}
