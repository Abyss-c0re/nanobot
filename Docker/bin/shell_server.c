/* Tiny authenticated TCP shell for nanobot containers (static-friendly). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *password;
static const char *auth_key;
static const char *shell_path = "/bin/sh";

static void die(const char *m) {
  fprintf(stderr, "shell_server: %s\n", m);
  exit(1);
}

static int auth_ok(const char *line) {
  size_t n = strlen(line);
  while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
  char buf[512];
  if (n >= sizeof buf) return 0;
  memcpy(buf, line, n);
  buf[n] = 0;
  if (password && password[0]) {
    if (strncmp(buf, "PASS ", 5) == 0 && strcmp(buf + 5, password) == 0) return 1;
    if (strcmp(buf, password) == 0) return 1;
  }
  if (auth_key && auth_key[0]) {
    if (strncmp(buf, "KEY ", 4) == 0 && strcmp(buf + 4, auth_key) == 0) return 1;
    if (strcmp(buf, auth_key) == 0) return 1;
  }
  return 0;
}

static void handle(int cfd) {
  char line[512];
  size_t len = 0;
  dprintf(cfd, "nanobot-shell auth: PASS <password> | KEY <token>\n");
  while (len < sizeof line - 1) {
    char ch;
    ssize_t r = read(cfd, &ch, 1);
    if (r <= 0) { close(cfd); return; }
    line[len++] = ch;
    if (ch == '\n') break;
  }
  line[len] = 0;
  if (!auth_ok(line)) {
    dprintf(cfd, "auth failed\n");
    close(cfd);
    return;
  }
  dprintf(cfd, "ok\n");

  int mfd;
  pid_t pid = forkpty(&mfd, NULL, NULL, NULL);
  if (pid < 0) {
    dprintf(cfd, "pty failed\n");
    close(cfd);
    return;
  }
  if (pid == 0) {
    const char *home = getenv("HOME");
    if (!home) home = "/home/nanobot";
    setenv("HOME", home, 1);
    setenv("TERM", getenv("TERM") ? getenv("TERM") : "xterm", 1);
    if (chdir(home) != 0) chdir("/");
    execl(shell_path, shell_path, "-l", (char *)NULL);
    _exit(127);
  }
  for (;;) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(cfd, &rfds);
    FD_SET(mfd, &rfds);
    int mx = cfd > mfd ? cfd : mfd;
    if (select(mx + 1, &rfds, NULL, NULL, NULL) < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (FD_ISSET(cfd, &rfds)) {
      char b[4096];
      ssize_t n = read(cfd, b, sizeof b);
      if (n <= 0) break;
      if (write(mfd, b, (size_t)n) < 0) break;
    }
    if (FD_ISSET(mfd, &rfds)) {
      char b[4096];
      ssize_t n = read(mfd, b, sizeof b);
      if (n <= 0) break;
      if (write(cfd, b, (size_t)n) < 0) break;
    }
  }
  close(mfd);
  close(cfd);
  kill(pid, SIGTERM);
  waitpid(pid, NULL, 0);
}

int main(void) {
  const char *bind_host = getenv("SHELL_SERVER_BIND");
  (void)bind_host;
  const char *port_s = getenv("SSH_PORT");
  if (!port_s || !port_s[0]) port_s = getenv("SHELL_SERVER_PORT");
  int port = port_s && port_s[0] ? atoi(port_s) : 22;
  password = getenv("SSH_PASSWORD");
  auth_key = getenv("SSH_SHELL_KEY");
  if (!auth_key || !auth_key[0]) auth_key = getenv("SSH_KEY_TOKEN");
  const char *sh = getenv("SHELL_SERVER_SHELL");
  if (sh && sh[0]) shell_path = sh;

  if ((!password || !password[0]) && (!auth_key || !auth_key[0])) {
    /* ephemeral */
    char pw[24];
    unsigned char r[12];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
      if (read(fd, r, sizeof r) == (ssize_t)sizeof r) {
        static const char b64[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        size_t i;
        for (i = 0; i < sizeof r; i++) pw[i] = b64[r[i] % 64];
        pw[sizeof r] = 0;
        password = strdup(pw);
        const char *home = getenv("NANOBOT_HOME");
        if (!home) home = "/home/nanobot/.nanobot";
        char path[512];
        snprintf(path, sizeof path, "%s/ssh_ephemeral_password", home);
        mkdir(home, 0700);
        FILE *f = fopen(path, "w");
        if (f) {
          fprintf(f, "%s\n", password);
          fclose(f);
          chmod(path, 0600);
        }
        fprintf(stderr, "shell_server: ephemeral password: %s (saved %s)\n",
                password, path);
      }
      close(fd);
    }
    if (!password) password = "nanobot";
  }

  signal(SIGCHLD, SIG_IGN);
  signal(SIGPIPE, SIG_IGN);

  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) die("socket");
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons((uint16_t)port);
  if (bind(s, (struct sockaddr *)&addr, sizeof addr) < 0) die("bind");
  if (listen(s, 16) < 0) die("listen");
  fprintf(stderr, "shell_server: listen 0.0.0.0:%d\n", port);

  for (;;) {
    int c = accept(s, NULL, NULL);
    if (c < 0) {
      if (errno == EINTR) continue;
      break;
    }
    pid_t p = fork();
    if (p == 0) {
      close(s);
      handle(c);
      _exit(0);
    }
    close(c);
  }
  return 0;
}
