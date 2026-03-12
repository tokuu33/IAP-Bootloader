#ifndef __FW_CRYPTO_H__
#define __FW_CRYPTO_H__

#include <stdint.h>
#include <stddef.h>

/* AES-128 key and IV sizes (in bytes) */
#define FW_CRYPTO_KEY_SIZE  16
#define FW_CRYPTO_IV_SIZE   16

/*
 * Default AES-128 key used for firmware encryption/decryption.
 * IMPORTANT: Replace this key before deploying to production.
 *            The identical key must be configured in the host-side
 *            firmware packaging/upload tool.
 */
extern const uint8_t fw_crypto_default_key[FW_CRYPTO_KEY_SIZE];

/**
 * @brief  Encrypt or decrypt data in-place using AES-128-CTR mode.
 *
 *         AES-CTR is a stream cipher: the same operation is used for both
 *         encryption and decryption.  The caller must ensure that the
 *         AES context (key + IV) used for decryption matches the one
 *         used during encryption.
 *
 *         When decrypting a large buffer in multiple successive calls with
 *         the same key/IV, use fw_crypto_ctr_xcrypt_continue() instead so
 *         that the internal counter is preserved across calls.
 *
 * @param  data    Buffer to process in-place.
 * @param  length  Number of bytes to process.
 * @param  key     16-byte AES-128 key.
 * @param  iv      16-byte initial vector (nonce / initial counter value).
 */
void fw_crypto_ctr_xcrypt(uint8_t *data, size_t length,
                          const uint8_t *key, const uint8_t *iv);

#endif /* __FW_CRYPTO_H__ */
