#include <time.h>
#include "shell.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <ctype.h>

/* Built-in defaults — used only when $NANOBOT_HOME/shell_denylist is missing.
 * User can replace the file entirely or add exceptions via shell_allow / SHELL_ALLOW. */
static const char *default_deny[] = {
  "mkfs", "dd if=", "dd of=", "ddof=", ":(){", "reboot", "poweroff", "halt",
  "shutdown", "init 0", "init 6", "telinit",
  "rm -rf /", "rm -rf /*", "rm -fr /", "rm -fr /*",
  "nandwrite", "flash_erase", "wget http", "wget -", "curl |", "curl|",
  "bash -i", "/dev/tcp/", "nc -l", "ncat -l",
  "chmod 777 /", "chown -R", ">/dev/sda", "of=/dev/",
  NULL
};

/* Absolute floor unless shell_allow_dangerous=1 — still user-overridable via shell_allow. */
static const char *floor_deny[] = {
  "rm -rf /", "rm -rf /*", "rm -fr /", "rm -fr /*", ":(){",
  NULL
};

static char *trim_copy(const char *s) {
  if (!s) return NULL;
  while (*s == ' ' || *s == '\t') s++;
  size_t n = strlen(s);
  while (n && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\n' || s[n-1] == '\r')) n--;
  char *o = malloc(n + 1);
  if (!o) return NULL;
  memcpy(o, s, n);
  o[n] = 0;
  return o;
}

static int line_is_comment(const char *s) {
  while (*s == ' ' || *s == '\t') s++;
  return !*s || *s == '#';
}

/* Load patterns from file (one per line). Returns NULL-terminated malloc array or NULL. */
static char **load_pattern_file(const char *path, size_t *out_n) {
  if (out_n) *out_n = 0;
  FILE *f = fopen(path, "r");
  if (!f) return NULL;
  char **arr = NULL;
  size_t n = 0, cap = 0;
  char line[512];
  while (fgets(line, sizeof line, f)) {
    if (line_is_comment(line)) continue;
    char *t = trim_copy(line);
    if (!t || !t[0]) { free(t); continue; }
    if (n + 1 >= cap) {
      size_t nc = cap ? cap * 2 : 16;
      char **na = realloc(arr, nc * sizeof(char *));
      if (!na) { free(t); break; }
      arr = na; cap = nc;
    }
    arr[n++] = t;
  }
  fclose(f);
  if (arr) {
    if (n + 1 >= cap) {
      char **na = realloc(arr, (n + 1) * sizeof(char *));
      if (na) arr = na;
    }
    if (arr) arr[n] = NULL;
  }
  if (out_n) *out_n = n;
  return arr;
}

static void free_patterns(char **arr) {
  if (!arr) return;
  for (size_t i = 0; arr[i]; i++) free(arr[i]);
  free(arr);
}

/* SHELL_ALLOW=reboot,poweroff or multi-line shell_allow file */
static int pattern_allowed(const char *pat) {
  if (!pat || !pat[0]) return 0;
  char path[640];
  snprintf(path, sizeof path, "%s/shell_allow", ng_workdir());
  FILE *f = fopen(path, "r");
  if (f) {
    char line[512];
    while (fgets(line, sizeof line, f)) {
      if (line_is_comment(line)) continue;
      char *t = trim_copy(line);
      if (t && t[0] && strcasecmp(t, pat) == 0) { free(t); fclose(f); return 1; }
      free(t);
    }
    fclose(f);
  }
  char *env = ng_settings_get("SHELL_ALLOW");
  if (!env) env = ng_getenv_dup("NANOBOT_SHELL_ALLOW");
  if (env) {
    char *p = env;
    while (*p) {
      while (*p == ' ' || *p == ',' || *p == ';') p++;
      if (!*p) break;
      char *start = p;
      while (*p && *p != ',' && *p != ';' && *p != '\n') p++;
      char save = *p;
      *p = 0;
      char *t = trim_copy(start);
      if (t && strcasecmp(t, pat) == 0) { free(t); free(env); if (save) *p = save; return 1; }
      free(t);
      if (save) { *p = save; p++; }
    }
    free(env);
  }
  return 0;
}

static int dangerous_allowed(void) {
  char path[640];
  snprintf(path, sizeof path, "%s/shell_allow_dangerous", ng_workdir());
  FILE *f = fopen(path, "r");
  if (f) {
    char b[16] = {0};
    if (fgets(b, sizeof b, f)) {
      if (b[0] == '1' || !strncasecmp(b, "on", 2) || !strncasecmp(b, "true", 4) || !strncasecmp(b, "yes", 3)) {
        fclose(f); return 1;
      }
    }
    fclose(f);
  }
  char *s = ng_settings_get("SHELL_ALLOW_DANGEROUS");
  if (s) {
    int ok = (s[0] == '1' || !strcasecmp(s, "on") || !strcasecmp(s, "true") || !strcasecmp(s, "yes"));
    free(s);
    return ok;
  }
  return 0;
}

int ng_command_denied(const char *command) {
  if (!command) return 1;
  if (strlen(command) > 8000) return 1;
  if (strstr(command, "`") && (strstr(command, "rm ") || strstr(command, "dd "))) {
    if (!pattern_allowed("`") && !dangerous_allowed()) return 1;
  }

  char path[640];
  snprintf(path, sizeof path, "%s/shell_denylist", ng_workdir());
  size_t n_user = 0;
  char **user = load_pattern_file(path, &n_user);

  const char **list = NULL;
  char **heap_list = user;
  if (user && user[0]) {
    /* user file replaces built-in defaults entirely */
    list = (const char **)user;
  } else {
    free_patterns(user);
    heap_list = NULL;
    list = default_deny;
  }

  for (int i = 0; list && list[i]; i++) {
    const char *pat = list[i];
    if (!strstr(command, pat)) continue;
    if (pattern_allowed(pat)) continue; /* user opt-in exception */
    free_patterns(heap_list);
    return 1;
  }
  free_patterns(heap_list);

  /* Floor: always deny catastrophic patterns unless explicitly allowed or dangerous mode */
  if (!dangerous_allowed()) {
    for (int i = 0; floor_deny[i]; i++) {
      if (strstr(command, floor_deny[i]) && !pattern_allowed(floor_deny[i]))
        return 1;
    }
  }
  return 0;
}

void ng_cmd_result_free(ng_cmd_result *r) {
  if (!r) return;
  free(r->output);
  r->output = NULL;
}

static int ng_shell_is_enabled(void) {
  char path[640];
  snprintf(path, sizeof path, "%s/shell_enabled", ng_workdir());
  FILE *f = fopen(path, "r");
  if (!f) return 1; /* default on */
  char b[16] = {0};
  if (fgets(b, sizeof b, f)) {
    size_t n = strlen(b);
    while (n && (b[n-1] == '\n' || b[n-1] == '\r' || b[n-1] == ' ')) b[--n] = 0;
  }
  fclose(f);
  if (!b[0]) return 1;
  if (b[0] == '0' || !strcasecmp(b, "off") || !strcasecmp(b, "false") || !strcasecmp(b, "disabled"))
    return 0;
  return 1;
}

/* Write default denylist file so users can edit it on disk. */
void ng_shell_ensure_policy_files(void) {
  char path[640];
  snprintf(path, sizeof path, "%s/shell_denylist", ng_workdir());
  if (access(path, F_OK) == 0) return;
  FILE *f = fopen(path, "w");
  if (!f) return;
  fprintf(f,
    "# nanobot shell denylist — one pattern per line (substring match).\n"
    "# Edit this file to customize. Delete a line to allow that pattern.\n"
    "# Exceptions: put patterns in shell_allow (or settings SHELL_ALLOW=reboot,poweroff).\n"
    "# Floor patterns (rm -rf / etc.) still blocked unless shell_allow_dangerous=1.\n"
    "#\n");
  for (int i = 0; default_deny[i]; i++)
    fprintf(f, "%s\n", default_deny[i]);
  fclose(f);
  /* empty allow file with docs */
  snprintf(path, sizeof path, "%s/shell_allow", ng_workdir());
  if (access(path, F_OK) != 0) {
    f = fopen(path, "w");
    if (f) {
      fprintf(f,
        "# Patterns listed here are allowed even if present in shell_denylist.\n"
        "# Example (uncomment to allow agent reboot):\n"
        "# reboot\n"
        "# svc power reboot\n");
      fclose(f);
    }
  }
}

ng_cmd_result ng_run_command(const char *command, int timeout_sec) {
  ng_cmd_result r = { .exit_code = -1, .output = NULL };
  ng_shell_ensure_policy_files();
  if (!ng_shell_is_enabled()) {
    r.exit_code = 403;
    r.output = strdup("shell disabled (shell_enabled=0 under NANOBOT_HOME)\n");
    return r;
  }
  if (ng_command_denied(command)) {
    r.exit_code = 126;
    r.output = strdup(
      "nanobot: command blocked by denylist policy\n"
      "hint: edit $NANOBOT_HOME/shell_denylist or add an exception to shell_allow\n"
      "      (e.g. put 'reboot' in shell_allow, or SHELL_ALLOW=reboot)\n");
    return r;
  }
  if (timeout_sec <= 0) timeout_sec = NG_CMD_TIMEOUT_SEC;

  int pipefd[2];
  if (pipe(pipefd) != 0) {
    r.output = strdup("pipe failed\n");
    return r;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]); close(pipefd[1]);
    r.output = strdup("fork failed\n");
    return r;
  }
  if (pid == 0) {
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);
    setenv("NANOBOT", "1", 1);
    {
      const char *old = getenv("PATH");
      const char *home = ng_workdir();
      char npath[1280];
      if (home && home[0])
        snprintf(npath, sizeof npath,
                 "%s/bin:/system/bin:/system/xbin:/vendor/bin:/usr/local/bin:/usr/bin:/bin:/sbin:/usr/sbin%s%s",
                 home, old && old[0] ? ":" : "", old && old[0] ? old : "");
      else
        snprintf(npath, sizeof npath,
                 "/system/bin:/system/xbin:/vendor/bin:/usr/local/bin:/usr/bin:/bin:/sbin:/usr/sbin%s%s",
                 old && old[0] ? ":" : "", old && old[0] ? old : "");
      setenv("PATH", npath, 1);
    }
    /* Android: prefer /system/bin/sh when /bin/sh missing */
    if (access("/bin/sh", X_OK) == 0)
      execl("/bin/sh", "sh", "-c", command, (char *)NULL);
    else
      execl("/system/bin/sh", "sh", "-c", command, (char *)NULL);
    _exit(127);
  }
  close(pipefd[1]);

  size_t cap = 4096, len = 0;
  char *buf = malloc(cap);
  if (!buf) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); close(pipefd[0]); r.output = strdup("oom\n"); return r; }
  buf[0] = 0;

  time_t deadline = time(NULL) + timeout_sec;
  int timed_out = 0;
  while (1) {
    if (time(NULL) > deadline) { timed_out = 1; kill(pid, SIGKILL); break; }
    struct pollfd pfd = { .fd = pipefd[0], .events = POLLIN };
    int pr = poll(&pfd, 1, 200);
    if (pr < 0 && errno == EINTR) continue;
    if (pr > 0 && (pfd.revents & POLLIN)) {
      char tmp[1024];
      ssize_t n = read(pipefd[0], tmp, sizeof tmp);
      if (n > 0) {
        if (len + (size_t)n + 1 > cap) {
          size_t ncap = cap * 2;
          if (ncap > ng_out_max()) ncap = ng_out_max() + 1;
          if (len + (size_t)n + 1 > ncap) {
            size_t room = ncap - len - 1;
            if (room > 0) {
              memcpy(buf + len, tmp, room);
              len += room;
              buf[len] = 0;
            }
            while (read(pipefd[0], tmp, sizeof tmp) > 0) {}
            break;
          }
          char *nb = realloc(buf, ncap);
          if (!nb) break;
          buf = nb; cap = ncap;
        }
        memcpy(buf + len, tmp, (size_t)n);
        len += (size_t)n;
        buf[len] = 0;
      } else if (n == 0) break;
    }
    int st;
    pid_t w = waitpid(pid, &st, WNOHANG);
    if (w == pid) {
      while (1) {
        char tmp[1024];
        ssize_t n = read(pipefd[0], tmp, sizeof tmp);
        if (n <= 0) break;
        if (len + (size_t)n + 1 < cap || (cap < ng_out_max() && (buf = realloc(buf, cap = (cap*2 < ng_out_max() ? cap*2 : ng_out_max()+1))))) {
          if (len + (size_t)n + 1 <= cap) {
            memcpy(buf + len, tmp, (size_t)n);
            len += (size_t)n;
            buf[len] = 0;
          }
        }
      }
      if (WIFEXITED(st)) r.exit_code = WEXITSTATUS(st);
      else r.exit_code = 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 1);
      break;
    }
  }
  close(pipefd[0]);
  if (timed_out) {
    waitpid(pid, NULL, 0);
    r.exit_code = 124;
    char *nb = realloc(buf, len + 64);
    if (nb) { buf = nb; strcat(buf, "\n[nanobot: timeout]\n"); }
  }
  r.output = buf ? buf : strdup("");
  return r;
}
