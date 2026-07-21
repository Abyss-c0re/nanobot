#ifndef NANOBOT_CRYPTO_H
#define NANOBOT_CRYPTO_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Fill buf with cryptographically secure random bytes. 0 on success, -1 on failure. */
int nb_random_bytes(void *buf, size_t n);

/** Constant-time equality. Returns 1 if equal, 0 otherwise. */
int nb_ct_eq(const void *a, size_t na, const void *b, size_t nb);

/** Best-effort wipe of secret material. */
void nb_secure_wipe(void *p, size_t n);

/**
 * Encode n bytes as lowercase hex into out (needs 2*n+1).
 * Returns 0 on success, -1 if out_cap too small or args invalid.
 */
int nb_hex_encode(const void *in, size_t n, char *out, size_t out_cap);

/**
 * Decode hex string into out. n_hex is strlen of hex (even).
 * Returns 0 on success, -1 on error.
 */
int nb_hex_decode(const char *hex, size_t n_hex, void *out, size_t out_cap, size_t *out_len);

/* ---- local secret AEAD (XChaCha20-Poly1305 via Monocypher) ----
 * Provider auth (Grok access/refresh, device_login) is sealed under a key
 * derived from the peer token when present:
 *   key = BLAKE2b-256("nanobot-provider-v1" || peer_token)
 * Fallback: $NANOBOT_HOME/session.key (legacy / no peer token yet).
 */

/** Load or create 32-byte master key at path. Returns 0 on success. */
int nb_master_key_load_or_create(const char *path, unsigned char key_out[32]);

/**
 * Derive 32-byte provider-seal key from peer token material (any length).
 * Domain-separated; not reversible to the peer token.
 */
void nb_kdf_provider_key(const void *peer_token, size_t token_len,
                         unsigned char key_out[32]);

/**
 * Encrypt plaintext → malloc'd ASCII envelope:
 *   nbenc1:<hex(nonce24 || mac16 || ciphertext)>
 * Caller frees. Returns NULL on failure.
 */
char *nb_secret_seal(const unsigned char key[32], const void *plain, size_t plain_len);

/**
 * Decrypt envelope from nb_secret_seal. Returns malloc'd plaintext
 * (NUL-terminated if was text) and optional length. NULL on failure.
 */
char *nb_secret_open(const unsigned char key[32], const char *envelope, size_t *out_len);

/** True if s looks like nbenc1:… envelope. */
int nb_secret_is_sealed(const char *s);

/** BLAKE2b-256(a || b) — for gate password hashing. */
void nb_blake2b_256_2(const void *a, size_t al, const void *b, size_t bl,
                      unsigned char out[32]);

#ifdef __cplusplus
}
#endif

#endif
