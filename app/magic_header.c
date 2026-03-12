#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "crc32.h"
#include "utils.h"
#include "magic_header.h"
#include "stm32_flash.h"

#define MAGIC_HEADER_MAGIC 0x4D414749 // "MAGI"
#define MAGIC_HEADER_ADDR  0x0800C000


typedef struct
{
    uint32_t magic;         // ħ�������ڱ�ʶ����һ����Ч��ħ��ͷ
    uint32_t bitmask;       // λ���룬���ڱ�ʶ��Щ�ֶ���Ч
    uint32_t reserved1[6];  // �����ֶΣ���������չʹ��

    uint32_t data_type;     // ���ͣ�����type����ѡ��̼�����λ��
    uint32_t data_offset;   // �̼��ļ������magic header��ƫ��
    uint32_t data_address;  // �̼�д���ʵ�ʵ�ַ
    uint32_t data_length;   // �̼�����
    uint32_t data_crc32;    // �̼���CRC32У��ֵ
    uint32_t reserved2[11]; // �����ֶΣ���������չʹ��

    char version[128];      // �̼��汾�ַ���

    uint32_t reserved3[6];  // �����ֶΣ���������չʹ��
    uint32_t this_address;  // �ýṹ���ڴ洢�����е�ʵ�ʵ�ַ
    uint32_t this_crc32;    // �ýṹ�屾����CRC32У��ֵ
} magic_header_t;

bool magic_header_validate(void)
{
    magic_header_t *header = (magic_header_t *)MAGIC_HEADER_ADDR;

    if (header->magic != MAGIC_HEADER_MAGIC)
        return false;

    uint32_t ccrc = crc32((uint8_t *)header, offset_of(magic_header_t, this_crc32));
    if (ccrc != header->this_crc32)
        return false;

    return true;
}

magic_header_type_t magic_header_get_type(void)
{
    magic_header_t *header = (magic_header_t *)MAGIC_HEADER_ADDR;
    return (magic_header_type_t)header->data_type;
}

uint32_t magic_header_get_offset(void)
{
    magic_header_t *header = (magic_header_t *)MAGIC_HEADER_ADDR;
    return header->data_offset;
}

uint32_t magic_header_get_address(void)
{
    magic_header_t *header = (magic_header_t *)MAGIC_HEADER_ADDR;
    return header->data_address;
}

uint32_t magic_header_get_length(void)
{
    magic_header_t *header = (magic_header_t *)MAGIC_HEADER_ADDR;
    return header->data_length;
}

uint32_t magic_header_get_crc32(void)
{
    magic_header_t *header = (magic_header_t *)MAGIC_HEADER_ADDR;
    return header->data_crc32;
}

uint32_t magic_header_get_version(void)
{
    magic_header_t *header = (magic_header_t *)MAGIC_HEADER_ADDR;
    if (!magic_header_validate())
        return 0;
    /* The 'bitmask' field is repurposed to store the numeric firmware version.
     * This field is never read by the original bootloader logic (only written),
     * so reusing it for version tracking preserves binary compatibility. */
    return header->bitmask;
}

bool magic_header_write(uint32_t version, const char *ver_str,
                        uint32_t address, uint32_t length, uint32_t fw_crc32)
{
    /* Use a static buffer to avoid a large stack frame. */
    static magic_header_t new_hdr;
    memset(&new_hdr, 0x00, sizeof(new_hdr));

    new_hdr.magic        = MAGIC_HEADER_MAGIC;
    new_hdr.bitmask      = version;       /* repurposed as numeric version; see magic_header_get_version() */
    new_hdr.data_type    = (uint32_t)MAGIC_HEADER_TYPE_APP;
    new_hdr.data_offset  = 0;
    new_hdr.data_address = address;
    new_hdr.data_length  = length;
    new_hdr.data_crc32   = fw_crc32;
    new_hdr.this_address = MAGIC_HEADER_ADDR;

    if (ver_str != NULL)
    {
        strncpy(new_hdr.version, ver_str, sizeof(new_hdr.version) - 1);
        new_hdr.version[sizeof(new_hdr.version) - 1] = '\0';
    }

    /* Compute self-CRC over all fields up to (but not including) this_crc32. */
    new_hdr.this_crc32 = crc32((const unsigned char *)&new_hdr,
                                offset_of(magic_header_t, this_crc32));

    /* Erase the magic-header sector and write the new header. */
    stm32_flash_unlock();
    stm32_flash_erase(MAGIC_HEADER_ADDR, sizeof(magic_header_t));
    stm32_flash_program(MAGIC_HEADER_ADDR,
                        (const uint8_t *)&new_hdr, sizeof(magic_header_t));
    stm32_flash_lock();

    return true;
}
