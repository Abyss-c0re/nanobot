/* P2P API share — local exit node + relay to peer exits (CubalC discovers). */
#ifndef NG_API_SHARE_H
#define NG_API_SHARE_H

#include "agent.h"
#include <stddef.h>

/* 1 when this home can serve as API exit (signed-in / local backend). */
int ng_api_share_local_exit_ok(ng_agent_cfg *c);

/* 1 when API share / relay path is enabled (env NANOBOT_API_SHARE). */
int ng_api_share_enabled(void);

/* Relay user prompt to first healthy exit peer. Returns owned reply body or NULL. */
char *ng_api_share_relay_prompt(const char *prompt);

/* JSON fragment fields for /peer/v1/info (no braces): exit_node, api_ok, api_share */
void ng_api_share_info_json(ng_agent_cfg *c, char *buf, size_t cap);

/* JSON body for GET /peer/v1/api-share (owned). */
char *ng_api_share_status_json(ng_agent_cfg *c);

#endif
