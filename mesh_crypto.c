#include "mesh_crypto.h"
#include <sodium.h>
#include <stdio.h>
#include <string.h>

#define KEY_FILE "mesh.key"

static uint8_t key[crypto_secretbox_KEYBYTES];
static int initialized = 0;

/* ── Public API ────────────────────────────────────────────────── */

int crypto_overhead(void) {
    return crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES;
    /* 24 (nonce) + 16 (MAC) = 40 bytes per packet */
}

int crypto_init(void) {
    if (sodium_init() < 0) {
        fprintf(stderr, "[CRYPTO] Failed to initialize libsodium\n");
        return -1;
    }

    FILE *fp = fopen(KEY_FILE, "rb");
    if (fp) {
        /* Load existing key */
        size_t n = fread(key, 1, sizeof(key), fp);
        fclose(fp);
        if (n != sizeof(key)) {
            fprintf(stderr,
                    "[CRYPTO] %s is corrupted (expected %zu bytes, got %zu). "
                    "Delete it and restart.\n",
                    KEY_FILE, sizeof(key), n);
            return -1;
        }
        printf("[CRYPTO] Loaded key from %s\n", KEY_FILE);
    } else {
        /* Generate brand-new random key */
        randombytes_buf(key, sizeof(key));

        fp = fopen(KEY_FILE, "wb");
        if (!fp) {
            fprintf(stderr, "[CRYPTO] Cannot create %s\n", KEY_FILE);
            return -1;
        }
        fwrite(key, 1, sizeof(key), fp);
        fclose(fp);

        printf("[CRYPTO] Generated new key — saved to %s\n", KEY_FILE);
        printf("[CRYPTO] Share this file with every team member before they join.\n");
        printf("[CRYPTO] Key (hex): ");
        for (int i = 0; i < (int)sizeof(key); i++)
            printf("%02x", key[i]);
        printf("\n");
    }

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
