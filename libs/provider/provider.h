#ifndef NANOBOT_PROVIDER_H
#define NANOBOT_PROVIDER_H
/* Tiny per-provider policy (Grok cloud vs local OpenAI-compatible). */

typedef enum {
  NG_PROVIDER_GROK = 0,
  NG_PROVIDER_LOCAL = 1,
  NG_PROVIDER_OFFLINE = 2
} ng_provider_kind;

typedef struct {
  ng_provider_kind kind;
  int subagents_enabled;     /* 0/1 */
  int subagents_max;         /* Grok default 8; share parent session */
  int llm_serial;            /* serialize LLM HTTP (default: 0 grok, 1 local) */
  int max_ctx_chars;         /* soft cap on messages blob chars */
  int max_sub_prompt_chars;  /* per subagent prompt cap */
  int max_sub_reply_chars;   /* truncate subagent result */
  int max_turns;             /* agent turns soft */
} ng_provider_policy;

/* Fill defaults from backend kind string: "grok"|"openai_compatible"|"offline_shell" */
void ng_provider_policy_defaults(ng_provider_policy *p, const char *backend_kind);

/* Overlay settings keys: SUBAGENTS, SUBAGENTS_MAX, LLM_SERIAL, MAX_CTX_CHARS, … */
void ng_provider_policy_load_settings(ng_provider_policy *p);

/* Resolve kind from base_url / backend label. */
ng_provider_kind ng_provider_kind_from(const char *backend_kind, const char *base_url);

#endif
