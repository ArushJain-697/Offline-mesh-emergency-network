#include "mesh_crypto.h"
#include <sodium.h>
#include <stdio.h>
#include <string.h>

static uint8_t key[crypto_secretbox_KEYBYTES];
static int initialized = 0;

/* ── Public API ────────────────────────────────────────────────── */

int crypto_overhead(void) {
    return crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES;
    /* 24 (nonce) + 16 (MAC) = 40 bytes per packet */
}

int crypto_init(const char *password) {
    if (sodium_init() < 0) {
        fprintf(stderr, "[CRYPTO] Failed to initialize libsodium\n");
        return -1;
    }

    if (!password || strlen(password) == 0) {
        fprintf(stderr, "[CRYPTO] Error: Password cannot be empty\n");
        return -1;
    }

    /* Hash the human-readable password into exactly 32 bytes */
    crypto_generichash(key, sizeof(key), 
                       (const unsigned char *)password, strlen(password), 
                       NULL, 0);

    printf("[CRYPTO] Network secured with password.\n");
    initialized = 1;
    return 0;
}

int crypto_encrypt_packet(const uint8_t *plain, int plain_len,
                           uint8_t *out, int out_len)
{
    if (!initialized) return -1;

    int needed = crypto_secretbox_NONCEBYTES
               + crypto_secretbox_MACBYTES
               + plain_len;
    if (out_len < needed) return -1;

    /* Write random nonce at the front of the output buffer */
    uint8_t *nonce      = out;
    uint8_t *ciphertext = out + crypto_secretbox_NONCEBYTES;

    randombytes_buf(nonce, crypto_secretbox_NONCEBYTES);

    /* crypto_secretbox_easy writes [MAC | encrypted_payload] */
    crypto_secretbox_easy(ciphertext, plain, plain_len, nonce, key);

    return needed;
}

int crypto_decrypt_packet(const uint8_t *cipher, int cipher_len,
                           uint8_t *out, int out_len)
{
    if (!initialized) return -1;

    int min_len = crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES;
    if (cipher_len <= min_len) return -1;

    const uint8_t *nonce       = cipher;
    const uint8_t *ciphertext  = cipher + crypto_secretbox_NONCEBYTES;
    int            cipher_data  = cipher_len  - crypto_secretbox_NONCEBYTES;
    int            plain_len    = cipher_data  - crypto_secretbox_MACBYTES;

    if (out_len < plain_len) return -1;

    if (crypto_secretbox_open_easy(out, ciphertext, cipher_data, nonce, key) != 0) {
        /* Authentication failed: wrong key, truncated, or tampered packet */
        return -1;
    }

    return plain_len;
}