#include "shell_gate.h"
#include "shell.h"
#include "util.h"
#include <nanobot/crypto.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>

/* Patterns that always need human approval (unless shell_allow exception). */
static const char *default_dangerous[] = {
  "reboot", "poweroff", "halt", "shutdown", "init 0", "init 6", "telinit",
  "mkfs", "dd if=", "dd of=", "nandwrite", "flash_erase",
  "rm -rf /", "rm -rf /*", "rm -fr /", "rm -fr /*",
  "chmod 777 /", "chown -R /", ">/dev/sda", "of=/dev/",
  NULL
};

static void approvals_dir(char *out, size_t n) {
  snprintf(out, n, "%s/approvals", ng_workdir());
  mkdir(out, 0700);
}

static void gate_paths(char *p1, size_t n1, char *p2, size_t n2) {
  snprintf(p1, n1, "%s/gate.blake2b", ng_workdir());
  snprintf(p2, n2, "/mnt/data/labauth/gate.blake2b");
}

void ng_shell_ensure_dangerous_file(void) {
  char path[640];
  snprintf(path, sizeof path, "%s/shell_dangerous", ng_workdir());
  if (access(path, F_OK) == 0) return;
  FILE *f = fopen(path, "w");
  if (!f) return;
  fprintf(f,
    "# Dangerous patterns — blocked until approved via password/biometric in ClankerCommander.\n"
    "# One substring per line. Hard denylist still applies first.\n");
  for (int i = 0; default_dangerous[i]; i++)
    fprintf(f, "%s\n", default_dangerous[i]);
  fclose(f);
}

static int pattern_in_file(const char *path, const char *command) {
  FILE *f = fopen(path, "r");
  if (!f) return 0;
  char line[512];
  int hit = 0;
  while (fgets(line, sizeof line, f)) {
    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p || *p == '#') continue;
    size_t n = strlen(p);
    while (n && (p[n-1] == '\n' || p[n-1] == '\r' || p[n-1] == ' ')) p[--n] = 0;
    if (n && strstr(command, p)) { hit = 1; break; }
  }
  fclose(f);
  return hit;
}

static int pattern_allowed_local(const char *pat) {
  /* reuse denylist allow path */
  char path[640];
  snprintf(path, sizeof path, "%s/shell_allow", ng_workdir());
  FILE *f = fopen(path, "r");
  if (!f) return 0;
  char line[512];
  int ok = 0;
  while (fgets(line, sizeof line, f)) {
    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p || *p == '#') continue;
    size_t n = strlen(p);
    while (n && (p[n-1] == '\n' || p[n-1] == '\r')) p[--n] = 0;
    if (n && strcasecmp(p, pat) == 0) { ok = 1; break; }
  }
  fclose(f);
  return ok;
}

ng_shell_class ng_shell_classify(const char *command) {
  if (!command || !command[0]) return NG_SHELL_DENY;
  /* hard deny first */
  if (ng_command_denied(command)) {
    /* if only matched as dangerous-style and allow file has exception, still check */
    return NG_SHELL_DENY;
  }
  ng_shell_ensure_dangerous_file();
  char path[640];
  snprintf(path, sizeof path, "%s/shell_dangerous", ng_workdir());
  int hit = 0;
  if (access(path, R_OK) == 0)
    hit = pattern_in_file(path, command);
  else {
    for (int i = 0; default_dangerous[i]; i++) {
      if (strstr(command, default_dangerous[i])) {
        if (!pattern_allowed_local(default_dangerous[i])) { hit = 1; break; }
      }
    }
  }
  if (hit) return NG_SHELL_DANGEROUS;
  return NG_SHELL_ALLOW;
}

static void blake_pw(const unsigned char *salt, size_t salt_len,
                     const char *password, unsigned char out[32]) {
  nb_blake2b_256_2(salt, salt_len, password, password ? strlen(password) : 0, out);
}

int ng_shell_gate_configured(void) {
  char a[640], b[640];
  gate_paths(a, sizeof a, b, sizeof b);
  return access(a, R_OK) == 0 || access(b, R_OK) == 0;
}

int ng_shell_gate_set_password(const char *password) {
  if (!password || !password[0] || strlen(password) < 4) return -1;
  unsigned char salt[16];
  if (nb_random_bytes(salt, sizeof salt) != 0) return -1;
  unsigned char hash[32];
  blake_pw(salt, sizeof salt, password, hash);
  char salt_hex[40], hash_hex[80];
  nb_hex_encode(salt, sizeof salt, salt_hex, sizeof salt_hex);
  nb_hex_encode(hash, sizeof hash, hash_hex, sizeof hash_hex);
  char line[160];
  snprintf(line, sizeof line, "v1:%s:%s\n", salt_hex, hash_hex);
  char a[640], b[640];
  gate_paths(a, sizeof a, b, sizeof b);
  if (ng_write_file(a, line, strlen(line)) != 0) return -1;
  chmod(a, 0600);
  /* best-effort mirror for ClankerDash shared gate */
  mkdir("/mnt/data/labauth", 0700);
  if (ng_write_file(b, line, strlen(line)) == 0) chmod(b, 0600);
  return 0;
}

int ng_shell_gate_verify_password(const char *password) {
  if (!password) return 0;
  char a[640], b[640];
  gate_paths(a, sizeof a, b, sizeof b);
  const char *path = access(a, R_OK) == 0 ? a : (access(b, R_OK) == 0 ? b : NULL);
  if (!path) return 0; /* no gate configured → deny approve */
  size_t len = 0;
  char *raw = ng_read_file(path, &len);
  if (!raw) return 0;
  /* v1:salt:hash */
  int ok = 0;
  if (!strncmp(raw, "v1:", 3)) {
    char *s1 = raw + 3;
    char *s2 = strchr(s1, ':');
    if (s2) {
      *s2 = 0;
      char *hexh = s2 + 1;
      while (*hexh == ' ' || *hexh == '\t') hexh++;
      size_t hl = strlen(hexh);
      while (hl && (hexh[hl-1] == '\n' || hexh[hl-1] == '\r')) hexh[--hl] = 0;
      unsigned char salt[32], expect[32], got[32];
      size_t sl = 0, el = 0;
      if (nb_hex_decode(s1, strlen(s1), salt, sizeof salt, &sl) == 0
          && nb_hex_decode(hexh, hl, expect, sizeof expect, &el) == 0
          && el == 32) {
        blake_pw(salt, sl, password, got);
        ok = nb_ct_eq(got, 32, expect, 32);
        nb_secure_wipe(got, sizeof got);
      }
    }
  }
  free(raw);
  return ok;
}

char *ng_shell_approval_create(const char *command, const char *source) {
  if (!command) return NULL;
  char dir[640];
  approvals_dir(dir, sizeof dir);
  unsigned char rnd[8];
  nb_random_bytes(rnd, sizeof rnd);
  char id[24];
  nb_hex_encode(rnd, sizeof rnd, id, sizeof id);
  char path[700];
  snprintf(path, sizeof path, "%s/%s.json", dir, id);
  char *esc = ng_json_escape(command);
  char *src = ng_json_escape(source ? source : "peer");
  char body[4200];
  snprintf(body, sizeof body,
    "{\"id\":\"%s\",\"command\":\"%s\",\"source\":\"%s\",\"ts\":%ld,\"status\":\"pending\"}\n",
    id, esc ? esc : "", src ? src : "peer", (long)time(NULL));
  free(esc); free(src);
  if (ng_write_file(path, body, strlen(body)) != 0) return NULL;
  chmod(path, 0600);
  return strdup(id);
}

char *ng_shell_approval_list_json(void) {
  char dir[640];
  approvals_dir(dir, sizeof dir);
  DIR *d = opendir(dir);
  if (!d) return strdup("[]");
  char *out = strdup("[");
  int first = 1;
  struct dirent *de;
  while ((de = readdir(d)) != NULL) {
    size_t n = strlen(de->d_name);
    if (n < 6 || strcmp(de->d_name + n - 5, ".json") != 0) continue;
    char path[700];
    snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
    size_t len = 0;
    char *raw = ng_read_file(path, &len);
    if (!raw) continue;
    if (strstr(raw, "\"status\":\"pending\"")) {
      size_t ol = strlen(out);
      char *nbuf = realloc(out, ol + len + 4);
      if (!nbuf) { free(raw); break; }
      out = nbuf;
      if (!first) strcat(out, ",");
      /* strip trailing newline */
      while (len && (raw[len-1] == '\n' || raw[len-1] == '\r')) raw[--len] = 0;
      strcat(out, raw);
      first = 0;
    }
    free(raw);
  }
  closedir(d);
  size_t ol = strlen(out);
  char *nbuf = realloc(out, ol + 2);
  if (nbuf) { out = nbuf; strcat(out, "]"); }
  return out;
}

static int load_approval(const char *id, char **cmd_out) {
  if (cmd_out) *cmd_out = NULL;
  if (!id || !id[0]) return -1;
  char path[700];
  snprintf(path, sizeof path, "%s/approvals/%s.json", ng_workdir(), id);
  size_t len = 0;
  char *raw = ng_read_file(path, &len);
  if (!raw) return -1;
  if (!strstr(raw, "\"status\":\"pending\"")) { free(raw); return -2; }
  char *cmd = ng_json_get_string(raw, "command");
  free(raw);
  if (!cmd) return -1;
  if (cmd_out) *cmd_out = cmd;
  else free(cmd);
  return 0;
}

int ng_shell_approval_approve(const char *id, const char *password, char **out_cmd) {
  if (out_cmd) *out_cmd = NULL;
  if (!ng_shell_gate_configured()) return -3; /* no password set */
  if (!ng_shell_gate_verify_password(password)) return -4;
  char *cmd = NULL;
  int rc = load_approval(id, &cmd);
  if (rc != 0) return rc;
  char path[700];
  snprintf(path, sizeof path, "%s/approvals/%s.json", ng_workdir(), id);
  unlink(path); /* consume */
  if (out_cmd) *out_cmd = cmd;
  else free(cmd);
  return 0;
}

int ng_shell_approval_reject(const char *id) {
  if (!id) return -1;
  char path[700];
  snprintf(path, sizeof path, "%s/approvals/%s.json", ng_workdir(), id);
  return unlink(path);
}
