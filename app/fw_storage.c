#include <string.h>
#include "fw_storage.h"
#include "fw_crypto.h"
#include "w25q128.h"
#include "stm32_flash.h"
#include "magic_header.h"
#include "crc32.h"
#include "utils.h"

#define LOG_TAG   "fw_storage"
#define LOG_LVL   ELOG_LVL_INFO
#include "elog.h"

/* ------------------------------------------------------------------ */
/* W25Q128 zone geometry                                               */
/* ------------------------------------------------------------------ */
#define FW_ZONE_SIZE        (1024UL * 1024UL)   /* 1 MB per zone          */
#define FW_ZONE_0_BASE      0x000000UL          /* Zone A base address    */
#define FW_ZONE_1_BASE      0x100000UL          /* Zone B base address    */

/* Zone header occupies the first 256 bytes (one W25Q128 page).        */
#define FW_HEADER_SIZE      256U
#define FW_DATA_OFFSET      FW_HEADER_SIZE

/* Maximum firmware payload that fits in a single zone.               */
#define FW_MAX_SIZE         (FW_ZONE_SIZE - FW_HEADER_SIZE)

/* W25Q128 erase / program granularity.                                */
#define W25Q_SECTOR_SIZE    4096U
#define W25Q_PAGE_SIZE      256U

/* Chunk size for streaming encrypt / decrypt loop.                    *
 * Must be a multiple of AES_BLOCKLEN (16).                            *
 * 256 bytes = 16 AES blocks = 1 W25Q128 page → ideal granularity.    */
#define CHUNK_SIZE          256U

/* ------------------------------------------------------------------ */
/* Firmware storage header  (exactly 256 bytes, one W25Q128 page)     */
/* ------------------------------------------------------------------ */
#define FW_STORAGE_MAGIC    0x46574F4DUL   /* "FWOM" */
#define FW_VERSION_LEN      32U

/*
 * Memory layout (offsets):
 *   0   magic          (4 B)
 *   4   generation     (4 B)
 *   8   fw_length      (4 B)
 *  12   fw_crc32       (4 B)
 *  16   fw_addr        (4 B)
 *  20   iv[16]        (16 B)
 *  36   version[32]   (32 B)
 *  68   _reserved[184](184 B)
 * 252   header_crc32   (4 B)
 * ──────────────────────────
 * 256 bytes total
 */
typedef struct {
    uint32_t magic;                     /* offset   0 */
    uint32_t generation;                /* offset   4 – monotonically inc */
    uint32_t fw_length;                 /* offset   8 – plaintext bytes   */
    uint32_t fw_crc32;                  /* offset  12 – plaintext CRC32   */
    uint32_t fw_addr;                   /* offset  16 – target STM32 addr */
    uint8_t  iv[FW_CRYPTO_IV_SIZE];     /* offset  20 – AES-128-CTR nonce */
    char     version[FW_VERSION_LEN];   /* offset  36 – NUL-terminated    */
    uint8_t  _reserved[184];            /* offset  68 – pad to 252        */
    uint32_t header_crc32;              /* offset 252 – CRC of bytes 0–251*/
} fw_storage_header_t;                  /* total = 256 bytes              */

/* Compile-time size assertion (C99 via array trick) */
typedef char fw_storage_header_size_assert[(sizeof(fw_storage_header_t) == 256U) ? 1 : -1];

/* ------------------------------------------------------------------ */
/* Scratch buffer for streaming encrypt / decrypt                      */
/* ------------------------------------------------------------------ */
static uint8_t s_chunk[CHUNK_SIZE];

/* ------------------------------------------------------------------ */
/* Helper: return zone base address                                    */
/* ------------------------------------------------------------------ */
static uint32_t zone_base(uint8_t zone)
{
    return (zone == FW_STORAGE_ZONE_A) ? FW_ZONE_0_BASE : FW_ZONE_1_BASE;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void fw_storage_init(void)
{
    w25qxx_init();
    log_i("W25Q128 ready.");
}

bool fw_storage_zone_validate(uint8_t zone)
{
    fw_storage_header_t hdr;
    w25qxx_read(zone_base(zone), (uint8_t *)&hdr, sizeof(hdr));

    if (hdr.magic != FW_STORAGE_MAGIC)
        return false;

    uint32_t calc = crc32((uint8_t *)&hdr,
                          offset_of(fw_storage_header_t, header_crc32));
    if (calc != hdr.header_crc32)
        return false;

    if (hdr.fw_length == 0 || hdr.fw_length > FW_MAX_SIZE)
        return false;

    return true;
}

bool fw_storage_get_zone_version(uint8_t zone, char *buf, uint32_t buflen)
{
    if (!fw_storage_zone_validate(zone))
        return false;

    fw_storage_header_t hdr;
    w25qxx_read(zone_base(zone), (uint8_t *)&hdr, sizeof(hdr));

    strncpy(buf, hdr.version, buflen - 1U);
    buf[buflen - 1U] = '\0';
    return true;
}

bool fw_storage_backup_current_app(void)
{
    /* ---- Validate the source (current app in internal flash) ---- */
    if (!magic_header_validate()) {
        log_e("No valid magic header, cannot backup.");
        return false;
    }

    uint32_t fw_addr   = magic_header_get_address();
    uint32_t fw_length = magic_header_get_length();
    uint32_t fw_crc32  = magic_header_get_crc32();
    const char *fw_ver = magic_header_get_version();

    if (fw_length == 0 || fw_length > FW_MAX_SIZE) {
        log_e("Firmware length %lu out of range.", fw_length);
        return false;
    }

    /* ---- Read generation counters from both zones --------------- */
    fw_storage_header_t hdr;
    uint32_t gen[FW_STORAGE_ZONES]  = {0, 0};
    bool     valid[FW_STORAGE_ZONES] = {false, false};

    for (uint8_t z = 0; z < FW_STORAGE_ZONES; z++) {
        valid[z] = fw_storage_zone_validate(z);
        if (valid[z]) {
            w25qxx_read(zone_base(z), (uint8_t *)&hdr, sizeof(hdr));
            gen[z] = hdr.generation;
        }
    }

    /* ---- Select target zone ------------------------------------- *
     * Prefer an invalid zone; if both are valid, overwrite the one  *
     * with the lower generation number (i.e. the older backup).     */
    uint8_t  target;
    uint32_t new_gen;

    if (!valid[FW_STORAGE_ZONE_A]) {
        target = FW_STORAGE_ZONE_A;
    } else if (!valid[FW_STORAGE_ZONE_B]) {
        target = FW_STORAGE_ZONE_B;
    } else {
        target = (gen[FW_STORAGE_ZONE_A] <= gen[FW_STORAGE_ZONE_B])
                 ? FW_STORAGE_ZONE_A : FW_STORAGE_ZONE_B;
    }
    new_gen = ((gen[0] > gen[1]) ? gen[0] : gen[1]) + 1U;

    uint32_t zbase = zone_base(target);

    log_i("Backup: fw@0x%08lX len=%lu ver=%s -> zone %d (gen %lu)",
          fw_addr, fw_length, fw_ver ? fw_ver : "?", target, new_gen);

    /* Derive AES-CTR IV from CRC32 and generation.
     * STM32 is little-endian; memcpy copies bytes in native order.
     * IV bytes  0..(sizeof(uint32_t)-1)             : fw_crc32 (LE)
     * IV bytes  sizeof(uint32_t)..(2*sizeof-1)      : new_gen  (LE)
     * IV bytes  2*sizeof(uint32_t)..AES_BLOCKLEN-1  : 0x00          */
    uint8_t iv[FW_CRYPTO_IV_SIZE] = {0};
    memcpy(iv,                    &fw_crc32, sizeof(uint32_t));
    memcpy(iv + sizeof(uint32_t), &new_gen,  sizeof(uint32_t));

    /* ---- Erase only the sectors actually needed ----------------- */
    uint32_t total_bytes    = (uint32_t)FW_HEADER_SIZE + fw_length;
    uint32_t sectors_needed = (total_bytes + W25Q_SECTOR_SIZE - 1U)
                              / W25Q_SECTOR_SIZE;
    log_i("Erasing %lu sector(s) in zone %d...", sectors_needed, target);
    for (uint32_t i = 0; i < sectors_needed; i++) {
        w25qxx_erase_sector(zbase + i * W25Q_SECTOR_SIZE);
    }

    /* ---- Encrypt and write firmware in CHUNK_SIZE chunks -------- */
    fw_crypto_ctx_t ctx;
    fw_crypto_init(&ctx, iv);

    uint32_t processed = 0;
    while (processed < fw_length) {
        uint32_t chunk_size = fw_length - processed;
        if (chunk_size > CHUNK_SIZE) chunk_size = CHUNK_SIZE;

        /* Read plaintext from internal flash (memory-mapped)        */
        memcpy(s_chunk, (const uint8_t *)(fw_addr + processed), chunk_size);

        /* Encrypt in-place; ctx advances counter for next chunk     */
        fw_crypto_xcrypt(&ctx, s_chunk, chunk_size);

        /* Write encrypted chunk to W25Q128                         */
        w25qxx_write(zbase + FW_DATA_OFFSET + processed, s_chunk, chunk_size);

        processed += chunk_size;
    }

    /* ---- Write header last (marks backup as complete) ----------- */
    fw_storage_header_t new_hdr;
    memset(&new_hdr, 0, sizeof(new_hdr));
    new_hdr.magic      = FW_STORAGE_MAGIC;
    new_hdr.generation = new_gen;
    new_hdr.fw_length  = fw_length;
    new_hdr.fw_crc32   = fw_crc32;
    new_hdr.fw_addr    = fw_addr;
    memcpy(new_hdr.iv, iv, FW_CRYPTO_IV_SIZE);
    if (fw_ver != NULL) {
        strncpy(new_hdr.version, fw_ver, FW_VERSION_LEN - 1U);
    }
    new_hdr.header_crc32 = crc32((uint8_t *)&new_hdr,
                                 offset_of(fw_storage_header_t, header_crc32));
    w25qxx_write(zbase, (uint8_t *)&new_hdr, sizeof(new_hdr));

    /* ---- Verify the written header ------------------------------ */
    if (!fw_storage_zone_validate(target)) {
        log_e("Backup verify failed for zone %d.", target);
        return false;
    }

    log_i("Backup to zone %d complete.", target);
    return true;
}

/* ------------------------------------------------------------------ */
/* Internal: restore a single zone to internal flash                   */
/* ------------------------------------------------------------------ */
static bool restore_zone(uint8_t zone)
{
    fw_storage_header_t hdr;
    w25qxx_read(zone_base(zone), (uint8_t *)&hdr, sizeof(hdr));

    uint32_t fw_addr   = hdr.fw_addr;
    uint32_t fw_length = hdr.fw_length;
    uint32_t fw_crc32  = hdr.fw_crc32;

    log_i("Restore zone %d: fw@0x%08lX len=%lu gen=%lu ver=%s",
          zone, fw_addr, fw_length, hdr.generation, hdr.version);

    /* Basic sanity: destination must be within the app region       */
    if (fw_addr < APP_BASE_ADDRESS ||
        fw_addr + fw_length > STM32_FLASH_BASE + STM32_FLASH_SIZE) {
        log_e("Restore address 0x%08lX len=%lu out of range.",
              fw_addr, fw_length);
        return false;
    }

    /* ---- Erase internal flash: magic-header sector + app area --- */
    stm32_flash_unlock();
    stm32_flash_erase(MAGIC_HEADER_ADDRESS, 16U * 1024U); /* sector 3 */
    stm32_flash_erase(fw_addr, fw_length);                /* app sectors */

    /* ---- Decrypt and write firmware in CHUNK_SIZE chunks -------- */
    fw_crypto_ctx_t ctx;
    fw_crypto_init(&ctx, hdr.iv);

    uint32_t processed = 0;
    while (processed < fw_length) {
        uint32_t chunk_size = fw_length - processed;
        if (chunk_size > CHUNK_SIZE) chunk_size = CHUNK_SIZE;

        /* Read encrypted chunk from W25Q128                        */
        w25qxx_read(zone_base(zone) + FW_DATA_OFFSET + processed,
                    s_chunk, chunk_size);

        /* Decrypt in-place                                         */
        fw_crypto_xcrypt(&ctx, s_chunk, chunk_size);

        /* Round up to word boundary required by STM32 flash write  */
        uint32_t write_size = (chunk_size + 3U) & ~3U;
        if (write_size > chunk_size) {
            /* Pad extra bytes with 0xFF (erased flash state)       */
            memset(s_chunk + chunk_size, 0xFF, write_size - chunk_size);
        }

        stm32_flash_program(fw_addr + processed, s_chunk, write_size);

        processed += chunk_size;
    }
    stm32_flash_lock();

    /* ---- Verify CRC32 of restored firmware ---------------------- */
    uint32_t calc_crc = crc32((uint8_t *)fw_addr, fw_length);
    if (calc_crc != fw_crc32) {
        log_e("Restore CRC mismatch: calculated 0x%08lX, expected 0x%08lX.",
              calc_crc, fw_crc32);
        return false;
    }

    /* ---- Reconstruct magic header ------------------------------- */
    if (!magic_header_write(fw_addr, fw_length, fw_crc32, hdr.version)) {
        log_e("Failed to write magic header after restore.");
        return false;
    }

    log_i("Restore from zone %d complete.", zone);
    return true;
}

bool fw_storage_auto_rollback(void)
{
    fw_storage_header_t hdr;
    uint32_t gen[FW_STORAGE_ZONES]  = {0, 0};
    bool     valid[FW_STORAGE_ZONES] = {false, false};

    for (uint8_t z = 0; z < FW_STORAGE_ZONES; z++) {
        valid[z] = fw_storage_zone_validate(z);
        if (valid[z]) {
            w25qxx_read(zone_base(z), (uint8_t *)&hdr, sizeof(hdr));
            gen[z] = hdr.generation;
            log_i("Zone %d: valid, gen=%lu, ver=%s", z, gen[z], hdr.version);
        } else {
            log_i("Zone %d: no valid backup.", z);
        }
    }

    if (!valid[0] && !valid[1]) {
        log_e("No valid backup found in either zone.");
        return false;
    }

    /* Try the zone with the highest generation first               */
    uint8_t order[FW_STORAGE_ZONES];
    if (!valid[0]) {
        order[0] = 1U; order[1] = 0U;
    } else if (!valid[1]) {
        order[0] = 0U; order[1] = 1U;
    } else if (gen[0] >= gen[1]) {
        order[0] = 0U; order[1] = 1U;
    } else {
        order[0] = 1U; order[1] = 0U;
    }

    for (uint8_t i = 0; i < FW_STORAGE_ZONES; i++) {
        uint8_t z = order[i];
        if (!valid[z])
            continue;
        log_i("Attempting rollback from zone %d...", z);
        if (restore_zone(z))
            return true;
        log_w("Rollback from zone %d failed, trying next zone.", z);
    }

    log_e("All rollback attempts failed.");
    return false;
}
