#include "fw_crypto.h"

/*
 * AES-128 key used to encrypt firmware images stored on the W25Q128.
 *
 * IMPORTANT: Replace this default key with a project-specific secret
 *            key before deploying to production hardware.  The same
 *            key must be used in the host-side flashing tool and in
 *            every bootloader build that needs to read the backups.
 */
static const uint8_t s_aes_key[FW_CRYPTO_KEY_SIZE] = {
    0x2B, 0x7E, 0x15, 0x16, 0x28, 0xAE, 0xD2, 0xA6,
    0xAB, 0xF7, 0x15, 0x88, 0x09, 0xCF, 0x4F, 0x3C
};

void fw_crypto_init(fw_crypto_ctx_t *ctx, const uint8_t *iv)
{
    AES_init_ctx_iv(ctx, s_aes_key, iv);
}

void fw_crypto_xcrypt(fw_crypto_ctx_t *ctx, uint8_t *buf, uint32_t len)
{
    AES_CTR_xcrypt_buffer(ctx, buf, (size_t)len);
}
