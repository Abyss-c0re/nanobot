#include <nanobot/crypto.h>
#include <ctype.h>

int nb_hex_encode(const void *in, size_t n, char *out, size_t out_cap) {
  static const char *hex = "0123456789abcdef";
  if (!in || !out) return -1;
  if (out_cap < n * 2 + 1) return -1;
  const unsigned char *p = (const unsigned char *)in;
  for (size_t i = 0; i < n; i++) {
    out[i * 2] = hex[p[i] >> 4];
    out[i * 2 + 1] = hex[p[i] & 0xf];
  }
  out[n * 2] = 0;
  return 0;
}

static int hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

int nb_hex_decode(const char *hex, size_t n_hex, void *out, size_t out_cap, size_t *out_len) {
  if (!hex || !out || (n_hex & 1)) return -1;
  size_t n = n_hex / 2;
  if (n > out_cap) return -1;
  unsigned char *d = (unsigned char *)out;
  for (size_t i = 0; i < n; i++) {
    int hi = hex_nibble(hex[i * 2]);
    int lo = hex_nibble(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) return -1;
    d[i] = (unsigned char)((hi << 4) | lo);
  }
  if (out_len) *out_len = n;
  return 0;
}
