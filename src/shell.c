#include <time.h>
#include "shell.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>

int ng_command_denied(const char *command) {
  if (!command) return 1;
  static const char *bad[] = {
    "mkfs", "dd if=", "ddof=", ":(){", "reboot", "poweroff", "halt",
    "rm -rf /", "rm -rf /*", "nandwrite", "flash_erase", NULL
  };
  for (int i = 0; bad[i]; i++) {
    if (strstr(command, bad[i])) return 1;
  }
  return 0;
}

void ng_cmd_result_free(ng_cmd_result *r) {
  if (!r) return;
  free(r->output);
  r->output = NULL;
}

ng_cmd_result ng_run_command(const char *command, int timeout_sec) {
  ng_cmd_result r = { .exit_code = -1, .output = NULL };
  if (ng_command_denied(command)) {
    r.exit_code = 126;
    r.output = strdup("nanobot: command blocked by allowlist policy\n");
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
    /* soft env */
    setenv("NANOBOT", "1", 1);
    execl("/bin/sh", "sh", "-c", command, (char *)NULL);
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
          if (ncap > NG_OUT_MAX) ncap = NG_OUT_MAX + 1;
          if (len + (size_t)n + 1 > ncap) {
            /* truncate */
            size_t room = ncap - len - 1;
            if (room > 0) {
              memcpy(buf + len, tmp, room);
              len += room;
              buf[len] = 0;
            }
            /* drain rest */
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
      /* drain */
      while (1) {
        char tmp[1024];
        ssize_t n = read(pipefd[0], tmp, sizeof tmp);
        if (n <= 0) break;
        if (len + (size_t)n + 1 < cap || (cap < NG_OUT_MAX && (buf = realloc(buf, cap = (cap*2 < NG_OUT_MAX ? cap*2 : NG_OUT_MAX+1))))) {
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
