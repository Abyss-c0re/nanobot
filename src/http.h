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
  /* 0 = bind 127.0.0.1 only (default, not LAN-exposed). 1 = 0.0.0.0. */
  int bind_lan;
} ng_http_cfg;

int ng_http_serve(ng_http_cfg *cfg);

#endif
