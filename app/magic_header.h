#ifndef __MAGIC_HEADER_H__
#define __MAGIC_HEADER_H__

#include <stdbool.h>
#include <stdint.h>

/** STM32 internal flash address of the magic header (sector 3, 16 KB). */
#define MAGIC_HEADER_ADDRESS  0x0800C000UL

/** First valid application address (sector 4). */
#define APP_BASE_ADDRESS      0x08010000UL

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
 * @brief Get the firmware version string stored in the magic header.
 * @return Pointer to the NUL-terminated version string, or NULL if
 *         the header is not present.  The string lives in flash and
 *         must not be modified by the caller.
 */
const char *magic_header_get_version(void);

/**
 * @brief Write (or overwrite) the magic header on internal flash.
 *
 * Erases the magic-header flash sector (sector 3, 16 KB at
 * MAGIC_HEADER_ADDRESS) then programs a new, fully populated header.
 * This is used after a firmware restore from the W25Q128 to make the
 * restored image bootable.
 *
 * @param fw_addr    Target STM32 flash address where firmware was written.
 * @param fw_length  Firmware binary length in bytes.
 * @param fw_crc32   CRC32 of the firmware binary.
 * @param version    NUL-terminated version string (may be NULL).
 * @return true if the written header passes validation.
 */
bool magic_header_write(uint32_t fw_addr, uint32_t fw_length,
                        uint32_t fw_crc32, const char *version);

#endif /* __MAGIC_HEADER_H__ */
