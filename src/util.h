#ifndef NANOBOT_UTIL_H
#define NANOBOT_UTIL_H
#include <stddef.h>
#include <stdint.h>

#define NG_VERSION "0.2.3"
/* Floor CLI version for proxy gate; runtime may auto-bump (see ng_cli_version_*). */
#define NG_CLI_VERSION_DEFAULT "0.1.220"
#define NG_DEFAULT_PORT 8787
#define NG_DEFAULT_BASE "https://127.0.0.1:8080/v1"
#define NG_DEFAULT_MODEL "grok-4.5"
#define NG_MAX_TURNS 12
#define NG_CMD_TIMEOUT_SEC 60
#define NG_HTTP_MAX_CHILDREN 24
#define NG_OUT_MAX (64 * 1024)

char *ng_read_file(const char *path, size_t *out_len);
int ng_write_file(const char *path, const char *data, size_t len);
char *ng_slurp_env_file(const char *path, const char *key); /* KEY=val lines */
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
char *ng_read_log_tail(size_t max_bytes);

#endif
