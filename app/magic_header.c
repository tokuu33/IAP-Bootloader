#include <stdbool.h>
#include <stdint.h>
#include "crc32.h"
#include "utils.h"
#include "magic_header.h"

#define MAGIC_HEADER_MAGIC 0x4D414749  /* 'MAGI' */
#define MAGIC_HEADER_ADDR  0x0800C000  /* Sector 3 on STM32F407 (16 KB sector) */

/*
 * magic_header_t — 256 bytes, little-endian
 *
 * Offset  Size  Field
 * ------  ----  -----
 *   0       4   magic           0x4D414749 ('MAGI')
 *   4       4   update_flag     non-zero when a new image waits in W25Q128
 *   8       4   rollback_flag   non-zero when rollback is requested by the app
 *  12       4   boot_fail_count consecutive failed-boot counter
 *  16       4   data_type       MAGIC_HEADER_TYPE_APP (= 0)
 *  20       4   data_offset     offset of app binary in the .xbin file (= 256)
 *  24       4   data_address    Flash address of the app binary
 *  28       4   data_length     length of the app binary in bytes
 *  32       4   data_crc32      CRC32 of the plaintext app binary
 *  36       4   new_app_length  length of new image in W25Q128 (filled by bootloader)
 *  40       4   new_app_crc32   CRC32  of new image in W25Q128 (filled by bootloader)
 *  44       4   backup_length   length of firmware in backup zone
 *  48       4   backup_crc32    CRC32  of firmware in backup zone
 *  52     128   version         ASCII version string, zero-padded
 * 180      68   reserved3       17 × uint32_t, reserved for future use
 * 248       4   this_address    Flash address of this struct (= MAGIC_HEADER_ADDR)
 * 252       4   this_crc32      CRC32 of header[0:252] (this field excluded)
 */
typedef struct
{
    uint32_t magic;              /* [0]    0x4D414749 */
    uint32_t update_flag;        /* [4]    OTA pending flag */
    uint32_t rollback_flag;      /* [8]    rollback request flag */
    uint32_t boot_fail_count;    /* [12]   consecutive boot-fail counter */

    uint32_t data_type;          /* [16]   MAGIC_HEADER_TYPE_APP */
    uint32_t data_offset;        /* [20]   256 — offset of app in .xbin */
    uint32_t data_address;       /* [24]   Flash address where app is written */
    uint32_t data_length;        /* [28]   app binary length in bytes */
    uint32_t data_crc32;         /* [32]   CRC32 of plaintext app binary */

    uint32_t new_app_length;     /* [36]   new firmware length in W25Q128 */
    uint32_t new_app_crc32;      /* [40]   new firmware CRC32  in W25Q128 */
    uint32_t backup_length;      /* [44]   backup zone firmware length */
    uint32_t backup_crc32;       /* [48]   backup zone firmware CRC32 */

    char     version[128];       /* [52]   ASCII version string */

    uint32_t reserved3[17];      /* [180]  reserved, 68 bytes */
    uint32_t this_address;       /* [248]  address of this struct in Flash */
    uint32_t this_crc32;         /* [252]  CRC32 of header[0:252] */
} magic_header_t;

/* Compile-time size guard */
typedef char _magic_header_size_check[sizeof(magic_header_t) == 256 ? 1 : -1];

/* ── Convenience macro ───────────────────────────────────────────────────── */
#define HEADER() ((const magic_header_t *)MAGIC_HEADER_ADDR)

/* ── Public API ─────────────────────────────────────────────────────────── */

bool magic_header_validate(void)
{
    const magic_header_t *h = HEADER();

    if (h->magic != MAGIC_HEADER_MAGIC)
        return false;

    /* CRC32 covers bytes [0 .. offset_of(this_crc32) - 1] = [0 .. 251] */
    uint32_t ccrc = crc32((const uint8_t *)h, offset_of(magic_header_t, this_crc32));
    return (ccrc == h->this_crc32);
}

magic_header_type_t magic_header_get_type(void)       { return (magic_header_type_t)HEADER()->data_type;       }
uint32_t magic_header_get_offset(void)                { return HEADER()->data_offset;       }
uint32_t magic_header_get_address(void)               { return HEADER()->data_address;      }
uint32_t magic_header_get_length(void)                { return HEADER()->data_length;       }
uint32_t magic_header_get_crc32(void)                 { return HEADER()->data_crc32;        }
uint32_t magic_header_get_update_flag(void)           { return HEADER()->update_flag;       }
uint32_t magic_header_get_rollback_flag(void)         { return HEADER()->rollback_flag;     }
uint32_t magic_header_get_boot_fail_count(void)       { return HEADER()->boot_fail_count;   }
uint32_t magic_header_get_new_app_length(void)        { return HEADER()->new_app_length;    }
uint32_t magic_header_get_new_app_crc32(void)         { return HEADER()->new_app_crc32;     }
uint32_t magic_header_get_backup_length(void)         { return HEADER()->backup_length;     }
uint32_t magic_header_get_backup_crc32(void)          { return HEADER()->backup_crc32;      }
