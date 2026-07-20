#include <nanobot/crypto.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

#if defined(__linux__)
#include <sys/syscall.h>
#ifndef SYS_getrandom
#define SYS_getrandom 318
#endif
#endif

int nb_random_bytes(void *buf, size_t n) {
  if (!buf || n == 0) return n == 0 ? 0 : -1;
  unsigned char *p = (unsigned char *)buf;
  size_t got = 0;

  /* BSD / macOS */
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__) || defined(__DragonFly__)
  arc4random_buf(buf, n);
  return 0;
#endif

#if defined(__linux__)
  while (got < n) {
    long r = syscall(SYS_getrandom, p + got, n - got, 0);
    if (r < 0) {
      if (errno == EINTR) continue;
      break;
    }
    got += (size_t)r;
  }
  if (got == n) return 0;
  got = 0;
#endif

  /* POSIX / embedded fallback */
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0) fd = open("/dev/random", O_RDONLY);
  if (fd < 0) return -1;
  while (got < n) {
    ssize_t r = read(fd, p + got, n - got);
    if (r < 0) {
      if (errno == EINTR) continue;
      close(fd);
      return -1;
    }
    if (r == 0) {
      close(fd);
      return -1;
    }
    got += (size_t)r;
  }
  close(fd);
  return 0;
}
