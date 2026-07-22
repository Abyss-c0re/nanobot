#ifndef NANOBOT_SUBAGENT_H
#define NANOBOT_SUBAGENT_H
/* Light subagents (Grok-build-inspired, robot-sized):
 * - max 8 concurrent (hard), share parent session/auth
 * - types: general | explore | plan (capability labels only)
 * - state files under $NANOBOT_HOME/subagents/
 * - results are short summaries for the parent
 */

#define NG_SUBAGENT_MAX 8
#define NG_SUBAGENT_ID_LEN 24

typedef struct {
  char id[NG_SUBAGENT_ID_LEN];
  char type[16];       /* general|explore|plan */
  char status[16];     /* queued|running|done|error|cancelled */
  char desc[64];
  int pid;
} ng_subagent_info;

/* Init from provider policy (call after load). */
void ng_subagent_configure(int enabled, int max_slots);

int ng_subagent_enabled(void);
int ng_subagent_max(void);
int ng_subagent_running_count(void);

/* Spawn background child that runs agent_fn(prompt). Returns malloc'd id or NULL.
 * agent_fn is ng_agent_run-compatible: (void* cfg, const char *prompt) -> malloc text.
 * We pass opaque cfg pointer (ng_agent_cfg*). */
typedef char *(*ng_subagent_run_fn)(void *cfg, const char *prompt);

char *ng_subagent_spawn(void *agent_cfg, ng_subagent_run_fn run_fn,
                        const char *type, const char *desc, const char *prompt);

/* Poll one id → malloc JSON. */
char *ng_subagent_status_json(const char *id);
/* List all → malloc JSON array. */
char *ng_subagent_list_json(void);
/* Cancel / kill by id. */
int ng_subagent_cancel(const char *id);

/* OpenAI tools fragment (leading comma) for agent loop — or empty string strdup. */
char *ng_subagent_openai_tools_fragment(void);
/* Dispatch tool; malloc result or NULL if not ours. */
char *ng_subagent_try_tool(void *agent_cfg, ng_subagent_run_fn run_fn,
                           const char *name, const char *args_json);

#endif
