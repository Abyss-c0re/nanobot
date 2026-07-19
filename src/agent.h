#ifndef NANOBOT_AGENT_H
#define NANOBOT_AGENT_H
#include "auth.h"

typedef struct {
  ng_session *session; /* not owned */
  char *base_url;
  char *model;
  int max_turns;
  int timeout_sec;
} ng_agent_cfg;

void ng_agent_cfg_init(ng_agent_cfg *c);
void ng_agent_cfg_free(ng_agent_cfg *c);
void ng_agent_load_env(ng_agent_cfg *c, const char *env_path);

/* Outer host (UI/shell/memory/MCP) is reusable. Only Grok cloud needs browser session. */
int ng_agent_is_grok_backend(const ng_agent_cfg *c);
int ng_agent_needs_browser_session(const ng_agent_cfg *c);
/* "grok" | "openai_compatible" | "offline_shell" */
const char *ng_agent_backend_kind(const ng_agent_cfg *c);

/* Point at local llama.cpp / OpenAI-compatible server (no browser session). */
void ng_agent_set_local_backend(ng_agent_cfg *c, const char *base_url, const char *model);
/* Point at Grok cloud (browser device-code session). */
void ng_agent_set_grok_backend(ng_agent_cfg *c, const char *model);
/* Persist NANOBOT_BASE_URL / NANOBOT_MODEL into $HOME/env (UI settings). */
int ng_agent_save_env(const ng_agent_cfg *c);

/* Run one user prompt; returns final assistant text (malloc'd) */
char *ng_agent_run(ng_agent_cfg *c, const char *user_prompt);

#endif
