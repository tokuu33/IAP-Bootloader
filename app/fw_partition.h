#ifndef __FW_PARTITION_H__
#define __FW_PARTITION_H__

#include <stdint.h>
#include <stdbool.h>

/*
 * W25Q128 (16MB = 0x000000 ~ 0xFFFFFF) 分区规划：
 *
 * +--------------------+----------+------------------------------------------+
 * | 区域               | 地址范围  | 说明                                      |
 * +--------------------+----------+------------------------------------------+
 * | 元数据区 (Metadata) | 0x000000 | 4KB，存储版本信息、分区状态、回滚标志      |
 * | 主固件区 (ZONE_A)   | 0x001000 | 最大512KB，存储主固件（加密）               |
 * | 备份固件区 (ZONE_B) | 0x081000 | 最大512KB，存储备份固件（加密）             |
 * +--------------------+----------+------------------------------------------+
 */

#define FW_META_ADDR          0x000000UL   /* 元数据扇区起始地址 */
#define FW_META_SIZE          0x001000UL   /* 元数据区大小：4KB (1扇区) */

#define FW_ZONE_A_ADDR        0x001000UL   /* 主固件区起始地址 */
#define FW_ZONE_A_SIZE        0x080000UL   /* 主固件区大小：512KB */

#define FW_ZONE_B_ADDR        0x081000UL   /* 备份固件区起始地址 */
#define FW_ZONE_B_SIZE        0x080000UL   /* 备份固件区大小：512KB */

#define FW_META_MAGIC         0xBAADF00DUL
#define FW_META_VERSION       1

/* 分区状态 */
typedef enum {
    ZONE_STATE_EMPTY   = 0xFF, /* 未写入 */
    ZONE_STATE_VALID   = 0x01, /* 固件有效 */
    ZONE_STATE_INVALID = 0x00, /* 固件无效/损坏 */
} fw_zone_state_t;

/* 固件元数据结构（存储在W25Q128元数据区） */
typedef struct {
    uint32_t magic;           /* 魔数 0xBAADF00D */
    uint32_t meta_version;    /* 元数据版本 */
    /* 主区信息 */
    uint32_t zone_a_fw_version; /* 主区固件版本号 */
    uint32_t zone_a_size;       /* 主区固件大小（字节）*/
    uint32_t zone_a_crc32;      /* 主区固件CRC32（明文）*/
    uint8_t  zone_a_nonce[8];   /* 主区AES CTR nonce */
    uint8_t  zone_a_state;      /* 主区状态 fw_zone_state_t */
    /* 备份区信息 */
    uint32_t zone_b_fw_version; /* 备份区固件版本号 */
    uint32_t zone_b_size;       /* 备份区固件大小（字节）*/
    uint32_t zone_b_crc32;      /* 备份区固件CRC32（明文）*/
    uint8_t  zone_b_nonce[8];   /* 备份区AES CTR nonce */
    uint8_t  zone_b_state;      /* 备份区状态 fw_zone_state_t */
    /* 回滚控制 */
    uint8_t  active_zone;       /* 当前激活区：0=A，1=B */
    uint8_t  rollback_flag;     /* 回滚标志：0=正常，1=请求回滚 */
    uint8_t  boot_fail_count;   /* 启动失败计数（超过阈值触发回滚）*/
    uint8_t  reserved[5];       /* 保留字节，用于对齐 */
    uint32_t meta_crc32;        /* 元数据本身的CRC32（不含此字段）*/
} fw_meta_t;

#endif /* __FW_PARTITION_H__ */
