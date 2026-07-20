#include <nanobot/crypto.h>
#include "vendor/monocypher.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#include <errno.h>
#include <sys/stat.h>

int nb_master_key_load_or_create(const char *path, unsigned char key_out[32]) {
  if (!path || !key_out) return -1;
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    ssize_t n = read(fd, key_out, 32);
    close(fd);
    if (n == 32) return 0;
    /* corrupt — recreate */
  }
  if (nb_random_bytes(key_out, 32) != 0) return -1;
  char tmp[768];
  snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, (int)getpid());
  fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) {
    nb_secure_wipe(key_out, 32);
    return -1;
  }
  ssize_t w = write(fd, key_out, 32);
  if (w != 32) {
    close(fd);
    unlink(tmp);
    nb_secure_wipe(key_out, 32);
    return -1;
  }
  fsync(fd);
  close(fd);
  chmod(tmp, 0600);
  if (rename(tmp, path) != 0) {
    unlink(tmp);
    nb_secure_wipe(key_out, 32);
    return -1;
  }
  chmod(path, 0600);
  return 0;
}

void nb_kdf_provider_key(const void *peer_token, size_t token_len,
                         unsigned char key_out[32]) {
  static const char dom[] = "nanobot-provider-v1";
  crypto_blake2b_ctx ctx;
  crypto_blake2b_init(&ctx, 32);
  crypto_blake2b_update(&ctx, (const uint8_t *)dom, sizeof dom - 1);
  if (peer_token && token_len)
    crypto_blake2b_update(&ctx, (const uint8_t *)peer_token, token_len);
  crypto_blake2b_final(&ctx, key_out);
}

int nb_secret_is_sealed(const char *s) {
  return s && strncmp(s, "nbenc1:", 7) == 0;
}

char *nb_secret_seal(const unsigned char key[32], const void *plain, size_t plain_len) {
  if (!key || (!plain && plain_len)) return NULL;
  unsigned char nonce[24];
  if (nb_random_bytes(nonce, sizeof nonce) != 0) return NULL;
  unsigned char *ct = malloc(plain_len ? plain_len : 1);
  unsigned char mac[16];
  if (!ct) return NULL;
  crypto_aead_lock(ct, mac, key, nonce, NULL, 0,
                   (const uint8_t *)plain, plain_len);

  size_t blob = 24 + 16 + plain_len;
  unsigned char *raw = malloc(blob);
  if (!raw) {
    nb_secure_wipe(ct, plain_len);
    free(ct);
    return NULL;
  }
  memcpy(raw, nonce, 24);
  memcpy(raw + 24, mac, 16);
  if (plain_len) memcpy(raw + 40, ct, plain_len);
  nb_secure_wipe(ct, plain_len);
  free(ct);
  nb_secure_wipe(nonce, sizeof nonce);
  nb_secure_wipe(mac, sizeof mac);

  char *hex = malloc(blob * 2 + 1);
  if (!hex) {
    nb_secure_wipe(raw, blob);
    free(raw);
    return NULL;
  }
  if (nb_hex_encode(raw, blob, hex, blob * 2 + 1) != 0) {
    free(hex);
    nb_secure_wipe(raw, blob);
    free(raw);
    return NULL;
  }
  nb_secure_wipe(raw, blob);
  free(raw);

  char *env = NULL;
  asprintf(&env, "nbenc1:%s", hex);
  free(hex);
  return env;
}

char *nb_secret_open(const unsigned char key[32], const char *envelope, size_t *out_len) {
  if (out_len) *out_len = 0;
  if (!key || !envelope || !nb_secret_is_sealed(envelope)) return NULL;
  const char *hex = envelope + 7;
  size_t hex_len = strlen(hex);
  if (hex_len < (24 + 16) * 2 || (hex_len & 1)) return NULL;
  size_t raw_len = hex_len / 2;
  unsigned char *raw = malloc(raw_len);
  if (!raw) return NULL;
  size_t got = 0;
  if (nb_hex_decode(hex, hex_len, raw, raw_len, &got) != 0 || got != raw_len) {
    free(raw);
    return NULL;
  }
  if (raw_len < 40) {
    free(raw);
    return NULL;
  }
  unsigned char *nonce = raw;
  unsigned char *mac = raw + 24;
  unsigned char *ct = raw + 40;
  size_t ct_len = raw_len - 40;
  unsigned char *plain = malloc(ct_len + 1);
  if (!plain) {
    nb_secure_wipe(raw, raw_len);
    free(raw);
    return NULL;
  }
  if (crypto_aead_unlock(plain, mac, key, nonce, NULL, 0, ct, ct_len) != 0) {
    free(plain);
    nb_secure_wipe(raw, raw_len);
    free(raw);
    return NULL;
  }
  plain[ct_len] = 0;
  nb_secure_wipe(raw, raw_len);
  free(raw);
  if (out_len) *out_len = ct_len;
  return (char *)plain;
}
