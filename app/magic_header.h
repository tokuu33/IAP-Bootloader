#ifndef __MAGIC_HEADER_H__
#define __MAGIC_HEADER_H__

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    MAGIC_HEADER_TYPE_APP = 0,
} magic_header_type_t;

bool magic_header_validate(void);
magic_header_type_t magic_header_get_type(void);
uint32_t magic_header_get_offset(void);
uint32_t magic_header_get_address(void);
uint32_t magic_header_get_length(void);
uint32_t magic_header_get_crc32(void);
/**
 * @brief Return the version string stored in the magic header.
 *        Points to the version field in flash — read-only, copy if needed.
 *        Returns an empty string if the header is uninitialised.
 */
const char *magic_header_get_version(void);

/**
 * @brief Write a new magic header to flash (sector 3, 0x0800C000).
 *
 * Used after a firmware rollback to rebuild the header from backup metadata.
 * Erases sector 3 and programs the new header.
 *
 * @param fw_address  Start address of the firmware in internal flash.
 * @param fw_length   Firmware size in bytes.
 * @param fw_crc32    CRC32 of the firmware image.
 * @param version     NUL-terminated version string (max 127 chars).
 * @return true if the header was written and validates successfully.
 */
bool magic_header_write(uint32_t fw_address, uint32_t fw_length,
                        uint32_t fw_crc32, const char *version);

#endif /* __MAGIC_HEADER_H__ */
