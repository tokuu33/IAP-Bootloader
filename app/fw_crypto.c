#include "fw_crypto.h"
#include "aes.h"
#include <string.h>

/**
 * Default AES-128 key – MUST be changed for your product.
 * The same key must be configured in the firmware-signing / encryption tool
 * on the host side.
 *
 * Override at compile time:
 *   -DFW_CRYPTO_DEFAULT_KEY="{0x11,0x22,...}"
 */
#ifndef FW_CRYPTO_DEFAULT_KEY
#define FW_CRYPTO_DEFAULT_KEY \
    { 0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6, \
      0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c }
#endif

static const uint8_t s_fw_key[FW_CRYPTO_KEY_SIZE] = FW_CRYPTO_DEFAULT_KEY;

static struct AES_ctx s_aes_ctx;
static bool           s_active = false;

void fw_crypto_init(const uint8_t *iv)
{
    AES_init_ctx_iv(&s_aes_ctx, s_fw_key, iv);
    s_active = true;
}

void fw_crypto_reset(void)
{
    memset(&s_aes_ctx, 0, sizeof(s_aes_ctx));
    s_active = false;
}

void fw_crypto_decrypt(uint8_t *data, uint32_t len)
{
    if (!s_active || len == 0)
        return;

    /* AES-CBC requires the length to be a multiple of the block size. */
    if (len % AES_BLOCKLEN != 0)
    {
        /* Caller must pad to a block boundary before encrypting.
         * Decrypting a non-block-aligned buffer would silently corrupt data. */
        return;
    }

    AES_CBC_decrypt_buffer(&s_aes_ctx, data, (size_t)len);
}

bool fw_crypto_is_active(void)
{
    return s_active;
}
