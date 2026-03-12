#ifndef __FW_CRYPTO_H__
#define __FW_CRYPTO_H__

/*
 * fw_crypto.h  –  AES-128-CTR firmware encryption / decryption
 *
 * STM32F407VET6 does not have a hardware AES peripheral, so a
 * software implementation (TinyAES) is used instead.
 *
 * CTR mode is chosen because:
 *   - No padding required (works on any byte length)
 *   - Encryption and decryption are the same XOR operation
 *   - Safe for chunk-by-chunk (streaming) processing
 *
 * The AES key is defined in fw_crypto.c.
 * IMPORTANT: Replace the default key with a project-specific key
 *            before deploying to production hardware.
 */

#include <stdint.h>
#include "aes.h"

#define FW_CRYPTO_KEY_SIZE  16              /* AES-128: 16-byte key  */
#define FW_CRYPTO_IV_SIZE   AES_BLOCKLEN    /* 16-byte IV / nonce    */

/**
 * Opaque streaming context.
 * Maintain across successive calls to fw_crypto_xcrypt() so that the
 * AES-CTR counter is advanced correctly between firmware chunks.
 */
typedef struct AES_ctx fw_crypto_ctx_t;

/**
 * @brief Initialise an AES-128-CTR streaming context.
 *
 * Must be called once before the first fw_crypto_xcrypt() call for a
 * given encrypt/decrypt session.  The same @p iv must be used for
 * both backup (encrypt) and restore (decrypt).
 *
 * @param ctx  Pointer to the context to initialise.
 * @param iv   16-byte IV / nonce.
 */
void fw_crypto_init(fw_crypto_ctx_t *ctx, const uint8_t *iv);

/**
 * @brief AES-128-CTR encrypt or decrypt (identical operation).
 *
 * Processes @p buf in-place.  The context counter is advanced after
 * each call, so successive calls produce a contiguous keystream and
 * can be used to process a firmware image in chunks.
 *
 * @param ctx  Streaming context (updated after each call).
 * @param buf  Data to encrypt / decrypt in-place.
 * @param len  Number of bytes to process.
 */
void fw_crypto_xcrypt(fw_crypto_ctx_t *ctx, uint8_t *buf, uint32_t len);

#endif /* __FW_CRYPTO_H__ */
