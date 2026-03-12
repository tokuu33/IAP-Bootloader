#ifndef __W25Q_BACKUP_H__
#define __W25Q_BACKUP_H__

#include <stdint.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------
 * Zone indices
 * ------------------------------------------------------------------------- */
#define W25Q_ZONE_A      0
#define W25Q_ZONE_B      1
#define W25Q_ZONE_COUNT  2

/* -------------------------------------------------------------------------
 * Zone status values (stored in zone_header_t.status)
 * ------------------------------------------------------------------------- */
/** Sector has been erased; no firmware stored yet. */
#define ZONE_STATUS_EMPTY    0xFFFFFFFFU
/** Zone contains a valid firmware backup (either as backup or as running). */
#define ZONE_STATUS_VALID    0xA5A5A5A5U
/** Zone header or data is corrupt; must be re-flashed before use. */
#define ZONE_STATUS_INVALID  0x00000000U

/* -------------------------------------------------------------------------
 * Zone header magic number ("ZVER" in ASCII, little-endian)
 * ------------------------------------------------------------------------- */
#define ZONE_HEADER_MAGIC   0x5A564552U

/* -------------------------------------------------------------------------
 * W25Q128 address map
 *
 *  Zone A  (0x000000 – 0x7FFFFF, 8 MB)
 *    Header sector : 0x000000 – 0x000FFF  (4 KB)
 *    Firmware data : 0x001000 – 0x7FFFFF  (~8 MB – 4 KB)
 *
 *  Zone B  (0x800000 – 0xFFFFFF, 8 MB)
 *    Header sector : 0x800000 – 0x800FFF  (4 KB)
 *    Firmware data : 0x801000 – 0xFFFFFF  (~8 MB – 4 KB)
 * ------------------------------------------------------------------------- */
#define ZONE_A_BASE_ADDR          0x000000U
#define ZONE_B_BASE_ADDR          0x800000U
#define ZONE_HEADER_SECTOR_SIZE   0x001000U   /* 4 KB */
#define ZONE_DATA_OFFSET          ZONE_HEADER_SECTOR_SIZE
#define ZONE_MAX_FW_SIZE          (0x800000U - ZONE_HEADER_SECTOR_SIZE)

/* -------------------------------------------------------------------------
 * Zone header – stored in the first 4 KB sector of each zone
 * ------------------------------------------------------------------------- */
typedef struct
{
    uint32_t magic;         /**< Must equal ZONE_HEADER_MAGIC            */
    uint32_t version;       /**< Firmware version number (e.g. 0x00010200
                                 for v1.2.0)                             */
    char     ver_str[32];   /**< Null-terminated version string,
                                 e.g. "v1.2.0"                          */
    uint32_t fw_size;       /**< Plain-text firmware size in bytes        */
    uint32_t fw_crc32;      /**< CRC32 of the plain-text firmware         */
    uint32_t enc_size;      /**< Encrypted data size in bytes
                                 (== fw_size for AES-CTR; no padding)    */
    uint32_t enc_crc32;     /**< CRC32 of the encrypted data in W25Q128   */
    uint8_t  iv[16];        /**< AES-128-CTR initial vector (nonce)       */
    uint32_t app_address;   /**< Target STM32 flash start address         */
    uint32_t status;        /**< One of ZONE_STATUS_*                     */
    uint32_t header_crc32;  /**< CRC32 of all fields above this one       */
} zone_header_t;

/* -------------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------------- */

/** Return the W25Q128 base address of the given zone. */
uint32_t w25q_backup_zone_base(uint8_t zone);

/** Return the W25Q128 address of the data area of the given zone. */
uint32_t w25q_backup_zone_data_addr(uint8_t zone);

/**
 * @brief  Erase the zone header sector and enough data sectors to hold
 *         @p data_size bytes of firmware.
 * @param  zone       Zone index (W25Q_ZONE_A or W25Q_ZONE_B).
 * @param  data_size  Number of bytes of firmware data to erase space for.
 */
void w25q_backup_erase_zone(uint8_t zone, uint32_t data_size);

/**
 * @brief  Write a zone header (erases the 4 KB header sector first).
 * @note   @p header->header_crc32 is computed and filled in automatically.
 */
bool w25q_backup_write_header(uint8_t zone, zone_header_t *header);

/** Read the raw zone header bytes from W25Q128 into @p header. */
bool w25q_backup_read_header(uint8_t zone, zone_header_t *header);

/** Return true if the zone header magic and CRC32 are valid. */
bool w25q_backup_validate_header(uint8_t zone);

/**
 * @brief  Write a chunk of (encrypted) firmware data to the zone data area.
 * @param  zone    Zone index.
 * @param  offset  Byte offset within the zone data area.
 * @param  data    Source buffer.
 * @param  len     Number of bytes to write.
 */
void w25q_backup_write_data(uint8_t zone, uint32_t offset,
                            const uint8_t *data, uint32_t len);

/**
 * @brief  Read a chunk of data from the zone data area.
 * @param  zone    Zone index.
 * @param  offset  Byte offset within the zone data area.
 * @param  data    Destination buffer.
 * @param  len     Number of bytes to read.
 */
void w25q_backup_read_data(uint8_t zone, uint32_t offset,
                           uint8_t *data, uint32_t len);

/**
 * @brief  Update the zone status field and rewrite the header sector.
 * @param  zone    Zone index.
 * @param  status  New status (ZONE_STATUS_VALID or ZONE_STATUS_INVALID).
 * @return true on success.
 */
bool w25q_backup_set_status(uint8_t zone, uint32_t status);

/**
 * @brief  Read and return the zone status.
 * @return ZONE_STATUS_INVALID if the header cannot be read.
 */
uint32_t w25q_backup_get_status(uint8_t zone);

/**
 * @brief  Decrypt firmware stored in @p zone and install it into STM32 flash.
 *
 *         Steps performed:
 *           1. Read and validate the zone header.
 *           2. Erase the target STM32 flash region.
 *           3. Read encrypted data from W25Q128 in 4 KB chunks, decrypt
 *              each chunk with AES-128-CTR, and write to STM32 flash.
 *           4. Verify CRC32 of installed firmware.
 *           5. Update the STM32 magic header.
 *           6. Mark the zone as ZONE_STATUS_VALID.
 *
 * @param  zone  Zone index (W25Q_ZONE_A or W25Q_ZONE_B).
 * @return true on success, false on any validation or write error.
 */
bool w25q_backup_install(uint8_t zone);

/**
 * @brief  Find a VALID zone suitable for rollback (different from the
 *         currently running firmware version).
 *
 * @param  current_ver  Numeric version of the currently running firmware
 *                      (from magic_header_get_version()).  Pass 0 to
 *                      return the first VALID zone regardless of version.
 * @return Zone index (W25Q_ZONE_A or W25Q_ZONE_B) on success, -1 if no
 *         suitable rollback zone was found.
 */
int w25q_backup_find_rollback_zone(uint32_t current_ver);

#endif /* __W25Q_BACKUP_H__ */
