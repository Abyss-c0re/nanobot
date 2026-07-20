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

/* Host (peer/shell/memory/MCP) is reusable. Only Grok cloud needs browser session. */
int ng_agent_is_grok_backend(const ng_agent_cfg *c);
int ng_agent_needs_browser_session(const ng_agent_cfg *c);
/* "grok" | "openai_compatible" | "offline_shell" */
const char *ng_agent_backend_kind(const ng_agent_cfg *c);

/* Point at local llama.cpp / OpenAI-compatible server (no browser session). */
void ng_agent_set_local_backend(ng_agent_cfg *c, const char *base_url, const char *model);
/* Point at Grok cloud (browser device-code session). */
void ng_agent_set_grok_backend(ng_agent_cfg *c, const char *model);
/* Persist NANOBOT_BASE_URL / NANOBOT_MODEL into $HOME/env. */
int ng_agent_save_env(const ng_agent_cfg *c);

/* Stream chunks of assistant text (CLI real-time). May be called 0+ times. */
typedef void (*ng_stream_fn)(void *userdata, const char *chunk, size_t n);

/* Run one user prompt; returns final assistant text (malloc'd) */
char *ng_agent_run(ng_agent_cfg *c, const char *user_prompt);

/**
 * Same as ng_agent_run, but streams final-answer tokens via on_delta when
 * stream_final is non-zero. Tool rounds stay buffered; only free-text
 * completion streams. on_delta may be NULL (same as non-stream).
 */
char *ng_agent_run_ex(ng_agent_cfg *c, const char *user_prompt,
                      int stream_final, ng_stream_fn on_delta, void *userdata);

#endif
