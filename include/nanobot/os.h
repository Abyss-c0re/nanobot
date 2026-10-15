#ifndef NANOBOT_OS_H
#define NANOBOT_OS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void nb_set_workdir(const char *dir);
const char *nb_workdir(void);

char *nb_read_file(const char *path, size_t *out_len);
int nb_write_file(const char *path, const char *data, size_t len);
/** Atomic write with mode 0600 (secrets). */
int nb_write_secret_file(const char *path, const char *data, size_t len);

char *nb_getenv_dup(const char *k);
/** KEY=val lines; returns malloc'd value or NULL. */
char *nb_slurp_env_file(const char *path, const char *key);

const char *nb_settings_path(void);
char *nb_settings_get(const char *key);
int nb_settings_set(const char *key, const char *value);

#ifdef __cplusplus
}
#endif

#endif
