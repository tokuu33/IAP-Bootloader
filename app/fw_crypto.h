#ifndef __FW_CRYPTO_H__
#define __FW_CRYPTO_H__

#include <stdint.h>
#include <stdbool.h>

/**
 * @file fw_crypto.h
 * @brief Firmware AES-128-CBC encryption/decryption module.
 *
 * The firmware AES key is a compile-time constant embedded in the bootloader.
 * Override FW_CRYPTO_DEFAULT_KEY (as a 16-element uint8_t initialiser list)
 * at compile time to use a custom product-specific key.
 *
 * Usage sequence (host side):
 *   1. Host encrypts the firmware binary with AES-128-CBC using the shared key
 *      and a randomly chosen 16-byte IV.
 *   2. Host sends CRYPTO_INIT command (opcode 0x84) carrying the 16-byte IV.
 *   3. Host sends PROGRAM_ENC commands (opcode 0x85) with encrypted chunks.
 *      The bootloader decrypts each chunk using the running CBC context before
 *      writing to internal flash.
 *   4. Host sends VERIFY to confirm the decrypted image CRC.
 */

#define FW_CRYPTO_KEY_SIZE  16u   /**< AES-128 key length in bytes */
#define FW_CRYPTO_IV_SIZE   16u   /**< AES block / IV length in bytes */

/**
 * @brief Initialise the AES-128-CBC decryption context with the given IV.
 *        Must be called before any fw_crypto_decrypt() calls.
 *        The hardcoded key is used automatically.
 * @param iv Pointer to a 16-byte IV buffer.
 */
void fw_crypto_init(const uint8_t *iv);

/**
 * @brief Deactivate the crypto context and zero the key schedule.
 */
void fw_crypto_reset(void);

/**
 * @brief Decrypt a chunk of firmware data in-place (AES-128-CBC).
 *        The internal IV is updated after each call so that successive calls
 *        correctly process a continuous CBC stream.
 * @param data Pointer to the data buffer (modified in-place).
 * @param len  Number of bytes to decrypt (must be a multiple of 16).
 */
void fw_crypto_decrypt(uint8_t *data, uint32_t len);

/**
 * @brief Return true if fw_crypto_init() has been called and the context
 *        is active.
 */
bool fw_crypto_is_active(void);

#endif /* __FW_CRYPTO_H__ */
