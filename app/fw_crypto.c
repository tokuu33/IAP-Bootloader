#include "fw_crypto.h"
#include "aes.h"

/*
 * Default AES-128 key (AES-128 NIST test-vector key: 2b7e151628aed2a6abf7158809cf4f3c).
 * IMPORTANT: Replace this with your own secret key before production deployment.
 *            The same key must be used by the host-side firmware packaging tool.
 */
const uint8_t fw_crypto_default_key[FW_CRYPTO_KEY_SIZE] = {
    0x2B, 0x7E, 0x15, 0x16, 0x28, 0xAE, 0xD2, 0xA6,
    0xAB, 0xF7, 0x15, 0x88, 0x09, 0xCF, 0x4F, 0x3C
};

void fw_crypto_ctr_xcrypt(uint8_t *data, size_t length,
                          const uint8_t *key, const uint8_t *iv)
{
    struct AES_ctx ctx;
    AES_init_ctx_iv(&ctx, key, iv);
    AES_CTR_xcrypt_buffer(&ctx, data, length);
}
