#ifndef __BACKUP_MGR_H__
#define __BACKUP_MGR_H__

#include <stdint.h>
#include <stdbool.h>

/**
 * @file backup_mgr.h
 * @brief W25Q128 dual-zone firmware backup manager.
 *
 * The W25Q128 (128 Mbit = 16 MB) is partitioned into two independent backup
 * zones.  Each zone stores a complete copy of the application firmware together
 * with a metadata header.
 *
 * W25Q128 address map:
 * ┌─────────────────────────────────────────────────────────────────┐
 * │ Zone 0 – latest backup (most recently confirmed good firmware)  │
 * │   Header : 0x000000 – 0x000FFF  (1 × 4 KB sector)             │
 * │   Data   : 0x001000 – 0x07FFFF  (up to 508 KB)                │
 * ├─────────────────────────────────────────────────────────────────┤
 * │ Zone 1 – previous backup (rollback target)                      │
 * │   Header : 0x200000 – 0x200FFF  (1 × 4 KB sector)             │
 * │   Data   : 0x201000 – 0x27FFFF  (up to 508 KB)                │
 * └─────────────────────────────────────────────────────────────────┘
 *
 * Backup rotation policy (triggered from the BOOT command):
 *   - If zone 0 is valid, its content is copied to zone 1 first.
 *   - The current (just-programmed) firmware is then saved to zone 0.
 *   - Zone 1 therefore always holds the previous firmware for one-step rollback.
 *
 * Rollback:
 *   - The best valid zone is found (zone 0 preferred).
 *   - Its firmware is erased + reprogrammed into internal flash.
 *   - The internal magic header is rebuilt from the zone metadata.
 */

/** Number of backup zones. */
#define BACKUP_ZONE_COUNT           2u

/** Maximum firmware size storable per zone (bytes). */
#define BACKUP_ZONE_MAX_FW_SIZE     (512u * 1024u)

/** Zone 0 W25Q128 addresses. */
#define BACKUP_ZONE_0_HEADER_ADDR   0x000000UL
#define BACKUP_ZONE_0_DATA_ADDR     0x001000UL   /* 4 KB after header */

/** Zone 1 W25Q128 addresses (2 MB offset). */
#define BACKUP_ZONE_1_HEADER_ADDR   0x200000UL
#define BACKUP_ZONE_1_DATA_ADDR     0x201000UL

/**
 * @brief Initialise the backup manager (initialises the W25Q128 driver).
 *        Safe to call more than once.
 */
void backup_mgr_init(void);

/**
 * @brief Save firmware currently in internal flash to the specified zone.
 *
 * @param zone      Destination zone index (0 or 1).
 * @param fw_addr   Start address in internal flash (e.g. APP_BASE_ADDRESS).
 * @param fw_len    Firmware size in bytes (must be > 0 and ≤ BACKUP_ZONE_MAX_FW_SIZE).
 * @param fw_crc32  CRC32 of the firmware image (plain/decrypted).
 * @param version   NUL-terminated version string (max 127 characters).
 * @return true on success, false on invalid parameters.
 */
bool backup_mgr_save(uint8_t zone, uint32_t fw_addr, uint32_t fw_len,
                     uint32_t fw_crc32, const char *version);

/**
 * @brief Validate the metadata header of the specified zone.
 * @return true if the zone header is intact and the valid-flag is set.
 */
bool backup_mgr_validate(uint8_t zone);

/**
 * @brief Return the firmware length (bytes) stored in the specified zone.
 *        Returns 0 if the zone is invalid.
 */
uint32_t backup_mgr_get_length(uint8_t zone);

/**
 * @brief Return the version string stored in the specified zone.
 *        Points to an internal static buffer — copy if persistence is needed.
 *        Returns an empty string if the zone is invalid.
 */
const char *backup_mgr_get_version(uint8_t zone);

/**
 * @brief Restore firmware from the specified zone into internal flash.
 *
 * The function:
 *   1. Erases the target flash region.
 *   2. Programs the data from W25Q128 into internal flash.
 *   3. Verifies the CRC32 of the restored data.
 *
 * The caller must update the internal magic header afterwards
 * (backup_mgr_rollback() does this automatically).
 *
 * @param zone Zone index (0 or 1).
 * @return true on success (CRC verified).
 */
bool backup_mgr_restore(uint8_t zone);

/**
 * @brief Find the best valid zone to use for rollback.
 * @return Zone index (0 or 1), or -1 if no valid zone is found.
 */
int backup_mgr_find_best(void);

/**
 * @brief Perform a full automatic rollback:
 *        1. Finds the best valid backup zone.
 *        2. Restores the firmware to internal flash.
 *        3. Rewrites the internal magic header from backup metadata.
 * @return true if rollback and magic-header update succeeded.
 */
bool backup_mgr_rollback(void);

/**
 * @brief Rotate backups then save the current firmware to zone 0.
 *
 * If zone 0 currently holds valid firmware it is copied to zone 1 first,
 * preserving one-step rollback capability.  The new firmware is then
 * saved to zone 0.
 *
 * @param fw_addr   Start address of firmware in internal flash.
 * @param fw_len    Firmware size in bytes.
 * @param fw_crc32  CRC32 of the firmware image.
 * @param version   NUL-terminated version string.
 */
void backup_mgr_rotate_and_save(uint32_t fw_addr, uint32_t fw_len,
                                uint32_t fw_crc32, const char *version);

#endif /* __BACKUP_MGR_H__ */
