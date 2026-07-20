#include <nanobot/crypto.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

int main(void) {
  unsigned char key[32];
  char path[] = "/tmp/nb-session-key-XXXXXX";
  int fd = mkstemp(path);
  if (fd < 0) return 1;
  close(fd);
  unlink(path);
  if (nb_master_key_load_or_create(path, key) != 0) {
    fprintf(stderr, "key create fail\n");
    return 1;
  }
  const char *plain = "access_token=secret-value-xyz\nrefresh_token=abc\n";
  char *env = nb_secret_seal(key, plain, strlen(plain));
  if (!env || !nb_secret_is_sealed(env)) {
    fprintf(stderr, "seal fail\n");
    return 1;
  }
  if (strstr(env, "secret-value-xyz")) {
    fprintf(stderr, "plaintext leaked into envelope\n");
    return 1;
  }
  size_t out_len = 0;
  char *open = nb_secret_open(key, env, &out_len);
  if (!open || strcmp(open, plain) != 0) {
    fprintf(stderr, "open mismatch: %s\n", open ? open : "(null)");
    return 1;
  }
  free(open);
  free(env);

  /* peer-token KDF is deterministic and 32 bytes */
  unsigned char k1[32], k2[32], k3[32];
  nb_kdf_provider_key("abc", 3, k1);
  nb_kdf_provider_key("abc", 3, k2);
  nb_kdf_provider_key("abd", 3, k3);
  if (memcmp(k1, k2, 32) != 0) {
    fprintf(stderr, "kdf not deterministic\n");
    return 1;
  }
  if (memcmp(k1, k3, 32) == 0) {
    fprintf(stderr, "kdf collision on different tokens\n");
    return 1;
  }
  char *env2 = nb_secret_seal(k1, plain, strlen(plain));
  char *open2 = nb_secret_open(k1, env2, NULL);
  if (!open2 || strcmp(open2, plain) != 0) {
    fprintf(stderr, "seal under peer kdf failed\n");
    return 1;
  }
  free(open2);
  free(env2);
  unlink(path);
  printf("test_secret OK\n");
  return 0;
}
