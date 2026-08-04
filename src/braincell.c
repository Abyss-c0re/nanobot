#define _POSIX_C_SOURCE 200809L
/* Mini-hive: BrainCube decides/fuses; local subagents = cells; optional external peers. */
#include "braincell.h"
#include "subagent.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <ctype.h>
#include <dirent.h>

#if defined(NANOBOT_HAS_BRAINCUBE) && NANOBOT_HAS_BRAINCUBE
#include <braincube/braincube.h>
#define BC_OK 1
#else
#define BC_OK 0
#endif

static int g_bc_inited;
#if BC_OK
static lhlam_cube g_core;
#endif

static void cells_dir(char *out, size_t n) {
  snprintf(out, n, "%s/braincells", ng_workdir());
  mkdir(out, 0755);
}

static void core_path(char *out, size_t n) {
  char d[640];
  cells_dir(d, sizeof d);
  snprintf(out, n, "%s/core.bin", d);
}

static void core_ensure(void) {
  if (g_bc_inited) return;
  g_bc_inited = 1;
#if BC_OK
  {
    char p[700];
    uint8_t seed[32];
    size_t n = 0;
    uint8_t *blob;
    memset(seed, 0xC3, sizeof seed);
    seed[0] = 0xB1;
    seed[1] = 0xA1;
    core_path(p, sizeof p);
    blob = (uint8_t *)ng_read_file(p, &n);
    if (blob && n && lhlam_cube_import(&g_core, blob, n) == 0) {
      free(blob);
      return;
    }
    free(blob);
    lhlam_cube_init(&g_core, seed);
    snprintf(g_core.id, sizeof g_core.id, "hive-core");
  }
#endif
}

static void core_save(void) {
#if BC_OK
  char p[700];
  uint8_t buf[8192];
  size_t n;
  core_path(p, sizeof p);
  n = lhlam_cube_export(&g_core, buf, sizeof buf);
  if (n) ng_write_file(p, (const char *)buf, n);
#else
  (void)0;
#endif
}

int ng_braincell_enabled(void) {
  const char *e = getenv("NANOBOT_BRAINCELLS");
  if (e && (e[0] == '0' || e[0] == 'n' || e[0] == 'N' || e[0] == 'f' || e[0] == 'F'))
    return 0;
  if (e && (e[0] == '1' || e[0] == 'y' || e[0] == 'Y')) return 1;
  /* default off on lean robots; desktop sets NANOBOT_BRAINCELLS=1 */
  return 0;
}

int ng_braincell_is_coding_prompt(const char *prompt) {
  static const char *kw[] = {
    "fix", "bug", "implement", "refactor", "code", "function", "compile",
    "test", "patch", "pr ", "pull request", "git ", "makefile", "cmake",
    "file ", "src/", "error:", "segfault", "lint", "typecheck", "build",
    "write a", "add a", "change the", "debug", "stack trace", "unit test",
    "integration", "api ", "endpoint", "class ", "struct ", "header",
    NULL
  };
  char low[512];
  size_t i, n;
  if (!prompt || !prompt[0]) return 0;
  n = strlen(prompt);
  if (n < 12) return 0;
  for (i = 0; i < n && i < sizeof low - 1; i++) {
    char c = prompt[i];
    low[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
  }
  low[i] = 0;
  for (i = 0; kw[i]; i++)
    if (strstr(low, kw[i])) return 1;
  /* multi-line + path-ish */
  if (strchr(prompt, '\n') && (strstr(prompt, "/") || strstr(prompt, ".")))
    return 1;
  return 0;
}

/* 0 = solo cell, 1 = multi-cell hive */
static int core_route_hive(const char *prompt) {
  core_ensure();
#if BC_OK
  {
    int d = lhlam_cube_decide_cstr(&g_core, prompt);
    /* Bias long/complex prompts toward hive even if cube is cold */
    size_t n = prompt ? strlen(prompt) : 0;
    if (n > 280 || (prompt && strchr(prompt, '\n') && n > 120))
      d = 1;
    if (n < 40)
      d = 0;
    return d ? 1 : 0;
  }
#else
  (void)prompt;
  return 0;
#endif
}

static void publish_cell(const char *role, const char *id, const char *body) {
  char path[700], dir[640];
  FILE *f;
  cells_dir(dir, sizeof dir);
  snprintf(path, sizeof path, "%s/%s_%s.json", dir, role, id ? id : "x");
  f = fopen(path, "w");
  if (!f) return;
  fprintf(f,
          "{\"role\":\"%s\",\"id\":\"%s\",\"ts\":%ld,\"body\":",
          role, id ? id : "", (long)time(NULL));
  {
    char *esc = ng_json_escape(body ? body : "");
    fprintf(f, "\"%s\"}\n", esc ? esc : "");
    free(esc);
  }
  fclose(f);
  /* External nanobots: best-effort signal on peer bus (mini-hive → remote cells).
   * Does not block the internal hive; peer must accept token-gated routes. */
  {
    const char *peer = getenv("NANOBOT_PEER_URL");
    if (peer && peer[0]) {
      char cmd[900];
      const char *tok = getenv("NANOBOT_PEER_TOKEN");
      if (tok && tok[0])
        snprintf(cmd, sizeof cmd,
                 "curl -sS -m 2 -X POST '%s/peer/v1/braincube' "
                 "-H 'Content-Type: application/json' "
                 "-H 'X-Nanobot-Peer-Token: %s' "
                 "-d '{\"action\":\"cell\",\"role\":\"%s\",\"scope\":\"external\"}' "
                 ">/dev/null 2>&1 &",
                 peer, tok, role);
      else
        snprintf(cmd, sizeof cmd,
                 "curl -sS -m 2 -X POST '%s/peer/v1/braincube' "
                 "-H 'Content-Type: application/json' "
                 "-d '{\"action\":\"cell\",\"role\":\"%s\",\"scope\":\"external\"}' "
                 ">/dev/null 2>&1 &",
                 peer, role);
      system(cmd);
    }
  }
}

static char *wait_sub_result(const char *id, int timeout_sec) {
  time_t deadline = time(NULL) + (timeout_sec > 0 ? timeout_sec : 180);
  while (time(NULL) < deadline) {
    char *st = ng_subagent_status_json(id);
    if (st) {
      char *status = ng_json_get_string(st, "status");
      char *result = ng_json_get_string(st, "result");
      if (!result) result = ng_json_get_string(st, "output");
      if (!result) result = ng_json_get_string(st, "reply");
      if (status && (!strcmp(status, "done") || !strcmp(status, "error") ||
                     !strcmp(status, "cancelled"))) {
        char *out = result ? strdup(result) : strdup(st);
        free(status);
        free(result);
        free(st);
        return out;
      }
      free(status);
      free(result);
      free(st);
    }
    ng_subagent_reap_all();
    usleep(200000);
  }
  return strdup("(braincell timeout — no result)");
}

static char *sub_run(void *cfg, const char *prompt) {
  return ng_agent_run((ng_agent_cfg *)cfg, prompt);
}

char *ng_braincell_try_coding(ng_agent_cfg *c, const char *prompt,
                              int stream_final, ng_stream_fn on_delta, void *ud) {
  char *explore_id = NULL, *plan_id = NULL;
  char *explore_out = NULL, *plan_out = NULL, *final = NULL;
  char *fused = NULL;
  int hive;
  (void)stream_final;
  (void)on_delta;
  (void)ud;

  if (!ng_braincell_enabled() || !c || !prompt) return NULL;
  if (!ng_braincell_is_coding_prompt(prompt)) return NULL;
  if (!ng_subagent_enabled()) return NULL;

  core_ensure();
  hive = core_route_hive(prompt);
  ng_log("braincell: coding route=%s", hive ? "HIVE" : "SOLO");

  if (!hive) {
#if BC_OK
    lhlam_cube_feedback_cstr(&g_core, prompt, 0, 1);
    core_save();
#endif
    return NULL; /* solo — normal agent loop */
  }

  /* --- HIVE: explore + plan cells in parallel --- */
  {
    char exp_p[4000], plan_p[4000];
    snprintf(exp_p, sizeof exp_p,
             "You are a BRAINCELL (explore) in a coding hive.\n"
             "Task: map the codebase facts needed for:\n---\n%.3000s\n---\n"
             "Use shell tools. Return: key files, symbols, constraints, risks. "
             "Be concrete. Max ~40 lines.",
             prompt);
    snprintf(plan_p, sizeof plan_p,
             "You are a BRAINCELL (plan) in a coding hive.\n"
             "Task: produce a short step plan for:\n---\n%.3000s\n---\n"
             "Return numbered steps, files to touch, acceptance checks. "
             "No full patches yet. Max ~30 lines.",
             prompt);
    explore_id = ng_subagent_spawn(c, sub_run, "explore", "braincell-explore", exp_p);
    plan_id = ng_subagent_spawn(c, sub_run, "plan", "braincell-plan", plan_p);
  }

  if (!explore_id && !plan_id) {
    ng_log("braincell: spawn failed — fall through solo");
    return NULL;
  }

  if (explore_id) {
    explore_out = wait_sub_result(explore_id, 240);
    publish_cell("explore", explore_id, explore_out);
  }
  if (plan_id) {
    plan_out = wait_sub_result(plan_id, 240);
    publish_cell("plan", plan_id, plan_out);
  }

  /* Core chooses emphasis: 0 = prefer plan, 1 = prefer explore-driven */
  {
    int prefer_explore = 0;
#if BC_OK
    char mix[8000];
    snprintf(mix, sizeof mix, "plan:%s\nexplore:%s",
             plan_out ? plan_out : "", explore_out ? explore_out : "");
    prefer_explore = lhlam_cube_decide_cstr(&g_core, mix);
#endif
    asprintf(&fused,
             "You are the coding agent. BrainCube core routed a HIVE.\n"
             "User request:\n%.2000s\n\n"
             "## Braincell explore\n%.2500s\n\n"
             "## Braincell plan\n%.2500s\n\n"
             "Core bias: %s.\n"
             "Now IMPLEMENT: use tools, edit files, run tests/builds as needed. "
             "Present a clear summary of what you did and results.",
             prompt,
             explore_out ? explore_out : "(none)",
             plan_out ? plan_out : "(none)",
             prefer_explore ? "ground in explore facts" : "follow the plan");
  }

  if (fused) {
    /* implement cell — can also run as subagent for isolation */
    char *impl_id = ng_subagent_spawn(c, sub_run, "general", "braincell-implement", fused);
    if (impl_id) {
      final = wait_sub_result(impl_id, 600);
      publish_cell("implement", impl_id, final);
      free(impl_id);
    } else {
      /* inline fallback */
      final = ng_agent_run(c, fused);
    }
  }

#if BC_OK
  {
    int ok = (final && final[0] && strncmp(final, "(braincell", 10) != 0);
    lhlam_cube_feedback_cstr(&g_core, prompt, 1, ok ? 1 : 0);
    core_save();
  }
#endif

  free(explore_id);
  free(plan_id);
  free(explore_out);
  free(plan_out);
  free(fused);

  if (final && on_delta && stream_final) {
    /* stream final text to UI */
    const char *p = final;
    while (*p) {
      size_t n = 0;
      while (p[n] && n < 32) n++;
      on_delta(ud, p, n);
      p += n;
    }
  }
  return final;
}

char *ng_braincell_openai_tools_fragment(void) {
  if (!ng_braincell_enabled()) return strdup("");
  return strdup(
      ",{\"type\":\"function\",\"function\":{"
      "\"name\":\"braincell_hive\","
      "\"description\":\"Run a coding braincell hive (explore+plan+implement) under BrainCube core decision. Use for non-trivial coding tasks.\","
      "\"parameters\":{\"type\":\"object\",\"properties\":{"
      "\"prompt\":{\"type\":\"string\",\"description\":\"coding task\"}"
      "},\"required\":[\"prompt\"]}}}"
      ",{\"type\":\"function\",\"function\":{"
      "\"name\":\"braincell_status\","
      "\"description\":\"Status of the braincell hive and core cube.\","
      "\"parameters\":{\"type\":\"object\",\"properties\":{}}}}");
}

char *ng_braincell_try_tool(ng_agent_cfg *c, const char *name, const char *args_json) {
  if (!name) return NULL;
  if (!strcmp(name, "braincell_status"))
    return ng_braincell_status_json();
  if (!strcmp(name, "braincell_hive")) {
    char *prompt = ng_json_get_string(args_json, "prompt");
    char *out;
    if (!prompt || !prompt[0]) {
      free(prompt);
      return strdup("{\"error\":\"prompt required\"}");
    }
    /* force hive path */
    setenv("NANOBOT_BRAINCELLS", "1", 1);
    out = ng_braincell_try_coding(c, prompt, 0, NULL, NULL);
    free(prompt);
    if (!out)
      out = strdup("{\"error\":\"hive did not run (solo route or spawn failed)\"}");
    return out;
  }
  return NULL;
}

char *ng_braincell_status_json(void) {
  char *out = NULL;
  char stats[256] = "unavailable";
  const char *peer = getenv("NANOBOT_PEER_URL");
  char *peer_esc = NULL;
  core_ensure();
#if BC_OK
  lhlam_cube_stats(&g_core, stats, sizeof stats);
#endif
  if (peer && peer[0]) peer_esc = ng_json_escape(peer);
  asprintf(&out,
           "{\"enabled\":%s,\"braincube\":%s,\"model\":\"mini-hive\","
           "\"core\":\"%s\",\"cells_dir\":\"braincells\","
           "\"external_peer\":%s%s%s}",
           ng_braincell_enabled() ? "true" : "false",
           BC_OK ? "true" : "false",
           stats,
           peer_esc ? "\"" : "null",
           peer_esc ? peer_esc : "",
           peer_esc ? "\"" : "");
  free(peer_esc);
  return out ? out : strdup("{\"enabled\":false}");
}
