#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "crc32.h"
#include "utils.h"
#include "stm32_flash.h"
#include "magic_header.h"

#define MAGIC_HEADER_MAGIC 0x4D414749UL  /* "MAGI" */
#define MAGIC_HEADER_ADDR  MAGIC_HEADER_ADDRESS


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

const char *magic_header_get_version(void)
{
    magic_header_t *header = (magic_header_t *)MAGIC_HEADER_ADDR;
    if (header->magic != MAGIC_HEADER_MAGIC)
        return NULL;
    return header->version;
}

bool magic_header_write(uint32_t fw_addr, uint32_t fw_length,
                        uint32_t fw_crc32, const char *version)
{
    magic_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));

    hdr.magic        = MAGIC_HEADER_MAGIC;
    hdr.bitmask      = 0;
    hdr.data_type    = (uint32_t)MAGIC_HEADER_TYPE_APP;
    hdr.data_offset  = 0;
    hdr.data_address = fw_addr;
    hdr.data_length  = fw_length;
    hdr.data_crc32   = fw_crc32;
    hdr.this_address = MAGIC_HEADER_ADDR;

    if (version != NULL) {
        strncpy(hdr.version, version, sizeof(hdr.version) - 1U);
    }

    hdr.this_crc32 = crc32((uint8_t *)&hdr,
                           offset_of(magic_header_t, this_crc32));

    stm32_flash_unlock();
    stm32_flash_erase(MAGIC_HEADER_ADDR, sizeof(hdr));
    stm32_flash_program(MAGIC_HEADER_ADDR, (const uint8_t *)&hdr, sizeof(hdr));
    stm32_flash_lock();

    return magic_header_validate();
}
