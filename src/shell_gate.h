#ifndef NANOBOT_SHELL_GATE_H
#define NANOBOT_SHELL_GATE_H

/* Command policy: allow | deny (hard) | dangerous (needs password/biometric approve). */

typedef enum {
  NG_SHELL_ALLOW = 0,
  NG_SHELL_DENY = 1,
  NG_SHELL_DANGEROUS = 2
} ng_shell_class;

ng_shell_class ng_shell_classify(const char *command);

/* Create pending approval for dangerous cmd. Returns malloc'd id or NULL. */
char *ng_shell_approval_create(const char *command, const char *source);

/* List pending as JSON array string (malloc). */
char *ng_shell_approval_list_json(void);

/* Approve by id if password matches gate (or force with peer-admin). 0 ok. */
int ng_shell_approval_approve(const char *id, const char *password, char **out_cmd);

/* Reject/cancel pending. */
int ng_shell_approval_reject(const char *id);

/* Set/verify web-compatible gate password.
 * Stored under NANOBOT_HOME/gate.blake2b; optional mirror via NANOBOT_GATE_MIRROR.
 * Format: v1:<salt_hex>:<hash_hex>  hash=BLAKE2b-256(salt||password)
 */
int ng_shell_gate_set_password(const char *password);
int ng_shell_gate_verify_password(const char *password);
int ng_shell_gate_configured(void);

void ng_shell_ensure_dangerous_file(void);

#endif
