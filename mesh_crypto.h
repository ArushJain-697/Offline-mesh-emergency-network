#ifndef MESH_CRYPTO_H
#define MESH_CRYPTO_H

#include <stdint.h>

/* Initialize crypto subsystem using a password.
   Generates a 32-byte key using libsodium's generic hash.
   Must be called before any encrypt/decrypt calls.
   Returns 0 on success, -1 on failure. */
int crypto_init(const char *password);

/* Number of bytes added to every packet: nonce (24) + MAC (16) = 40. */
int crypto_overhead(void);

/* Encrypt plain[plain_len] into out[].
   out must be at least plain_len + crypto_overhead() bytes.
   Returns total bytes written to out, or -1 on failure. */
int crypto_encrypt_packet(const uint8_t *plain, int plain_len,
                           uint8_t *out, int out_len);

/* Decrypt cipher[cipher_len] into out[].
   Returns decrypted byte count, or -1 if authentication fails
   (wrong key, truncated packet, or tampered data). */
int crypto_decrypt_packet(const uint8_t *cipher, int cipher_len,
                           uint8_t *out, int out_len);

#endif /* MESH_CRYPTO_H */