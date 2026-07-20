#ifndef NANOBOT_HTTP_H
#define NANOBOT_HTTP_H
#include "agent.h"
#include "auth.h"

typedef struct {
  int port;
  ng_agent_cfg *agent;
  ng_session *session;
  volatile int stop;
  /* Optional static file root (--www). NULL = peer/CLI only. */
  const char *www_root;
} ng_http_cfg;

int ng_http_serve(ng_http_cfg *cfg);

#endif
