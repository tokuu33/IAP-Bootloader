#ifndef __MAGIC_HEADER_H__
#define __MAGIC_HEADER_H__

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    MAGIC_HEADER_TYPE_APP = 0,
} magic_header_type_t;

/* ── Validation ─────────────────────────────────────────────────────────── */

/** Validate magic number and this_crc32 of the header in internal Flash. */
bool magic_header_validate(void);

/* ── Immutable app-binary fields (set by gen_xbin.py at packaging time) ── */

magic_header_type_t magic_header_get_type(void);
/** Offset of the app binary inside the .xbin file (always 256). */
uint32_t magic_header_get_offset(void);
/** Flash address where the app binary is programmed. */
uint32_t magic_header_get_address(void);
/** Size of the app binary in bytes. */
uint32_t magic_header_get_length(void);
/** CRC32 of the plaintext app binary. */
uint32_t magic_header_get_crc32(void);

/* ── Runtime OTA/rollback fields (writeable after initial programming) ─── */

/**
 * @brief  update_flag — non-zero when a new firmware image is waiting in
 *         W25Q128 to be flashed.  Set by the bootloader/app before reset to
 *         trigger an OTA apply on next boot.
 */
uint32_t magic_header_get_update_flag(void);

/**
 * @brief  rollback_flag — non-zero when the application requests a rollback to
 *         the backup zone.  Consumed and cleared by the bootloader.
 */
uint32_t magic_header_get_rollback_flag(void);

/**
 * @brief  boot_fail_count — number of consecutive failed boots.  The
 *         bootloader increments this on each unsuccessful boot attempt.
 */
uint32_t magic_header_get_boot_fail_count(void);

/* ── W25Q128 OTA image info (filled by bootloader after FW_COMMIT) ──────── */

/** Length of the new firmware image stored in W25Q128 Zone A/B. */
uint32_t magic_header_get_new_app_length(void);
/** CRC32 of the new firmware image in W25Q128 (plaintext). */
uint32_t magic_header_get_new_app_crc32(void);
/** Length of the firmware image stored in the backup zone. */
uint32_t magic_header_get_backup_length(void);
/** CRC32 of the firmware in the backup zone (plaintext). */
uint32_t magic_header_get_backup_crc32(void);

#endif /* __MAGIC_HEADER_H__ */
