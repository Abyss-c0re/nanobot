#ifndef NANOBOT_HTTP_H
#define NANOBOT_HTTP_H
#include "agent.h"
#include "auth.h"
#include <signal.h>

typedef struct ng_http_cfg ng_http_cfg;
struct ng_http_cfg {
  int port;
  ng_agent_cfg *agent;
  ng_session *session;
  /* Point at caller's stop flag (e.g. g_stop). Copied ints never see SIGTERM. */
  volatile sig_atomic_t *stop;
  /* Optional static file root (--www). NULL = peer/CLI only. */
  const char *www_root;
  /* 0 = bind 127.0.0.1 only (default, not LAN-exposed). 1 = 0.0.0.0. */
  int bind_lan;
  /* Optional: dual-wire listen plate only after bind+listen succeed. */
  void (*on_listening)(ng_http_cfg *cfg);
  void *on_listening_ud;
};

int ng_http_serve(ng_http_cfg *cfg);

#endif
