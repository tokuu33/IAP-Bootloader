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
 * @brief  Get the numeric firmware version stored in the magic header.
 * @return Version number, or 0 if the header is not valid.
 */
uint32_t magic_header_get_version(void);

/**
 * @brief  Erase the magic-header flash sector and write a new magic header.
 *
 *         This is called by the bootloader after successfully installing a
 *         firmware image from the W25Q128 backup zone.
 *
 * @param  version    Numeric firmware version (e.g. 0x00010200 for v1.2.0).
 * @param  ver_str    Null-terminated version string (e.g. "v1.2.0").
 * @param  address    Firmware start address in STM32 internal flash.
 * @param  length     Firmware length in bytes.
 * @param  fw_crc32   CRC32 of the firmware data.
 * @return true on success.
 */
bool magic_header_write(uint32_t version, const char *ver_str,
                        uint32_t address, uint32_t length, uint32_t fw_crc32);

#endif /* __MAGIC_HEADER_H__ */
