#include <string.h>
#include "w25q_backup.h"
#include "w25q128.h"
#include "fw_crypto.h"
#include "stm32_flash.h"
#include "magic_header.h"
#include "crc32.h"
#include "aes.h"
#include "utils.h"

#define LOG_TAG  "w25q_backup"
#define LOG_LVL  ELOG_LVL_INFO
#include "elog.h"

/* Internal chunk size for read/decrypt/write loops (must be a multiple of
 * AES_BLOCKLEN = 16 so AES-CTR counter boundaries are consistent). */
#define CHUNK_SIZE  4096U

/* Module-level buffer to avoid large stack allocations. */
static uint8_t s_chunk_buf[CHUNK_SIZE];

/* -------------------------------------------------------------------------
 * Helper: compute CRC32 over data stored in W25Q128 (not memory-mapped)
 * by reading in CHUNK_SIZE chunks.
 * ------------------------------------------------------------------------- */
static uint32_t w25q_compute_crc(uint32_t w25q_addr, uint32_t length)
{
    /* Replicate the crc32() algorithm incrementally without needing the
     * look-up table symbol.  We read chunks, accumulate via the standard
     * IEEE-802.3 reflected CRC32 (same polynomial as crc32.c). */
    static const uint32_t tab[16] = {
        0x00000000UL, 0x1DB71064UL, 0x3B6E20C8UL, 0x26D930ACUL,
        0x76DC4190UL, 0x6B6B51F4UL, 0x4DB26158UL, 0x5005713CUL,
        0xEDB88320UL, 0xF00F9344UL, 0xD6D6A3E8UL, 0xCB61B38CUL,
        0x9B64C2B0UL, 0x86D3D2D4UL, 0xA00AE278UL, 0xBDBDF21CUL
    };

    uint32_t crc = 0xFFFFFFFFUL;
    uint32_t remaining = length;
    uint32_t offset = 0;

    while (remaining > 0)
    {
        uint32_t chunk = (remaining > CHUNK_SIZE) ? CHUNK_SIZE : remaining;
        w25qxx_read(w25q_addr + offset, s_chunk_buf, chunk);
        for (uint32_t i = 0; i < chunk; i++)
        {
            uint8_t b = s_chunk_buf[i];
            crc = (crc >> 4) ^ tab[(crc ^ b)        & 0x0F];
            crc = (crc >> 4) ^ tab[(crc ^ (b >> 4)) & 0x0F];
        }
        offset += chunk;
        remaining -= chunk;
    }
    return crc ^ 0xFFFFFFFFUL;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

uint32_t w25q_backup_zone_base(uint8_t zone)
{
    return (zone == W25Q_ZONE_A) ? ZONE_A_BASE_ADDR : ZONE_B_BASE_ADDR;
}

uint32_t w25q_backup_zone_data_addr(uint8_t zone)
{
    return w25q_backup_zone_base(zone) + ZONE_DATA_OFFSET;
}

void w25q_backup_erase_zone(uint8_t zone, uint32_t data_size)
{
    uint32_t base  = w25q_backup_zone_base(zone);
    /* Total bytes to erase: 4 KB header + rounded-up data area */
    uint32_t total = ZONE_HEADER_SECTOR_SIZE
                   + ((data_size + 0xFFFU) & ~0xFFFU);

    log_i("Erasing zone %u: base=0x%06X, total=%u bytes (%u sectors)",
          zone, base, total, total / 4096);

    for (uint32_t off = 0; off < total; off += 4096U)
        w25qxx_erase_sector(base + off);
}

bool w25q_backup_write_header(uint8_t zone, zone_header_t *header)
{
    uint32_t base = w25q_backup_zone_base(zone);

    /* Compute and fill header CRC32 (covers all fields up to but NOT including
     * header_crc32 itself, consistent with magic_header.c's self-CRC pattern). */
    header->header_crc32 = crc32((const unsigned char *)header,
                                 offset_of(zone_header_t, header_crc32));

    /* Erase the 4 KB header sector. */
    w25qxx_erase_sector(base);

    /* Write the header. */
    w25qxx_write(base, (uint8_t *)header, sizeof(zone_header_t));

    log_d("Zone %u header written: magic=0x%08X, ver=%u, fw_size=%u, status=0x%08X",
          zone, header->magic, header->version,
          header->fw_size, header->status);
    return true;
}

bool w25q_backup_read_header(uint8_t zone, zone_header_t *header)
{
    w25qxx_read(w25q_backup_zone_base(zone),
                (uint8_t *)header, sizeof(zone_header_t));
    return true;
}

bool w25q_backup_validate_header(uint8_t zone)
{
    zone_header_t hdr;
    w25q_backup_read_header(zone, &hdr);

    if (hdr.magic != ZONE_HEADER_MAGIC)
    {
        log_w("Zone %u: bad magic 0x%08X", zone, hdr.magic);
        return false;
    }

    uint32_t calc = crc32((const unsigned char *)&hdr,
                          offset_of(zone_header_t, header_crc32));
    if (calc != hdr.header_crc32)
    {
        log_e("Zone %u: header CRC mismatch (calc=0x%08X stored=0x%08X)",
              zone, calc, hdr.header_crc32);
        return false;
    }
    return true;
}

void w25q_backup_write_data(uint8_t zone, uint32_t offset,
                            const uint8_t *data, uint32_t len)
{
    w25qxx_write(w25q_backup_zone_data_addr(zone) + offset,
                 (uint8_t *)data, len);
}

void w25q_backup_read_data(uint8_t zone, uint32_t offset,
                           uint8_t *data, uint32_t len)
{
    w25qxx_read(w25q_backup_zone_data_addr(zone) + offset, data, len);
}

bool w25q_backup_set_status(uint8_t zone, uint32_t status)
{
    zone_header_t hdr;
    if (!w25q_backup_read_header(zone, &hdr))
        return false;

    /* Validate before modifying to avoid clobbering a corrupt header. */
    if (hdr.magic != ZONE_HEADER_MAGIC)
        return false;

    hdr.status = status;
    return w25q_backup_write_header(zone, &hdr);
}

uint32_t w25q_backup_get_status(uint8_t zone)
{
    zone_header_t hdr;
    if (!w25q_backup_read_header(zone, &hdr))
        return ZONE_STATUS_INVALID;
    if (hdr.magic != ZONE_HEADER_MAGIC)
        return ZONE_STATUS_INVALID;
    return hdr.status;
}

int w25q_backup_find_rollback_zone(uint32_t current_ver)
{
    for (int i = 0; i < W25Q_ZONE_COUNT; i++)
    {
        if (!w25q_backup_validate_header((uint8_t)i))
            continue;

        zone_header_t hdr;
        w25q_backup_read_header((uint8_t)i, &hdr);

        if (hdr.status != ZONE_STATUS_VALID)
            continue;

        /* Accept this zone if its version differs from the running firmware,
         * or if the caller passes 0 to indicate "any VALID zone". */
        if (current_ver == 0 || hdr.version != current_ver)
            return i;
    }
    return -1;
}

bool w25q_backup_install(uint8_t zone)
{
    zone_header_t hdr;

    /* ------------------------------------------------------------------
     * 1. Read and validate zone header.
     * ------------------------------------------------------------------ */
    if (!w25q_backup_read_header(zone, &hdr))
    {
        log_e("Zone %u: failed to read header", zone);
        return false;
    }

    if (hdr.magic != ZONE_HEADER_MAGIC)
    {
        log_e("Zone %u: invalid magic 0x%08X", zone, hdr.magic);
        return false;
    }

    uint32_t hdr_calc = crc32((const unsigned char *)&hdr,
                               offset_of(zone_header_t, header_crc32));
    if (hdr_calc != hdr.header_crc32)
    {
        log_e("Zone %u: header CRC error (calc=0x%08X stored=0x%08X)",
              zone, hdr_calc, hdr.header_crc32);
        return false;
    }

    if (hdr.status == ZONE_STATUS_INVALID)
    {
        log_e("Zone %u: status is INVALID", zone);
        return false;
    }

    if (hdr.fw_size == 0 || hdr.fw_size > ZONE_MAX_FW_SIZE)
    {
        log_e("Zone %u: bad fw_size=%u", zone, hdr.fw_size);
        return false;
    }

    if (hdr.enc_size == 0 || hdr.enc_size > ZONE_MAX_FW_SIZE)
    {
        log_e("Zone %u: bad enc_size=%u", zone, hdr.enc_size);
        return false;
    }

    log_i("Zone %u: installing fw ver=%u (%s) size=%u -> 0x%08X",
          zone, hdr.version, hdr.ver_str, hdr.fw_size, hdr.app_address);

    /* ------------------------------------------------------------------
     * 2. Verify encrypted data integrity in W25Q128 before touching flash.
     * ------------------------------------------------------------------ */
    log_i("Zone %u: verifying encrypted data CRC...", zone);
    uint32_t enc_crc_calc = w25q_compute_crc(
        w25q_backup_zone_data_addr(zone), hdr.enc_size);
    if (enc_crc_calc != hdr.enc_crc32)
    {
        log_e("Zone %u: encrypted CRC mismatch (calc=0x%08X stored=0x%08X)",
              zone, enc_crc_calc, hdr.enc_crc32);
        return false;
    }

    /* ------------------------------------------------------------------
     * 3. Erase target STM32 flash region.
     * ------------------------------------------------------------------ */
    log_i("Zone %u: erasing STM32 flash 0x%08X..0x%08X",
          zone, hdr.app_address, hdr.app_address + hdr.fw_size - 1);
    stm32_flash_unlock();
    stm32_flash_erase(hdr.app_address, hdr.fw_size);

    /* ------------------------------------------------------------------
     * 4. Decrypt in chunks and program STM32 flash.
     *    AES-CTR context is initialised once; the counter advances as
     *    AES_CTR_xcrypt_buffer is called sequentially over each chunk.
     * ------------------------------------------------------------------ */
    struct AES_ctx aes_ctx;
    AES_init_ctx_iv(&aes_ctx, fw_crypto_default_key, hdr.iv);

    uint32_t remaining  = hdr.enc_size;
    uint32_t offset     = 0;

    while (remaining > 0)
    {
        uint32_t chunk = (remaining > CHUNK_SIZE) ? CHUNK_SIZE : remaining;

        /* Read encrypted chunk from W25Q128. */
        w25q_backup_read_data(zone, offset, s_chunk_buf, chunk);

        /* Decrypt in-place using AES-128-CTR (stateful context). */
        AES_CTR_xcrypt_buffer(&aes_ctx, s_chunk_buf, chunk);

        /* Pad to 4-byte boundary for the flash word-write routine. */
        uint32_t write_len = (chunk + 3U) & ~3U;
        if (write_len > chunk)
            memset(s_chunk_buf + chunk, 0xFFU, write_len - chunk);

        stm32_flash_program(hdr.app_address + offset, s_chunk_buf, write_len);

        offset    += chunk;
        remaining -= chunk;
    }
    stm32_flash_lock();

    /* ------------------------------------------------------------------
     * 5. Verify CRC32 of the installed (plain-text) firmware in flash.
     * ------------------------------------------------------------------ */
    log_i("Zone %u: verifying installed firmware CRC...", zone);
    uint32_t fw_crc_calc = crc32((const unsigned char *)hdr.app_address,
                                 hdr.fw_size);
    if (fw_crc_calc != hdr.fw_crc32)
    {
        log_e("Zone %u: installed firmware CRC mismatch "
              "(calc=0x%08X expected=0x%08X)",
              zone, fw_crc_calc, hdr.fw_crc32);
        /* Mark zone invalid to prevent repeated bad installs. */
        w25q_backup_set_status(zone, ZONE_STATUS_INVALID);
        return false;
    }

    /* ------------------------------------------------------------------
     * 6. Update the STM32 magic header with new firmware metadata.
     * ------------------------------------------------------------------ */
    if (!magic_header_write(hdr.version, hdr.ver_str,
                            hdr.app_address, hdr.fw_size, hdr.fw_crc32))
    {
        log_e("Zone %u: failed to update magic header", zone);
        return false;
    }

    /* ------------------------------------------------------------------
     * 7. Mark zone as VALID (keeps it available for future rollback).
     * ------------------------------------------------------------------ */
    w25q_backup_set_status(zone, ZONE_STATUS_VALID);

    log_i("Zone %u: install complete", zone);
    return true;
}
