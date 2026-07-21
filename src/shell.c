#include <time.h>
#include "shell.h"
#include "shell_gate.h"
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
/* Hard deny only — never runnable even with password.
 * reboot/poweroff/mkfs/dd live in shell_dangerous (approval-gated). */
static const char *default_deny[] = {
  ":(){", "rm -rf /", "rm -rf /*", "rm -fr /", "rm -fr /*",
  "wget http", "wget -", "curl |", "curl|",
  "bash -i", "/dev/tcp/", "nc -l", "ncat -l",
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

/* Personal files ACL — multiplatform, product-agnostic.
 * Mode:  $NANOBOT_HOME/files_acl  or env NANOBOT_FILES_ACL  (deny|read|full)
 * Roots: $NANOBOT_HOME/files_acl_roots (one path/prefix per line)
 *        or env NANOBOT_PERSONAL_ROOTS (colon-separated). No built-in OS paths.
 * Allow: $NANOBOT_HOME/files_acl_allow (exact path lines) — read exception under deny.
 * Hosts (wrappers/init) plant roots and dual-path aliases; this binary never hardcodes them.
 */
static char *read_files_acl(void) {
  char home_path[640];
  snprintf(home_path, sizeof home_path, "%s/files_acl", ng_workdir());
  size_t n = 0;
  char *raw = ng_read_file(home_path, &n);
  if (!raw || !raw[0]) {
    free(raw);
    raw = ng_getenv_dup("NANOBOT_FILES_ACL");
  }
  if (raw && raw[0]) {
    char *p = raw;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    char out[16];
    int j = 0;
    while (*p && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t' && j + 1 < (int)sizeof out)
      out[j++] = (char)tolower((unsigned char)*p++);
    out[j] = 0;
    free(raw);
    if (!strcmp(out, "deny") || !strcmp(out, "blocked") || !strcmp(out, "0") || !strcmp(out, "off"))
      return strdup("deny");
    if (!strcmp(out, "read") || !strcmp(out, "readonly") || !strcmp(out, "ro"))
      return strdup("read");
    if (!strcmp(out, "full") || !strcmp(out, "rw") || !strcmp(out, "1") || !strcmp(out, "on"))
      return strdup("full");
  } else {
    free(raw);
  }
  /* Mode missing: deny when roots exist (caller still no-ops if no roots). */
  return strdup("deny");
}

/* Mutating ops — blocked on personal trees when ACL is read-only. */
static int cmd_mutates(const char *cmd) {
  if (!cmd) return 0;
  static const char *writes[] = {
    " rm ", "rm ", "\trm ",
    " mv ", "mv ",
    " mkdir ", "mkdir ",
    " rmdir ", "rmdir ",
    " touch ", "touch ",
    " chmod ", "chmod ",
    " chown ", "chown ",
    " truncate ",
    " sed -i", "sed -i ",
    " tee ", " | tee",
    "dd of=", " of=",
    " -delete",
    " unzip ", "unzip ",
    " tar -x", "tar -c",
    " cp ", "cp ",
    " install ",
    NULL
  };
  if (strstr(cmd, ">>") || strstr(cmd, " >") || strstr(cmd, ">\"") || strstr(cmd, ">'"))
    return 1;
  for (int i = 0; writes[i]; i++) {
    if (strstr(cmd, writes[i])) return 1;
  }
  while (*cmd == ' ' || *cmd == '\t') cmd++;
  if (!strncmp(cmd, "rm ", 3) || !strcmp(cmd, "rm")) return 1;
  if (!strncmp(cmd, "mv ", 3) || !strncmp(cmd, "cp ", 3)) return 1;
  if (!strncmp(cmd, "mkdir", 5) || !strncmp(cmd, "rmdir", 5)) return 1;
  if (!strncmp(cmd, "touch", 5) || !strncmp(cmd, "chmod", 5)) return 1;
  return 0;
}

/* True if cmd mentions any configured personal root (substring). */
static int cmd_touches_personal(const char *cmd, char **roots, int nroots) {
  if (!cmd || nroots <= 0) return 0;
  for (int i = 0; i < nroots; i++) {
    if (roots[i] && roots[i][0] && strstr(cmd, roots[i])) return 1;
  }
  return 0;
}

/* True if every allow path present… no: allow if cmd contains any allow path
 * and does not touch other roots outside those allows — keep simple:
 * under deny, permit only when an allow path is a substring of cmd (host wrote aliases). */
static int cmd_gui_allow(const char *cmd, char **allows, int nallow) {
  if (!cmd || nallow <= 0) return 0;
  for (int i = 0; i < nallow; i++) {
    if (allows[i] && allows[i][0] == '/' && strstr(cmd, allows[i])) return 1;
  }
  return 0;
}

static int load_lines_home(const char *name, char ***out, int *out_n) {
  *out = NULL;
  *out_n = 0;
  char path[640];
  snprintf(path, sizeof path, "%s/%s", ng_workdir(), name);
  size_t n = 0;
  char **lines = load_pattern_file(path, &n);
  if (!lines || !n) {
    free_patterns(lines);
    return 0;
  }
  *out = lines;
  *out_n = (int)n;
  return 0;
}

/* Load roots from file, else NANOBOT_PERSONAL_ROOTS (: or ; separated). */
static int load_personal_roots(char ***out, int *out_n) {
  load_lines_home("files_acl_roots", out, out_n);
  if (*out_n > 0) return 0;
  char *env = ng_getenv_dup("NANOBOT_PERSONAL_ROOTS");
  if (!env || !env[0]) { free(env); return 0; }
  /* split into heap lines compatible with free_patterns */
  int cap = 8, n = 0;
  char **arr = calloc((size_t)cap + 1, sizeof(char *));
  if (!arr) { free(env); return 0; }
  char *p = env;
  while (*p) {
    while (*p == ':' || *p == ';' || *p == ' ' || *p == '\t') p++;
    if (!*p) break;
    char *s = p;
    while (*p && *p != ':' && *p != ';') p++;
    size_t len = (size_t)(p - s);
    while (len && (s[len - 1] == ' ' || s[len - 1] == '\t')) len--;
    if (len == 0) continue;
    char *one = malloc(len + 1);
    if (!one) break;
    memcpy(one, s, len);
    one[len] = 0;
    if (n >= cap) {
      cap *= 2;
      char **na = realloc(arr, (size_t)(cap + 1) * sizeof(char *));
      if (!na) { free(one); break; }
      arr = na;
    }
    arr[n++] = one;
  }
  arr[n] = NULL;
  free(env);
  *out = arr;
  *out_n = n;
  return 0;
}

/* 0=ok, 1=blocked personal */
static int personal_files_blocked(const char *command, char **why) {
  if (why) *why = NULL;
  if (!command) return 0;

  char **roots = NULL;
  int nroots = 0;
  load_personal_roots(&roots, &nroots);
  if (nroots <= 0) {
    free_patterns(roots);
    return 0; /* no roots configured → ACL not active */
  }

  char **allows = NULL;
  int nallow = 0;
  load_lines_home("files_acl_allow", &allows, &nallow);

  int touches = cmd_touches_personal(command, roots, nroots);
  if (!touches) {
    free_patterns(roots);
    free_patterns(allows);
    return 0;
  }

  char *acl = read_files_acl();
  int block = 0;
  const char *reason = NULL;
  int allowed = cmd_gui_allow(command, allows, nallow);

  if (!acl || !strcmp(acl, "deny")) {
    if (allowed && !cmd_mutates(command)) {
      block = 0;
      ng_log("shell: personal ACL allowlisted: %.160s", command);
    } else if (allowed && cmd_mutates(command)) {
      block = 1;
      reason = "personal files ACL=deny — allowlist is read-only for listed paths.\n";
    } else {
      block = 1;
      reason =
        "personal files ACL=deny — personal paths blocked.\n"
        "Set files_acl to read|full under NANOBOT_HOME, or add path to files_acl_allow.\n";
    }
  } else if (!strcmp(acl, "read")) {
    if (cmd_mutates(command)) {
      block = 1;
      reason =
        "personal files ACL=read — view only; write/delete/move blocked.\n"
        "Set files_acl=full under NANOBOT_HOME for edits.\n";
    }
  }
  if (block && why && reason) *why = strdup(reason);
  free(acl);
  free_patterns(roots);
  free_patterns(allows);
  return block;
}


int ng_command_denied(const char *command) {
  if (!command) return 1;
  if (strlen(command) > 8000) return 1;
  if (strstr(command, "`") && (strstr(command, "rm ") || strstr(command, "dd "))) {
    if (!pattern_allowed("`") && !dangerous_allowed()) return 1;
  }

  /* Privacy: personal storage ACL (before user denylist so it cannot be bypassed) */
  if (personal_files_blocked(command, NULL)) return 1;

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
        "# reboot\n");
      fclose(f);
    }
  }
}

static ng_cmd_result ng_run_command_ex(const char *command, int timeout_sec, int skip_dangerous) {
  ng_cmd_result r = { .exit_code = -1, .output = NULL };
  ng_shell_ensure_policy_files();
  ng_shell_ensure_dangerous_file();
  if (!ng_shell_is_enabled()) {
    r.exit_code = 403;
    r.output = strdup("shell disabled (shell_enabled=0 under NANOBOT_HOME)\n");
    return r;
  }
  /* Personal files ACL first (explicit message) */
  {
    char *why = NULL;
    if (personal_files_blocked(command, &why)) {
      r.exit_code = 126;
      char *msg = NULL;
      asprintf(&msg, "nanobot: blocked by personal files privacy\n%s\n",
               why ? why : "files_acl=deny");
      r.output = msg ? msg : strdup("nanobot: personal files blocked\n");
      free(why);
      ng_log("shell: personal ACL blocked: %.120s", command);
      return r;
    }
  }
  /* hard denylist always */
  if (ng_command_denied(command)) {
    r.exit_code = 126;
    r.output = strdup(
      "nanobot: command blocked by denylist policy (hard deny)\n"
      "hint: edit $NANOBOT_HOME/shell_denylist or shell_allow exception\n");
    return r;
  }
  if (!skip_dangerous) {
    ng_shell_class cls = ng_shell_classify(command);
    if (cls == NG_SHELL_DANGEROUS) {
      char *aid = ng_shell_approval_create(command, "shell");
      r.exit_code = 425;
      char *msg = NULL;
      asprintf(&msg,
        "nanobot: dangerous command — approval required\n"
        "approval_id=%s\n"
        "Approve via UI or POST /api/shell/approve {\"id\":\"%s\",\"password\":\"…\"}\n",
        aid ? aid : "?", aid ? aid : "?");
      r.output = msg ? msg : strdup("dangerous: approval required\n");
      free(aid);
      return r;
    }
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
      /* Unix-first PATH: $NANOBOT_HOME/bin, standard bins, then inherited PATH.
       * Hosts that need extra dirs set PATH before launch (no product OS prefixes). */
      const char *old = getenv("PATH");
      const char *home = ng_workdir();
      char npath[1280];
      if (home && home[0])
        snprintf(npath, sizeof npath,
                 "%s/bin:/usr/local/bin:/usr/bin:/bin:/sbin:/usr/sbin%s%s",
                 home, old && old[0] ? ":" : "", old && old[0] ? old : "");
      else
        snprintf(npath, sizeof npath,
                 "/usr/local/bin:/usr/bin:/bin:/sbin:/usr/sbin%s%s",
                 old && old[0] ? ":" : "", old && old[0] ? old : "");
      setenv("PATH", npath, 1);
    }
    /* Resolve sh by existence: /bin/sh, /usr/bin/sh, then $SHELL if it is a path. */
    if (access("/bin/sh", X_OK) == 0)
      execl("/bin/sh", "sh", "-c", command, (char *)NULL);
    if (access("/usr/bin/sh", X_OK) == 0)
      execl("/usr/bin/sh", "sh", "-c", command, (char *)NULL);
    {
      const char *sh = getenv("SHELL");
      if (sh && sh[0] == '/' && access(sh, X_OK) == 0)
        execl(sh, "sh", "-c", command, (char *)NULL);
    }
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

ng_cmd_result ng_run_command(const char *command, int timeout_sec) {
  return ng_run_command_ex(command, timeout_sec, 0);
}

ng_cmd_result ng_run_command_approved(const char *command, int timeout_sec) {
  return ng_run_command_ex(command, timeout_sec, 1);
}
