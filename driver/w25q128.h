#ifndef __W25Q128_H__
#define __W25Q128_H__

#include <stdint.h>

void w25qxx_read(uint32_t addr, uint8_t *buf, uint32_t len);
void w25qxx_write(uint32_t addr, uint8_t *buf, uint32_t len);
void w25qxx_erase_sector(uint32_t addr);

#endif /* __W25Q128_H__ */
