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

/* Run one user prompt; returns final assistant text (malloc'd) */
char *ng_agent_run(ng_agent_cfg *c, const char *user_prompt);

#endif
