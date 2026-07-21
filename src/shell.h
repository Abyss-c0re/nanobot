#ifndef NANOBOT_SHELL_H
#define NANOBOT_SHELL_H
#include <stddef.h>

typedef struct {
  int exit_code;
  char *output; /* stdout+stderr combined, truncated */
} ng_cmd_result;

/* Run command via /bin/sh -c with timeout; allowlist optional (NULL = permissive but blocks some patterns) */
ng_cmd_result ng_run_command(const char *command, int timeout_sec);
void ng_cmd_result_free(ng_cmd_result *r);
int ng_command_denied(const char *command);
/* Write default shell_denylist / shell_allow under NANOBOT_HOME if missing. */
void ng_shell_ensure_policy_files(void);

#endif
