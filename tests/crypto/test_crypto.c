#include <nanobot/crypto.h>
#include <stdio.h>
#include <string.h>

static int fails;

static void expect(int cond, const char *msg) {
  if (!cond) {
    fprintf(stderr, "FAIL: %s\n", msg);
    fails++;
  }
}

int main(void) {
  expect(nb_ct_eq("abc", 3, "abc", 3) == 1, "ct_eq equal");
  expect(nb_ct_eq("abc", 3, "abd", 3) == 0, "ct_eq differ");
  expect(nb_ct_eq("abc", 3, "ab", 2) == 0, "ct_eq len");
  expect(nb_ct_eq(NULL, 0, "a", 1) == 0, "ct_eq null");

  unsigned char a[32], b[32];
  memset(a, 0, sizeof a);
  memset(b, 0, sizeof b);
  expect(nb_random_bytes(a, sizeof a) == 0, "random ok");
  expect(nb_random_bytes(b, sizeof b) == 0, "random ok2");
  /* extremely unlikely all zeros */
  int nonzero = 0;
  for (size_t i = 0; i < sizeof a; i++) if (a[i]) nonzero = 1;
  expect(nonzero, "random non-zero");
  /* two draws should almost never match */
  expect(nb_ct_eq(a, sizeof a, b, sizeof b) == 0, "random distinct");

  char hex[65];
  expect(nb_hex_encode(a, 16, hex, sizeof hex) == 0, "hex encode");
  expect(strlen(hex) == 32, "hex len");
  unsigned char back[16];
  size_t bl = 0;
  expect(nb_hex_decode(hex, 32, back, sizeof back, &bl) == 0, "hex decode");
  expect(bl == 16 && nb_ct_eq(a, 16, back, 16), "hex roundtrip");

  char wipe[8] = "secret!";
  nb_secure_wipe(wipe, sizeof wipe);
  expect(wipe[0] == 0 && wipe[7] == 0, "wipe");

  if (fails) {
    fprintf(stderr, "%d failures\n", fails);
    return 1;
  }
  printf("test_crypto OK\n");
  return 0;
}
