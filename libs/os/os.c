#include <nanobot/os.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

static char g_workdir[512] = "/tmp/nanobot";

void nb_set_workdir(const char *dir) {
  if (!dir || !*dir) return;
  snprintf(g_workdir, sizeof g_workdir, "%s", dir);
}

const char *nb_workdir(void) { return g_workdir; }

char *nb_read_file(const char *path, size_t *out_len) {
  if (out_len) *out_len = 0;
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
  long n = ftell(f);
  if (n < 0) { fclose(f); return NULL; }
  rewind(f);
  char *buf = malloc((size_t)n + 1);
  if (!buf) { fclose(f); return NULL; }
  size_t rd = fread(buf, 1, (size_t)n, f);
  fclose(f);
  buf[rd] = 0;
  if (out_len) *out_len = rd;
  return buf;
}

int nb_write_file(const char *path, const char *data, size_t len) {
  FILE *f = fopen(path, "wb");
  if (!f) return -1;
  size_t w = fwrite(data, 1, len, f);
  fclose(f);
  return w == len ? 0 : -1;
}

int nb_write_secret_file(const char *path, const char *data, size_t len) {
  if (!path || !data) return -1;
  char tmp[768];
  snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, (int)getpid());
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return -1;
  size_t off = 0;
  while (off < len) {
    ssize_t w = write(fd, data + off, len - off);
    if (w < 0) {
      if (errno == EINTR) continue;
      close(fd);
      unlink(tmp);
      return -1;
    }
    off += (size_t)w;
  }
  if (fsync(fd) != 0) { /* best effort */ }
  close(fd);
  if (chmod(tmp, 0600) != 0) { /* ignore */ }
  if (rename(tmp, path) != 0) {
    unlink(tmp);
    return -1;
  }
  return 0;
}

char *nb_getenv_dup(const char *k) {
  const char *v = getenv(k);
  return v ? strdup(v) : NULL;
}

char *nb_slurp_env_file(const char *path, const char *key) {
  char *body = nb_read_file(path, NULL);
  if (!body) return NULL;
  char linekey[128];
  snprintf(linekey, sizeof linekey, "%s=", key);
  char *p = body;
  char *found = NULL;
  while (p && *p) {
    char *nl = strchr(p, '\n');
    if (nl) *nl = 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '#' && strncmp(p, linekey, strlen(linekey)) == 0) {
      char *v = p + strlen(linekey);
      while (*v == ' ' || *v == '"' || *v == '\'') v++;
      size_t L = strlen(v);
      while (L && (v[L - 1] == ' ' || v[L - 1] == '"' || v[L - 1] == '\'' || v[L - 1] == '\r'))
        v[--L] = 0;
      found = strdup(v);
      break;
    }
    if (!nl) break;
    p = nl + 1;
  }
  free(body);
  return found;
}

const char *nb_settings_path(void) {
  static char path[700];
  snprintf(path, sizeof path, "%s/settings", nb_workdir());
  return path;
}

char *nb_settings_get(const char *key) {
  if (!key || !key[0]) return NULL;
  return nb_slurp_env_file(nb_settings_path(), key);
}

int nb_settings_set(const char *key, const char *value) {
  if (!key || !key[0] || !value) return -1;
  const char *path = nb_settings_path();
  char *body = nb_read_file(path, NULL);
  size_t cap = (body ? strlen(body) : 0) + strlen(key) + strlen(value) + 64;
  char *out = malloc(cap);
  if (!out) {
    free(body);
    return -1;
  }
  out[0] = 0;
  size_t o = 0;
  int replaced = 0;
  if (body) {
    char *p = body;
    while (*p) {
      char *nl = strchr(p, '\n');
      size_t len = nl ? (size_t)(nl - p) : strlen(p);
      char line[512];
      if (len >= sizeof line) len = sizeof line - 1;
      memcpy(line, p, len);
      line[len] = 0;
      size_t L = strlen(line);
      while (L && (line[L - 1] == '\r' || line[L - 1] == ' ')) line[--L] = 0;
      int is_comment = (!line[0] || line[0] == '#');
      int match = 0;
      if (!is_comment) {
        char *eq = strchr(line, '=');
        if (eq) {
          *eq = 0;
          char *k = line;
          while (*k == ' ' || *k == '\t') k++;
          size_t kl = strlen(k);
          while (kl && (k[kl - 1] == ' ' || k[kl - 1] == '\t')) k[--kl] = 0;
          if (strcmp(k, key) == 0) match = 1;
          *eq = '=';
        }
      }
      if (match) {
        o += (size_t)snprintf(out + o, cap - o, "%s=%s\n", key, value);
        replaced = 1;
      } else if (line[0] || is_comment) {
        o += (size_t)snprintf(out + o, cap - o, "%s\n", line);
      }
      if (!nl) break;
      p = nl + 1;
    }
    free(body);
  }
  if (!replaced) o += (size_t)snprintf(out + o, cap - o, "%s=%s\n", key, value);
  int rc = nb_write_file(path, out, o);
  free(out);
  return rc;
}
