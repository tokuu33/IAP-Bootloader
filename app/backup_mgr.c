#include "backup_mgr.h"
#include "w25q128.h"
#include "stm32_flash.h"
#include "magic_header.h"
#include "crc32.h"
#include "utils.h"
#include <string.h>

#define LOG_TAG    "backup"
#define LOG_LVL    ELOG_LVL_INFO
#include "elog.h"

/* Magic identifier for a backup zone header ("BAKU"). */
#define BACKUP_ZONE_MAGIC   0x42414B55UL

/* Flags stored in backup_zone_header_t::flags. */
#define BACKUP_FLAG_VALID   (1u << 0)   /**< Zone contains a valid firmware image. */

/** W25Q128 sector size (erase granularity). */
#define W25Q_SECTOR_SIZE    4096u

/**
 * Metadata header stored at the beginning of each backup zone.
 * The CRC32 covers every field *before* this_crc32.
 */
typedef struct
{
    uint32_t magic;         /**< BACKUP_ZONE_MAGIC                          */
    uint32_t flags;         /**< BACKUP_FLAG_VALID etc.                     */
    uint32_t fw_address;    /**< Original internal-flash start address      */
    uint32_t fw_length;     /**< Firmware image length in bytes             */
    uint32_t fw_crc32;      /**< CRC32 of the firmware image                */
    char     version[128];  /**< NUL-terminated version string              */
    uint32_t reserved[6];   /**< Reserved for future use                    */
    uint32_t this_crc32;    /**< CRC32 of all fields above this one         */
} backup_zone_header_t;

/* Address map for each zone. */
static const struct
{
    uint32_t header_addr;
    uint32_t data_addr;
} s_zone_map[BACKUP_ZONE_COUNT] =
{
    { BACKUP_ZONE_0_HEADER_ADDR, BACKUP_ZONE_0_DATA_ADDR },
    { BACKUP_ZONE_1_HEADER_ADDR, BACKUP_ZONE_1_DATA_ADDR },
};

/* Temporary copy buffer (used for flash <-> W25Q128 transfers). */
static uint8_t s_copy_buf[W25Q_SECTOR_SIZE];

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static void zone_read_header(uint8_t zone, backup_zone_header_t *hdr)
{
    w25qxx_read(s_zone_map[zone].header_addr, (uint8_t *)hdr,
                sizeof(backup_zone_header_t));
}

static void zone_write_header(uint8_t zone, const backup_zone_header_t *hdr)
{
    /* Erase the 4 KB header sector before writing. */
    w25qxx_erase_sector(s_zone_map[zone].header_addr);
    w25qxx_write(s_zone_map[zone].header_addr, (uint8_t *)hdr,
                 sizeof(backup_zone_header_t));
}

/** Compute the CRC32 that covers all header fields except this_crc32. */
static uint32_t calc_header_crc(const backup_zone_header_t *hdr)
{
    return crc32((const uint8_t *)hdr,
                 offset_of(backup_zone_header_t, this_crc32));
}

/** Return true if the address range [addr, addr+len) lies within internal flash. */
static bool is_valid_flash_range(uint32_t addr, uint32_t len)
{
    return (addr >= STM32_FLASH_BASE) &&
           (len  >  0u) &&
           (addr + len <= STM32_FLASH_BASE + STM32_FLASH_SIZE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

void backup_mgr_init(void)
{
    w25qxx_init();
    log_i("Backup manager initialised");
}

bool backup_mgr_validate(uint8_t zone)
{
    if (zone >= BACKUP_ZONE_COUNT)
        return false;

    backup_zone_header_t hdr;
    zone_read_header(zone, &hdr);

    if (hdr.magic != BACKUP_ZONE_MAGIC)
        return false;

    if (!(hdr.flags & BACKUP_FLAG_VALID))
        return false;

    if (hdr.fw_length == 0 || hdr.fw_length > BACKUP_ZONE_MAX_FW_SIZE)
        return false;

    /* Validate that the stored firmware address is a legal flash region. */
    if (!is_valid_flash_range(hdr.fw_address, hdr.fw_length))
        return false;

    uint32_t ccrc = calc_header_crc(&hdr);
    if (ccrc != hdr.this_crc32)
        return false;

    return true;
}

uint32_t backup_mgr_get_length(uint8_t zone)
{
    if (!backup_mgr_validate(zone))
        return 0u;

    backup_zone_header_t hdr;
    zone_read_header(zone, &hdr);
    return hdr.fw_length;
}

const char *backup_mgr_get_version(uint8_t zone)
{
    /* NOTE: returns a pointer to a module-internal static buffer.
     * The buffer is overwritten on every call.  Copy the result if you
     * need it to survive across multiple calls. */
    static backup_zone_header_t s_hdr;

    zone_read_header(zone, &s_hdr);
    s_hdr.version[sizeof(s_hdr.version) - 1] = '\0';
    return s_hdr.version;
}

bool backup_mgr_save(uint8_t zone, uint32_t fw_addr, uint32_t fw_len,
                     uint32_t fw_crc32, const char *version)
{
    if (zone >= BACKUP_ZONE_COUNT)
    {
        log_e("Invalid zone %u", zone);
        return false;
    }
    if (fw_len == 0 || fw_len > BACKUP_ZONE_MAX_FW_SIZE)
    {
        log_e("Firmware length %u out of range", fw_len);
        return false;
    }

    log_i("Saving firmware to backup zone %u: addr=0x%08X len=%u",
          zone, fw_addr, fw_len);

    /* 1. Erase all data sectors needed for this firmware. */
    uint32_t sectors_needed = (fw_len + W25Q_SECTOR_SIZE - 1u) / W25Q_SECTOR_SIZE;
    for (uint32_t s = 0; s < sectors_needed; s++)
        w25qxx_erase_sector(s_zone_map[zone].data_addr + s * W25Q_SECTOR_SIZE);

    /* 2. Copy firmware from internal flash to W25Q128 in sector-sized chunks. */
    uint32_t remaining = fw_len;
    uint32_t src       = fw_addr;
    uint32_t dst       = s_zone_map[zone].data_addr;

    while (remaining > 0)
    {
        uint32_t chunk = (remaining > sizeof(s_copy_buf))
                       ? (uint32_t)sizeof(s_copy_buf) : remaining;
        /* Internal flash is memory-mapped – cast address directly to pointer. */
        memcpy(s_copy_buf, (const void *)src, chunk);
        w25qxx_write(dst, s_copy_buf, chunk);
        src       += chunk;
        dst       += chunk;
        remaining -= chunk;
    }

    /* 3. Build and write the zone metadata header. */
    backup_zone_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic      = BACKUP_ZONE_MAGIC;
    hdr.flags      = BACKUP_FLAG_VALID;
    hdr.fw_address = fw_addr;
    hdr.fw_length  = fw_len;
    hdr.fw_crc32   = fw_crc32;
    if (version)
        strncpy(hdr.version, version, sizeof(hdr.version) - 1u);
    hdr.this_crc32 = calc_header_crc(&hdr);

    zone_write_header(zone, &hdr);

    log_i("Backup zone %u saved: version=%s crc32=0x%08X",
          zone, hdr.version, hdr.fw_crc32);
    return true;
}

bool backup_mgr_restore(uint8_t zone)
{
    if (!backup_mgr_validate(zone))
    {
        log_e("Zone %u is not valid, cannot restore", zone);
        return false;
    }

    backup_zone_header_t hdr;
    zone_read_header(zone, &hdr);

    log_i("Restoring from backup zone %u: addr=0x%08X len=%u ver=%s",
          zone, hdr.fw_address, hdr.fw_length, hdr.version);

    /* 1. Erase the target region in internal flash. */
    stm32_flash_unlock();
    stm32_flash_erase(hdr.fw_address, hdr.fw_length);

    /* 2. Copy data from W25Q128 to internal flash in sector-sized chunks. */
    uint32_t remaining = hdr.fw_length;
    uint32_t src       = s_zone_map[zone].data_addr;
    uint32_t dst       = hdr.fw_address;

    while (remaining > 0)
    {
        uint32_t chunk = (remaining > sizeof(s_copy_buf))
                       ? (uint32_t)sizeof(s_copy_buf) : remaining;
        w25qxx_read(src, s_copy_buf, chunk);
        stm32_flash_program(dst, s_copy_buf, chunk);
        src       += chunk;
        dst       += chunk;
        remaining -= chunk;
    }

    stm32_flash_lock();

    /* 3. Verify the restored image by CRC32.
     * hdr.fw_address has already been validated by backup_mgr_validate(). */
    uint32_t ccrc = crc32((const uint8_t *)hdr.fw_address, hdr.fw_length);
    if (ccrc != hdr.fw_crc32)
    {
        log_e("Restore CRC32 mismatch: got 0x%08X, expected 0x%08X",
              ccrc, hdr.fw_crc32);
        return false;
    }

    log_i("Restore succeeded (CRC32=0x%08X)", ccrc);
    return true;
}

int backup_mgr_find_best(void)
{
    /* Zone 0 is the most recent backup — prefer it for rollback. */
    if (backup_mgr_validate(0))
        return 0;
    if (backup_mgr_validate(1))
        return 1;
    return -1;
}

bool backup_mgr_rollback(void)
{
    int zone = backup_mgr_find_best();
    if (zone < 0)
    {
        log_e("No valid backup zone found for rollback");
        return false;
    }

    log_w("Rolling back firmware from zone %d", zone);

    if (!backup_mgr_restore((uint8_t)zone))
        return false;

    /* Rebuild the internal magic header from the backup zone metadata. */
    backup_zone_header_t hdr;
    zone_read_header((uint8_t)zone, &hdr);

    if (!magic_header_write(hdr.fw_address, hdr.fw_length,
                            hdr.fw_crc32, hdr.version))
    {
        log_e("Failed to update magic header after rollback");
        return false;
    }

    log_i("Rollback complete (zone %d, ver=%s)", zone, hdr.version);
    return true;
}

void backup_mgr_rotate_and_save(uint32_t fw_addr, uint32_t fw_len,
                                uint32_t fw_crc32, const char *version)
{
    /* If zone 0 already has valid firmware, copy it to zone 1 first. */
    if (backup_mgr_validate(0))
    {
        backup_zone_header_t hdr0;
        zone_read_header(0, &hdr0);

        log_i("Rotating backup: zone 0 (ver=%s) -> zone 1", hdr0.version);

        uint32_t len      = hdr0.fw_length;
        uint32_t sectors  = (len + W25Q_SECTOR_SIZE - 1u) / W25Q_SECTOR_SIZE;

        /* Erase zone 1 data sectors. */
        for (uint32_t s = 0; s < sectors; s++)
            w25qxx_erase_sector(BACKUP_ZONE_1_DATA_ADDR + s * W25Q_SECTOR_SIZE);

        /* Copy zone 0 data to zone 1. */
        uint32_t remaining = len;
        uint32_t s0        = BACKUP_ZONE_0_DATA_ADDR;
        uint32_t s1        = BACKUP_ZONE_1_DATA_ADDR;

        while (remaining > 0)
        {
            uint32_t chunk = (remaining > sizeof(s_copy_buf))
                           ? (uint32_t)sizeof(s_copy_buf) : remaining;
            w25qxx_read(s0, s_copy_buf, chunk);
            w25qxx_write(s1, s_copy_buf, chunk);
            s0        += chunk;
            s1        += chunk;
            remaining -= chunk;
        }

        /* Rewrite zone 1 header (same metadata as zone 0). */
        hdr0.this_crc32 = calc_header_crc(&hdr0);
        zone_write_header(1, &hdr0);
    }

    /* Save the new (current) firmware to zone 0. */
    backup_mgr_save(0, fw_addr, fw_len, fw_crc32, version);
}
