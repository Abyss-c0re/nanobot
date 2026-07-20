#include <nanobot/crypto.h>
#include <string.h>

int nb_ct_eq(const void *a, size_t na, const void *b, size_t nb) {
  if (!a || !b) return 0;
  if (na != nb) return 0;
  const unsigned char *x = (const unsigned char *)a;
  const unsigned char *y = (const unsigned char *)b;
  unsigned char d = 0;
  for (size_t i = 0; i < na; i++) d |= (unsigned char)(x[i] ^ y[i]);
  return d == 0;
}

void nb_secure_wipe(void *p, size_t n) {
  if (!p || n == 0) return;
  volatile unsigned char *v = (volatile unsigned char *)p;
  while (n--) *v++ = 0;
}
