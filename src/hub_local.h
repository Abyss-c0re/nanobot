#ifndef NANOBOT_HUB_LOCAL_H
#define NANOBOT_HUB_LOCAL_H
#include "agent.h"
#include "auth.h"

/* Append JSON object as one line to $HOME/hub/events.jsonl */
int ng_hub_event_obj(const char *json_object);

/* Convenience: type + optional fields as flat strings (escapes values). */
int ng_hub_event(const char *type, const char *k1, const char *v1,
                 const char *k2, const char *v2);

typedef struct {
  int port_out;
  volatile int *stop; /* shared with parent */
  /* expected observe token (malloc or static); NULL = use peer_token file */
  const char *out_token;
} ng_hub_out_cfg;

/** Blocking OUT listener (fork-per-client). Returns when *stop or error. */
int ng_hub_out_serve(ng_hub_out_cfg *cfg);

#endif
