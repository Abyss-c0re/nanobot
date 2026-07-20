#ifndef NANOBOT_AUTH_H
#define NANOBOT_AUTH_H
#include <time.h>

#define NG_AUTH_ISSUER "https://auth.x.ai"
#define NG_AUTH_CLIENT_ID "b1a00492-073a-47ea-816f-4c329264a828"
#define NG_AUTH_TOKEN_HDR "xai-grok-cli"
#define NG_AUTH_SCOPES \
  "openid profile email offline_access grok-cli:access api:access " \
  "conversations:read conversations:write workspaces:read workspaces:write"

typedef struct {
  char *access_token;
  char *refresh_token;
  time_t expires_at;
  char *email;
  char *user_code;
  char *verification_uri;
  char *verification_uri_complete;
  char *device_code;
  int poll_interval;
  time_t device_deadline;
  int login_pending;
} ng_session;

void ng_session_init(ng_session *s);
void ng_session_free(ng_session *s);
void ng_session_clear(ng_session *s);

/* Load/save session from NANOBOT_HOME/session.
 * Provider tokens (access/refresh) AEAD-encrypted at rest under key derived
 * from peer_token (preferred) or legacy session.key. */
int ng_session_load(ng_session *s);
int ng_session_save(const ng_session *s);

/* Persist in-flight device login so fork-per-request workers can poll.
 * Stored under NANOBOT_HOME/device_login (AEAD-sealed, mode 0600).
 * Contains device_code — treat as secret; never log full value. */
int ng_session_save_pending(const ng_session *s);
int ng_session_load_pending(ng_session *s);
void ng_session_clear_pending(void);

/* True if access_token present and not expired (with 60s skew) */
int ng_session_valid(const ng_session *s);

/* Ensure valid token: refresh if needed. Returns 0 on success. */
int ng_session_ensure(ng_session *s);

/* Start RFC8628 device login. Fills verification_uri* and user_code.
 * Does NOT block. Call ng_session_poll_login repeatedly. */
int ng_session_start_device_login(ng_session *s);

/* Poll token endpoint once. Returns:
 *  1 = authorized (session ready)
 *  0 = still pending
 * -1 = error/denied/expired
 */
int ng_session_poll_login(ng_session *s);

/* Blocking: start + poll until done or timeout. Prints links to stderr. */
int ng_session_login_blocking(ng_session *s);

/* Bearer token for API, or NULL */
const char *ng_session_bearer(const ng_session *s);

#endif
