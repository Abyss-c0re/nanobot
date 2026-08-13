#ifndef NANOBOT_UTIL_H
#define NANOBOT_UTIL_H
#include <stddef.h>
#include <stdint.h>

#define NG_VERSION "0.5.4"
/* Floor CLI version for proxy gate; runtime may auto-bump (see ng_cli_version_*). */
#define NG_CLI_VERSION_DEFAULT "0.1.220"
#define NG_DEFAULT_PORT 8787
#define NG_DEFAULT_BASE "https://cli-chat-proxy.grok.com/v1"
#define NG_DEFAULT_MODEL "grok-4.5"
/* Default when --offline / --llama (OpenAI-compatible outer API). */
#define NG_DEFAULT_LOCAL_BASE "http://127.0.0.1:8080/v1"
#define NG_DEFAULT_LOCAL_MODEL "local"
/* Desktop defaults — long agent runs. Override with env (see getters). */
#define NG_MAX_TURNS 48
#define NG_CMD_TIMEOUT_SEC 600
/* Peer async shell jobs (POST /jobs kind=shell): mesh adb pulls must not stack
 * 600s workers every few minutes. Agent interactive shell keeps CMD_TIMEOUT. */
#define NG_JOB_SHELL_TIMEOUT_SEC 120
#define NG_HTTP_TIMEOUT_SEC 600
#define NG_HTTP_MAX_CHILDREN 24
#define NG_OUT_MAX (64 * 1024)
/* Prefer lean caps on small hosts / NANOBOT_LEAN=1 */
#define NG_LEAN_MAX_TURNS 6
#define NG_LEAN_CMD_TIMEOUT_SEC 45
#define NG_LEAN_HTTP_TIMEOUT_SEC 90
#define NG_LEAN_HTTP_MAX_CHILDREN 6
#define NG_LEAN_OUT_MAX (12 * 1024)
#define NG_LEAN_LOG_MAX (24 * 1024)
#define NG_HOST_LOG_MAX (256 * 1024)

/* Runtime limits (auto-lean on MemTotal < 400MB or NANOBOT_LEAN=1).
 * Env: NANOBOT_MAX_TURNS, NANOBOT_CMD_TIMEOUT, NANOBOT_HTTP_TIMEOUT */
void ng_limits_init(void);
int ng_is_lean(void);
int ng_max_turns(void);          /* agent tool loop budget */
int ng_cmd_timeout_sec(void);    /* shell tool wall time */
int ng_http_timeout_sec(void);   /* LLM curl --max-time */
int ng_http_max_children(void);
size_t ng_out_max(void);
size_t ng_log_max(void);
/* Static string for curl argv (valid until next call / process end). */
const char *ng_http_timeout_arg(void);
/* Compact JSON resource snapshot for monitors (malloc'd) */
char *ng_resources_json(void);

char *ng_read_file(const char *path, size_t *out_len);
int ng_write_file(const char *path, const char *data, size_t len);
/* mkstemp under $TMPDIR, then $NANOBOT_HOME/tmp, then /tmp.
 * path must be writable buffer >= 640. Returns fd or -1. */
int ng_mkstemp_home(char *path, size_t path_sz, const char *prefix);
char *ng_slurp_env_file(const char *path, const char *key); /* KEY=val lines */
/* Persist KEY=val in $NANOBOT_HOME/settings. */
char *ng_settings_get(const char *key); /* malloc'd or NULL */
int ng_settings_set(const char *key, const char *value);
const char *ng_settings_path(void);
char *ng_getenv_dup(const char *k);
void ng_set_workdir(const char *dir);
const char *ng_workdir(void);
char *ng_json_escape(const char *s);

/* Runtime Grok CLI version header (auto-bumps when proxy says outdated). */
void ng_cli_version_init(void);
const char *ng_cli_version(void);          /* e.g. "0.1.220" */
const char *ng_cli_user_agent(void);       /* e.g. "grok-cli/0.1.220" */
/* If resp is an outdated-CLI error, bump stored version and return 1 (caller retries). */
int ng_cli_version_handle_error(const char *resp);
/* Extract first JSON string value for "key": "..." (naive, fine for our payloads) */
char *ng_json_get_string(const char *json, const char *key);
/* Find "tool_calls" / function call name+arguments from chat completions response */
int ng_json_first_tool_call(const char *json, char **name, char **args, char **id);
char *ng_json_message_content(const char *json);
void ng_log(const char *fmt, ...);
void ng_log_init(const char *path);
const char *ng_log_path(void);
/* 0 = no stderr (file still gets logs) — for TUI / embedded hosts. Default 1. */
void ng_log_set_stderr(int enabled);
int ng_log_stderr_enabled(void);
char *ng_read_log_tail(size_t max_bytes);

#endif
