#ifndef __FW_STORAGE_H__
#define __FW_STORAGE_H__

/*
 * fw_storage.h  –  W25Q128 dual-zone firmware backup and version rollback
 *
 * The W25Q128 (16 MB) is partitioned into two 1 MB backup zones:
 *
 *   Zone A  (FW_STORAGE_ZONE_A, base address 0x000000):
 *       Stores the most-recently confirmed firmware image.
 *
 *   Zone B  (FW_STORAGE_ZONE_B, base address 0x100000):
 *       Stores the previously confirmed firmware image.
 *
 * Each zone begins with a 256-byte header (one W25Q128 page) that
 * holds the firmware metadata and AES-CTR IV.  The encrypted
 * firmware binary follows immediately after the header.
 *
 * A monotonically-increasing generation counter is stored in each
 * header so the bootloader can determine which zone is newer without
 * needing to copy data between zones.  When a new backup is saved,
 * the zone with the lower generation number is overwritten.
 *
 * On automatic rollback the zone with the highest valid generation is
 * tried first; if that restore fails the other zone is attempted.
 */

#include <stdbool.h>
#include <stdint.h>

/** Zone indices */
#define FW_STORAGE_ZONE_A   0   /**< Most recent backup zone */
#define FW_STORAGE_ZONE_B   1   /**< Previous backup zone    */
#define FW_STORAGE_ZONES    2   /**< Total number of zones   */

/**
 * @brief Initialise the W25Q128 SPI flash for firmware storage.
 *
 * Must be called once during bootloader startup before any other
 * fw_storage_* function.
 */
void fw_storage_init(void);

/**
 * @brief Check whether a storage zone contains a valid firmware image.
 *
 * Validates the zone header magic, header CRC32, and basic size
 * constraints.  Does NOT verify the firmware payload CRC32 (that is
 * deferred to the restore operation).
 *
 * @param zone  FW_STORAGE_ZONE_A or FW_STORAGE_ZONE_B.
 * @return true if the zone header is intact.
 */
bool fw_storage_zone_validate(uint8_t zone);

/**
 * @brief Back up the current internal application to external flash.
 *
 * Reads the current application from the STM32 internal flash using
 * the existing magic header metadata, encrypts it with AES-128-CTR,
 * and writes it to the zone with the lower generation number (or the
 * first invalid zone found).
 *
 * The generation counter of the written zone is set to
 * max(gen_A, gen_B) + 1 so that the rollback path always knows which
 * image is newer.
 *
 * This function can take several seconds due to W25Q128 sector erase
 * times (~50 ms per 4 KB sector).
 *
 * @return true on success.
 */
bool fw_storage_backup_current_app(void);

/**
 * @brief Restore the best available firmware from external flash.
 *
 * Searches both zones for valid headers.  Tries the zone with the
 * highest generation number first.  For each candidate zone:
 *   1. Erases the internal flash magic-header sector and app sectors.
 *   2. Reads the encrypted firmware from the W25Q128, decrypts it,
 *      and writes it to the target address on internal flash.
 *   3. Verifies the CRC32 of the written firmware.
 *   4. Reconstructs and writes the magic header.
 *
 * If the caller receives true it MUST reboot the system (e.g. via
 * NVIC_SystemReset()) so the restored firmware is executed.
 *
 * @return true if at least one zone was successfully restored.
 */
bool fw_storage_auto_rollback(void);

/**
 * @brief Retrieve the version string stored in a zone header.
 *
 * @param zone    Zone index.
 * @param buf     Output buffer (NUL-terminated on success).
 * @param buflen  Size of @p buf in bytes.
 * @return true if the zone is valid and the version was copied.
 */
bool fw_storage_get_zone_version(uint8_t zone, char *buf, uint32_t buflen);

#endif /* __FW_STORAGE_H__ */
