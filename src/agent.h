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

/* GET {base_url}/models (OpenAI-compatible; same as grok-build / llama.cpp).
 * Uses session Bearer on cloud, optional NANOBOT_API_KEY / env Bearer on local.
 * Returns malloc'd raw JSON body, or error string. */
char *ng_agent_fetch_models_json(ng_agent_cfg *c);
/* Parse model ids from OpenAI list JSON → malloc'd JSON array ["id",...] or "[]". */
char *ng_agent_models_ids_json(const char *models_body);
/* Set model id (and optional base) and save env. */
void ng_agent_select_model(ng_agent_cfg *c, const char *model);

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

/**
 * Multimodal: optional base64 image (no data: prefix) + mime (image/jpeg|png|webp).
 * OpenAI/Grok vision content array. image_b64 may be NULL (text-only).
 * Caps ~2.5MB base64. Memory stores text only ("[image] …").
 */
char *ng_agent_run_vision(ng_agent_cfg *c, const char *user_prompt,
                          const char *image_b64, const char *image_mime,
                          int stream_final, ng_stream_fn on_delta, void *userdata);

/**
 * Same as vision, plus optional images_json array:
 * [{"base64":"...","mime":"image/jpeg"}, ...] (up to 4). Document text should
 * already be folded into user_prompt by the caller.
 */
char *ng_agent_run_attachments(ng_agent_cfg *c, const char *user_prompt,
                               const char *image_b64, const char *image_mime,
                               const char *images_json,
                               int stream_final, ng_stream_fn on_delta, void *userdata);

/* Provider policy (subagents max, LLM serial) — safe to call often. */
void ng_agent_apply_provider_policy(ng_agent_cfg *c);
/* Spawn light subagent sharing this cfg's session; returns malloc'd id or NULL. */
char *ng_agent_subagent_spawn(ng_agent_cfg *c, const char *type, const char *desc,
                              const char *prompt);

#endif
