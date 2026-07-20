#include "improve.h"
#include "memory.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

static void mem_path(char *out, size_t n, const char *name) {
  snprintf(out, n, "%s/memory/%s", ng_workdir(), name);
}

static const char *SEED_GOALS =
  "# nanobot self-improvement goals (edit freely; keep short)\n"
  "\n"
  "Mode: ship tiny durable wins each cycle. Prefer:\n"
  "1) Fix bugs / flaky scripts under $NANOBOT_HOME or this source tree\n"
  "2) Improve docs (MCP, peer, limits) — accurate only\n"
  "3) Add/refresh memory facts that help the next turn\n"
  "4) Tighten tests, build, or lean-mode behavior\n"
  "\n"
  "Hard limits:\n"
  "- No mass delete, force-push, wipe, or firewall lockout of SSH\n"
  "- No secrets in memory or git\n"
  "- One primary change per cycle; record result in improve_log\n"
  "- Prefer shell via run_terminal_command; @! also works offline\n"
  "\n"
  "Cycle checklist:\n"
  "A. Observe (uname, df -h $HOME, ls memory/, peer health if present)\n"
  "B. Read self_improve.txt goals + improve_log tail\n"
  "C. Pick ONE small improvement and do it\n"
  "D. Verify (command output) and summarize in 3-6 lines\n";

void ng_improve_seed(void) {
  ng_memory_init();
  char path[640];
  mem_path(path, sizeof path, "self_improve.txt");
  if (access(path, R_OK) != 0)
    ng_write_file(path, SEED_GOALS, strlen(SEED_GOALS));

  /* fold a short pointer into core once */
  char corep[640];
  mem_path(corep, sizeof corep, "core.txt");
  size_t clen = 0;
  char *core = ng_read_file(corep, &clen);
  if (!core || !strstr(core, "self-improvement")) {
    const char *add =
      "Self-improvement: run `nanobot --self-improve` or MCP tool self_improve_cycle. "
      "Goals in memory/self_improve.txt. Use shell tools; keep changes small and durable.\n";
    char *merged = NULL;
    asprintf(&merged, "%s%s%s", core ? core : "",
             (core && core[0] && core[clen ? clen - 1 : 0] != '\n') ? "\n" : "",
             add);
    if (merged) {
      size_t n = strlen(merged);
      if (n > NG_MEM_MAX_CORE) {
        /* keep head of old core + add is hard; just rewrite seed if huge */
        ng_write_file(corep, merged + (n - NG_MEM_MAX_CORE), NG_MEM_MAX_CORE);
      } else {
        ng_write_file(corep, merged, n);
      }
      free(merged);
    }
  }
  free(core);
}

void ng_improve_log_line(const char *kind, const char *text) {
  ng_memory_init();
  char path[640];
  mem_path(path, sizeof path, "improve_log.jsonl");
  FILE *f = fopen(path, "a");
  if (!f) return;
  time_t t = time(NULL);
  char *esc = ng_json_escape(text ? text : "");
  char *kesc = ng_json_escape(kind ? kind : "note");
  fprintf(f, "{\"ts\":%ld,\"kind\":\"%s\",\"text\":\"%s\"}\n",
          (long)t, kesc ? kesc : "note", esc ? esc : "");
  free(esc); free(kesc);
  fclose(f);
}

char *ng_improve_status_json(void) {
  ng_improve_seed();
  char goals[640], logp[640];
  mem_path(goals, sizeof goals, "self_improve.txt");
  mem_path(logp, sizeof logp, "improve_log.jsonl");
  size_t gl = 0, ll = 0;
  char *g = ng_read_file(goals, &gl);
  char *l = ng_read_file(logp, &ll);
  int cycles = 0;
  if (l) {
    for (const char *p = l; *p; p++) if (*p == '\n') cycles++;
  }
  /* last non-empty line */
  const char *last = "";
  if (l && ll) {
    const char *p = l + ll;
    while (p > l && (p[-1] == '\n' || p[-1] == '\r')) p--;
    const char *end = p;
    while (p > l && p[-1] != '\n') p--;
    static char lastbuf[400];
    size_t n = (size_t)(end - p);
    if (n >= sizeof lastbuf) n = sizeof lastbuf - 1;
    memcpy(lastbuf, p, n);
    lastbuf[n] = 0;
    last = lastbuf;
  }
  char *esc_last = ng_json_escape(last);
  char *out = NULL;
  asprintf(&out,
    "{\"ok\":true,\"mode\":\"self-improve\",\"version\":\"%s\",\"workdir\":\"%s\","
    "\"goals_path\":\"%s\",\"log_path\":\"%s\",\"cycles_logged\":%d,"
    "\"goals_bytes\":%zu,\"last_log\":\"%s\"}",
    NG_VERSION, ng_workdir(), goals, logp, cycles, gl, esc_last ? esc_last : "");
  free(g); free(l); free(esc_last);
  return out ? out : strdup("{\"ok\":false}");
}

static char *build_cycle_prompt(const char *focus) {
  char goals[640], logp[640];
  mem_path(goals, sizeof goals, "self_improve.txt");
  mem_path(logp, sizeof logp, "improve_log.jsonl");
  size_t gl = 0, ll = 0;
  char *g = ng_read_file(goals, &gl);
  char *l = ng_read_file(logp, &ll);
  /* tail of log ~1200 chars */
  const char *ltail = "";
  char lbuf[1201];
  if (l && ll) {
    if (ll <= 1200) {
      ltail = l;
    } else {
      size_t start = ll - 1200;
      while (start < ll && l[start] != '\n') start++;
      if (start < ll) start++;
      size_t n = ll - start;
      if (n > 1200) n = 1200;
      memcpy(lbuf, l + start, n);
      lbuf[n] = 0;
      ltail = lbuf;
    }
  }
  char *out = NULL;
  asprintf(&out,
    "SELF-IMPROVEMENT CYCLE (nanobot).\n"
    "You have tools: run_terminal_command. You may also answer with @!-style shell if needed "
    "but prefer the tool.\n"
    "Focus: %s\n\n"
    "Goals file (memory/self_improve.txt):\n%s\n\n"
    "Recent improve_log (tail):\n%s\n\n"
    "Do ONE small durable improvement now. Observe first, then act, then verify.\n"
    "End with:\n"
    "RESULT: <one line>\n"
    "NEXT: <one line for next cycle>\n",
    (focus && focus[0]) ? focus : "general — pick highest leverage small fix",
    g && g[0] ? g : "(empty goals)",
    ltail[0] ? ltail : "(no prior cycles)");
  free(g); free(l);
  return out ? out : strdup("SELF-IMPROVEMENT CYCLE: improve nanobot or host tooling safely.");
}

char *ng_improve_run_cycle(ng_agent_cfg *agent, const char *focus) {
  ng_improve_seed();
  char *prompt = build_cycle_prompt(focus);
  ng_log("improve: cycle start focus=%.120s", focus ? focus : "");
  ng_improve_log_line("cycle_start", focus && focus[0] ? focus : "general");
  char *reply = ng_agent_run(agent, prompt);
  free(prompt);
  if (reply) {
    /* store a compact log line */
    char brief[500];
    snprintf(brief, sizeof brief, "%.480s", reply);
    ng_improve_log_line("cycle_result", brief);
    /* profile breadcrumb */
    ng_memory_note_profile("self-improve: last cycle completed");
  } else {
    ng_improve_log_line("cycle_error", "null reply");
    reply = strdup("(no reply from agent)");
  }
  ng_log("improve: cycle done");
  return reply;
}

char *ng_improve_run_n(ng_agent_cfg *agent, int n, const char *focus) {
  if (n < 1) n = 1;
  if (n > 8) n = 8;
  char *acc = strdup("");
  if (!acc) return NULL;
  for (int i = 0; i < n; i++) {
    char *r = ng_improve_run_cycle(agent, focus);
    char *next = NULL;
    asprintf(&next, "%s\n===== cycle %d/%d =====\n%s\n",
             acc, i + 1, n, r ? r : "");
    free(acc); free(r);
    acc = next ? next : strdup("");
    if (!acc) break;
  }
  return acc;
}
