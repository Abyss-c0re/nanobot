#include "provider.h"
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Settings helpers live in util — weak link via extern to avoid cycle.
 * provider is linked with legacy which has util. */
extern char *ng_settings_get(const char *key);

static int parse_bool(const char *s, int def) {
  if (!s || !s[0]) return def;
  if (s[0]=='1' || s[0]=='y' || s[0]=='Y' || s[0]=='t' || s[0]=='T' ||
      !strcasecmp(s,"on") || !strcasecmp(s,"yes") || !strcasecmp(s,"true"))
    return 1;
  if (s[0]=='0' || s[0]=='n' || s[0]=='N' || s[0]=='f' || s[0]=='F' ||
      !strcasecmp(s,"off") || !strcasecmp(s,"no") || !strcasecmp(s,"false"))
    return 0;
  return def;
}

static int parse_int(const char *s, int def, int lo, int hi) {
  if (!s || !s[0]) return def;
  int v = atoi(s);
  if (v < lo) v = lo;
  if (v > hi) v = hi;
  return v;
}

ng_provider_kind ng_provider_kind_from(const char *backend_kind, const char *base_url) {
  if (backend_kind && !strcmp(backend_kind, "grok")) return NG_PROVIDER_GROK;
  if (backend_kind && !strcmp(backend_kind, "offline_shell")) return NG_PROVIDER_OFFLINE;
  if (base_url && (strstr(base_url, "x.ai") || strstr(base_url, "grok")))
    return NG_PROVIDER_GROK;
  return NG_PROVIDER_LOCAL;
}

void ng_provider_policy_defaults(ng_provider_policy *p, const char *backend_kind) {
  if (!p) return;
  memset(p, 0, sizeof *p);
  p->kind = ng_provider_kind_from(backend_kind, NULL);
  /* Grok: subagents on, share session, max 8; LLM serial OFF (API can fan-out carefully).
   * Local: serial ON by default (one concurrent connection to small servers). */
  if (p->kind == NG_PROVIDER_GROK) {
    p->subagents_enabled = 1;
    p->subagents_max = 8;
    p->llm_serial = 0;
    p->max_ctx_chars = 96000;
    p->max_sub_prompt_chars = 6000;
    p->max_sub_reply_chars = 12000;
    p->max_turns = 16;
  } else if (p->kind == NG_PROVIDER_LOCAL) {
    p->subagents_enabled = 1;
    p->subagents_max = 4;
    p->llm_serial = 1; /* default ON for local */
    p->max_ctx_chars = 32000;
    p->max_sub_prompt_chars = 4000;
    p->max_sub_reply_chars = 8000;
    p->max_turns = 12;
  } else {
    p->subagents_enabled = 0;
    p->subagents_max = 0;
    p->llm_serial = 0;
    p->max_ctx_chars = 8000;
    p->max_sub_prompt_chars = 2000;
    p->max_sub_reply_chars = 4000;
    p->max_turns = 4;
  }
}

void ng_provider_policy_load_settings(ng_provider_policy *p) {
  if (!p) return;
  char *s;
  s = ng_settings_get("SUBAGENTS");
  if (s) { p->subagents_enabled = parse_bool(s, p->subagents_enabled); free(s); }
  s = ng_settings_get("SUBAGENTS_MAX");
  if (s) { p->subagents_max = parse_int(s, p->subagents_max, 0, 8); free(s); }
  s = ng_settings_get("LLM_SERIAL");
  if (s) { p->llm_serial = parse_bool(s, p->llm_serial); free(s); }
  s = ng_settings_get("MAX_CTX_CHARS");
  if (s) { p->max_ctx_chars = parse_int(s, p->max_ctx_chars, 2000, 200000); free(s); }
  s = ng_settings_get("MAX_SUB_PROMPT");
  if (s) { p->max_sub_prompt_chars = parse_int(s, p->max_sub_prompt_chars, 256, 32000); free(s); }
  s = ng_settings_get("MAX_SUB_REPLY");
  if (s) { p->max_sub_reply_chars = parse_int(s, p->max_sub_reply_chars, 256, 64000); free(s); }
  /* Hard cap: never more than 8 (Grok-build-style session share budget) */
  if (p->subagents_max > 8) p->subagents_max = 8;
}
