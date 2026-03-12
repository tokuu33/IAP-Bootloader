/*
 * fw_manager.c — 固件管理模块完整实现
 *
 * 功能：
 *   1. W25Q128 双区备份（主区ZONE_A + 备份区ZONE_B）
 *   2. 版本管理与回滚（支持自动故障切换和手动回滚）
 *   3. 固件加密存储（AES-128 CTR，密钥流由fw_crypto提供）
 *
 * 内部Flash应用区：0x08010000（Sector 4），最大448KB（Sector 4~11）
 * W25Q128分区布局：详见 fw_partition.h
 */

#include <string.h>
#include "stm32f4xx.h"
#include "fw_manager.h"
#include "fw_crypto.h"
#include "w25q128.h"
#include "stm32_flash.h"

#define LOG_TAG "fw_mgr"
#define LOG_LVL ELOG_LVL_INFO
#include "elog.h"

/* ========================================================================== */
/* 内部常量                                                                     */
/* ========================================================================== */

/* 内部Flash应用区起始地址（Sector 4）*/
#define APP_FLASH_ADDR       0x08010000UL

/* 内部Flash应用区最大大小：Sector4(64KB) + Sector5~11(7×128KB) = 960KB
 * 实际STM32F407VET6总Flash 512KB，应用区最大 512-64=448KB（Sector4~11共8个扇区）
 * Sector4: 64KB, Sector5~11: 7*128KB=896KB -> 但总Flash只有512KB
 * 实际可用：从Sector4(0x08010000)到末尾(0x08080000)= 448KB */
#define APP_FLASH_SIZE_MAX   (448UL * 1024UL)

/* W25Q128扇区大小（4KB） */
#define W25Q_SECTOR_SIZE     4096UL

/* 分块处理缓冲区大小（匹配W25Q128扇区大小，避免大栈分配） */
#define CHUNK_SIZE           4096UL

/* 启动失败触发回滚的阈值 */
#define BOOT_FAIL_THRESHOLD  3

/* AES CTR 模式：size字节数据消耗的AES块数（不足16字节的尾块算1块） */
#define AES_BLOCKS_FOR_SIZE(size)  (((size) / 16U) + (((size) % 16U) ? 1U : 0U))

/* ========================================================================== */
/* 模块状态                                                                     */
/* ========================================================================== */

/* 当前元数据缓存（内存中） */
static fw_meta_t s_meta;

/* 元数据是否已初始化 */
static uint8_t s_meta_valid = 0;

/* 4KB静态缓冲区（避免栈溢出） */
static uint8_t s_chunk_buf[CHUNK_SIZE];

/* 解密后的明文缓冲区（与s_chunk_buf等大） */
static uint8_t s_plain_buf[CHUNK_SIZE];

/* ========================================================================== */
/* 分片流式写入状态（由 begin_write / write_chunk / commit_write 共享）          */
/* ========================================================================== */

/* 当前写入目标区的起始地址（0 = 未开始） */
static uint32_t s_write_zone_addr = 0;
/* 当前写入目标区编号：0=ZONE_A，1=ZONE_B */
static uint8_t  s_write_zone_idx  = 0;
/* 本次固件的完整大小（由 begin_write 设置，commit_write 使用） */
static uint32_t s_write_total_size = 0;

/* ========================================================================== */
/* CRC32 内部辅助（与fw_crypto使用相同算法，多项式0xEDB88320） */
/* ========================================================================== */

static uint32_t calc_crc32_buf(const uint8_t *data, uint32_t len)
{
    static const uint32_t crc_table[16] = {
        0x00000000UL, 0x1DB71064UL, 0x3B6E20C8UL, 0x26D930ACUL,
        0x76DC4190UL, 0x6B6B51F4UL, 0x4DB26158UL, 0x5005713CUL,
        0xEDB88320UL, 0xF00F9344UL, 0xD6D6A3E8UL, 0xCB61B38CUL,
        0x9B64C2B0UL, 0x86D3D2D4UL, 0xA00AE278UL, 0xBDBDF21CUL
    };
    uint32_t crc = 0xFFFFFFFFUL;
    uint32_t i;
    for (i = 0; i < len; i++)
    {
        crc = (crc >> 4) ^ crc_table[(crc ^ (data[i] >> 0)) & 0x0FU];
        crc = (crc >> 4) ^ crc_table[(crc ^ (data[i] >> 4)) & 0x0FU];
    }
    return crc ^ 0xFFFFFFFFUL;
}

/* ========================================================================== */
/* 元数据内部操作                                                               */
/* ========================================================================== */

/**
 * @brief 从W25Q128读取元数据到meta结构体
 */
static void meta_read(fw_meta_t *meta)
{
    w25qxx_read(FW_META_ADDR, (uint8_t *)meta, sizeof(fw_meta_t));
}

/**
 * @brief 将meta结构体写入W25Q128元数据区
 *        写前计算meta_crc32（不含meta_crc32字段本身），先擦除扇区再写入
 */
static void meta_write(const fw_meta_t *meta)
{
    fw_meta_t tmp;
    memcpy(&tmp, meta, sizeof(fw_meta_t));

    /* 计算元数据CRC32（不含meta_crc32字段） */
    tmp.meta_crc32 = calc_crc32_buf((const uint8_t *)&tmp,
                                     (uint32_t)((uint8_t *)&tmp.meta_crc32 - (uint8_t *)&tmp));

    /* 擦除元数据扇区（1扇区 = 4KB） */
    w25qxx_erase_sector(FW_META_ADDR);

    /* 写入更新后的元数据 */
    w25qxx_write(FW_META_ADDR, (uint8_t *)&tmp, sizeof(fw_meta_t));
}

/**
 * @brief 验证元数据有效性（检查magic和meta_crc32）
 * @return true=元数据有效
 */
static bool meta_validate(const fw_meta_t *meta)
{
    if (meta->magic != FW_META_MAGIC)
        return false;

    /* 计算并比对元数据CRC32 */
    uint32_t crc = calc_crc32_buf((const uint8_t *)meta,
                                   (uint32_t)((const uint8_t *)&meta->meta_crc32 - (const uint8_t *)meta));
    if (crc != meta->meta_crc32)
        return false;

    return true;
}

/* ========================================================================== */
/* W25Q128 区域操作                                                             */
/* ========================================================================== */

/**
 * @brief 按4KB扇区擦除W25Q128指定区域
 * @param zone_addr 区域起始地址（4KB对齐）
 * @param size      区域大小（字节）
 */
static void erase_zone(uint32_t zone_addr, uint32_t size)
{
    uint32_t addr = zone_addr;
    uint32_t erased = 0;

    while (erased < size)
    {
        w25qxx_erase_sector(addr);
        addr   += W25Q_SECTOR_SIZE;
        erased += W25Q_SECTOR_SIZE;
    }
}

/**
 * @brief 将数据写入W25Q128区域
 * @param zone_addr 目标起始地址
 * @param data      数据缓冲区
 * @param size      数据大小
 */
static void write_zone(uint32_t zone_addr, const uint8_t *data, uint32_t size)
{
    w25qxx_write(zone_addr, (uint8_t *)data, size);
}

/**
 * @brief 从W25Q128读出固件并解密，验证明文CRC32
 * @param zone_addr      区域起始地址
 * @param size           固件大小
 * @param expected_crc32 期望的明文CRC32
 * @param nonce          AES-128 CTR nonce（Number Used Once，8字节）：上位机加密固件时随机生成
 *                       的初始化向量前半段。计数器块格式 [nonce(8B)][counter_be(4B)][zeros(4B)]，
 *                       counter 从 0 起每 16 字节递增。由 FW_COMMIT payload 传入，使 bootloader
 *                       能以与加密时相同的参数逐块解密固件，从而还原明文并计算 CRC32。
 * @return true=验证通过
 */
static bool verify_zone(uint32_t zone_addr, uint32_t size,
                         uint32_t expected_crc32, const uint8_t nonce[8])
{
    uint32_t offset     = 0;
    uint32_t counter    = 0;
    uint32_t chunk_len;
    uint32_t crc_accum  = 0xFFFFFFFFUL;

    /* 复用4位nibble查找表进行流式CRC32计算 */
    static const uint32_t crc_table[16] = {
        0x00000000UL, 0x1DB71064UL, 0x3B6E20C8UL, 0x26D930ACUL,
        0x76DC4190UL, 0x6B6B51F4UL, 0x4DB26158UL, 0x5005713CUL,
        0xEDB88320UL, 0xF00F9344UL, 0xD6D6A3E8UL, 0xCB61B38CUL,
        0x9B64C2B0UL, 0x86D3D2D4UL, 0xA00AE278UL, 0xBDBDF21CUL
    };

    while (offset < size)
    {
        chunk_len = (size - offset > CHUNK_SIZE) ? CHUNK_SIZE : (size - offset);

        /* 从W25Q128读出加密数据 */
        w25qxx_read(zone_addr + offset, s_chunk_buf, chunk_len);

        /* CTR模式解密（counter基于已处理块数） */
        fw_crypto_decrypt(s_chunk_buf, s_plain_buf, chunk_len, nonce, counter);

        /* 流式计算明文CRC32 */
        uint32_t i;
        for (i = 0; i < chunk_len; i++)
        {
            crc_accum = (crc_accum >> 4) ^ crc_table[(crc_accum ^ (s_plain_buf[i] >> 0)) & 0x0FU];
            crc_accum = (crc_accum >> 4) ^ crc_table[(crc_accum ^ (s_plain_buf[i] >> 4)) & 0x0FU];
        }

        offset  += chunk_len;
        /* 每个AES块（含不足16字节的尾块）消耗一个counter值 */
        counter += AES_BLOCKS_FOR_SIZE(chunk_len);
    }

    crc_accum ^= 0xFFFFFFFFUL;
    return (crc_accum == expected_crc32);
}

/**
 * @brief 从W25Q128分块读取、解密，并写入内部Flash
 * @param zone_addr 区域起始地址（W25Q128）
 * @param size      固件大小
 * @param nonce     AES-128 CTR nonce（Number Used Once，8字节）：上位机加密固件时随机生成
 *                  的初始化向量前半段。计数器块格式 [nonce(8B)][counter_be(4B)][zeros(4B)]，
 *                  counter 从 0 起每 16 字节递增。由 FW_COMMIT payload 传入，使 bootloader
 *                  能以与加密时相同的参数逐块解密固件并烧录到内部 Flash。
 * @return true=成功
 */
static bool decrypt_and_flash(uint32_t zone_addr, uint32_t size,
                               const uint8_t nonce[8])
{
    uint32_t offset   = 0;
    uint32_t counter  = 0;
    uint32_t chunk_len;
    uint32_t flash_addr = APP_FLASH_ADDR;

    while (offset < size)
    {
        chunk_len = (size - offset > CHUNK_SIZE) ? CHUNK_SIZE : (size - offset);

        /* 从W25Q128读出加密数据 */
        w25qxx_read(zone_addr + offset, s_chunk_buf, chunk_len);

        /* CTR模式解密 */
        fw_crypto_decrypt(s_chunk_buf, s_plain_buf, chunk_len, nonce, counter);

        /* 写入内部Flash（stm32_flash_program按4字节对齐写入） */
        stm32_flash_program(flash_addr + offset, s_plain_buf, chunk_len);

        /* 验证写入：比对Flash内容与解密后数据 */
        if (memcmp((const uint8_t *)(flash_addr + offset), s_plain_buf, chunk_len) != 0)
        {
            log_e("Flash verify failed at offset 0x%08lX", (unsigned long)offset);
            return false;
        }

        offset    += chunk_len;
        /* 每个AES块（含不足16字节的尾块）消耗一个counter值 */
        counter   += AES_BLOCKS_FOR_SIZE(chunk_len);
    }

    return true;
}

/* ========================================================================== */
/* 公共接口实现                                                                 */
/* ========================================================================== */

void fw_manager_init(void)
{
    /* 使能SPI1时钟（board_lowlevel_init中未启用SPI1 APB2时钟，在此补充初始化）
     * 注：不修改board.c/w25q128.c的前提下，须在调用w25qxx_init前使能时钟 */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_SPI1, ENABLE);

    /* 初始化W25Q128 SPI驱动 */
    w25qxx_init();

    /* 读取并验证元数据 */
    meta_read(&s_meta);
    if (meta_validate(&s_meta))
    {
        s_meta_valid = 1;
        log_i("fw_manager: metadata valid, active_zone=%d, fw_ver_a=%lu, fw_ver_b=%lu",
              s_meta.active_zone,
              (unsigned long)s_meta.zone_a_fw_version,
              (unsigned long)s_meta.zone_b_fw_version);
    }
    else
    {
        /* 元数据无效（首次使用或已损坏），初始化默认值 */
        log_w("fw_manager: metadata invalid, initializing defaults");
        memset(&s_meta, 0, sizeof(fw_meta_t));
        s_meta.magic         = FW_META_MAGIC;
        s_meta.meta_version  = FW_META_VERSION;
        s_meta.zone_a_state  = (uint8_t)ZONE_STATE_EMPTY;
        s_meta.zone_b_state  = (uint8_t)ZONE_STATE_EMPTY;
        s_meta.active_zone   = 0;
        s_meta.rollback_flag = 0;
        s_meta.boot_fail_count = 0;
        s_meta_valid = 1;
        /* 不写入Flash，等有固件写入时再保存 */
    }
}

bool fw_manager_write_firmware(const uint8_t *encrypted_data, uint32_t size,
                               uint32_t crc32, const uint8_t nonce[8],
                               uint32_t fw_version)
{
    uint32_t target_addr;
    uint8_t  target_zone; /* 0=A，1=B */

    if (size == 0 || size > FW_ZONE_A_SIZE)
    {
        log_e("fw_write: invalid size %lu", (unsigned long)size);
        return false;
    }

    /* 确定写入目标区：优先写入非激活区（备份区策略）
     * 若主区(ZONE_A)无效（首次写入），则写ZONE_A并激活为主区；
     * 否则写非激活区（active_zone=0则写B，active_zone=1则写A）。*/
    if (s_meta.zone_a_state != (uint8_t)ZONE_STATE_VALID)
    {
        /* 主区为空，写入主区 */
        target_zone = 0;
        target_addr = FW_ZONE_A_ADDR;
    }
    else
    {
        /* 主区有效，写入非激活区（备份区） */
        if (s_meta.active_zone == 0)
        {
            target_zone = 1;
            target_addr = FW_ZONE_B_ADDR;
        }
        else
        {
            target_zone = 0;
            target_addr = FW_ZONE_A_ADDR;
        }
    }

    log_i("fw_write: writing to zone_%c, addr=0x%06lX, size=%lu",
          (target_zone == 0) ? 'A' : 'B',
          (unsigned long)target_addr,
          (unsigned long)size);

    /* 擦除目标区（按4KB对齐，向上取整） */
    uint32_t erase_size = ((size + W25Q_SECTOR_SIZE - 1) / W25Q_SECTOR_SIZE) * W25Q_SECTOR_SIZE;
    erase_zone(target_addr, erase_size);

    /* 将加密固件原样写入W25Q128（保持加密态） */
    write_zone(target_addr, encrypted_data, size);

    /* 读回验证写入完整性（字节比对） */
    {
        uint32_t offset = 0;
        uint32_t chunk_len;
        bool write_ok = true;
        while (offset < size)
        {
            chunk_len = (size - offset > CHUNK_SIZE) ? CHUNK_SIZE : (size - offset);
            w25qxx_read(target_addr + offset, s_chunk_buf, chunk_len);
            if (memcmp(encrypted_data + offset, s_chunk_buf, chunk_len) != 0)
            {
                log_e("fw_write: write verify failed at offset 0x%08lX", (unsigned long)offset);
                write_ok = false;
                break;
            }
            offset += chunk_len;
        }
        if (!write_ok)
            return false;
    }

    /* 解密验证：读出加密数据，解密后计算CRC32，与预期比对 */
    if (!verify_zone(target_addr, size, crc32, nonce))
    {
        log_e("fw_write: CRC32 verify failed after decryption");
        return false;
    }

    /* 更新元数据 */
    if (target_zone == 0)
    {
        s_meta.zone_a_fw_version = fw_version;
        s_meta.zone_a_size       = size;
        s_meta.zone_a_crc32      = crc32;
        memcpy(s_meta.zone_a_nonce, nonce, 8);
        s_meta.zone_a_state      = (uint8_t)ZONE_STATE_VALID;
    }
    else
    {
        s_meta.zone_b_fw_version = fw_version;
        s_meta.zone_b_size       = size;
        s_meta.zone_b_crc32      = crc32;
        memcpy(s_meta.zone_b_nonce, nonce, 8);
        s_meta.zone_b_state      = (uint8_t)ZONE_STATE_VALID;
    }

    /* 如主区无效（首次写入），激活为主区；否则设为备份区 */
    if (target_zone == 0 && s_meta.zone_b_state != (uint8_t)ZONE_STATE_VALID)
    {
        /* 首次写入主区，激活主区 */
        s_meta.active_zone = 0;
    }
    /* 其他情况保持active_zone不变（新写入区域作为备份） */

    /* 清除回滚标志和失败计数（新固件到来，重置状态） */
    s_meta.rollback_flag    = 0;
    s_meta.boot_fail_count  = 0;

    /* 保存元数据 */
    meta_write(&s_meta);
    log_i("fw_write: metadata updated, zone_%c is now valid",
          (target_zone == 0) ? 'A' : 'B');

    /* 收到新固件后，立即解密烧录到内部Flash并重启 */
    if (!fw_manager_flash_firmware())
    {
        log_e("fw_write: flash firmware failed");
        return false;
    }

    /* 烧录成功后复位系统，从新固件启动 */
    log_i("fw_write: firmware flashed, resetting system...");
    NVIC_SystemReset();

    /* 不会执行到此处 */
    return true;
}

bool fw_manager_flash_firmware(void)
{
    uint32_t zone_addr;
    uint32_t zone_size;
    uint32_t zone_crc32;
    const uint8_t *zone_nonce;
    bool flash_ok = false;
    uint8_t try_zone; /* 本次尝试的区（0=A，1=B） */

    /* 读取最新元数据 */
    meta_read(&s_meta);
    if (!meta_validate(&s_meta))
    {
        log_e("fw_flash: metadata invalid, cannot flash");
        return false;
    }

    /* 首先尝试active_zone指向的区域；若失败，切换到另一区域 */
    for (uint8_t attempt = 0; attempt < 2; attempt++)
    {
        try_zone = (attempt == 0) ? s_meta.active_zone
                                  : (uint8_t)(1U - s_meta.active_zone);

        if (try_zone == 0)
        {
            if (s_meta.zone_a_state != (uint8_t)ZONE_STATE_VALID)
            {
                log_w("fw_flash: zone_A invalid, skip");
                continue;
            }
            zone_addr  = FW_ZONE_A_ADDR;
            zone_size  = s_meta.zone_a_size;
            zone_crc32 = s_meta.zone_a_crc32;
            zone_nonce = s_meta.zone_a_nonce;
        }
        else
        {
            if (s_meta.zone_b_state != (uint8_t)ZONE_STATE_VALID)
            {
                log_w("fw_flash: zone_B invalid, skip");
                continue;
            }
            zone_addr  = FW_ZONE_B_ADDR;
            zone_size  = s_meta.zone_b_size;
            zone_crc32 = s_meta.zone_b_crc32;
            zone_nonce = s_meta.zone_b_nonce;
        }

        if (zone_size == 0 || zone_size > APP_FLASH_SIZE_MAX)
        {
            log_e("fw_flash: invalid zone size %lu", (unsigned long)zone_size);
            continue;
        }

        log_i("fw_flash: flashing from zone_%c, size=%lu",
              (try_zone == 0) ? 'A' : 'B', (unsigned long)zone_size);

        /* 解锁内部Flash */
        stm32_flash_unlock();

        /* 擦除应用区对应扇区（从0x08010000开始，按实际大小） */
        stm32_flash_erase(APP_FLASH_ADDR, zone_size);

        /* 分块解密并写入内部Flash */
        flash_ok = decrypt_and_flash(zone_addr, zone_size, zone_nonce);

        /* 锁定内部Flash */
        stm32_flash_lock();

        if (!flash_ok)
        {
            log_e("fw_flash: decrypt_and_flash failed for zone_%c",
                  (try_zone == 0) ? 'A' : 'B');
            /* 标记该区无效（防止下次再选到损坏区域） */
            if (try_zone == 0)
                s_meta.zone_a_state = (uint8_t)ZONE_STATE_INVALID;
            else
                s_meta.zone_b_state = (uint8_t)ZONE_STATE_INVALID;
            meta_write(&s_meta);
            continue; /* 尝试另一区域 */
        }

        /* 验证内部Flash：对Flash中已烧录数据重新计算CRC32 */
        if (!fw_crypto_verify_crc32((const uint8_t *)APP_FLASH_ADDR, zone_size, zone_crc32))
        {
            log_e("fw_flash: CRC32 verify failed on internal flash");
            flash_ok = false;
            if (try_zone == 0)
                s_meta.zone_a_state = (uint8_t)ZONE_STATE_INVALID;
            else
                s_meta.zone_b_state = (uint8_t)ZONE_STATE_INVALID;
            meta_write(&s_meta);
            continue;
        }

        /* 成功，更新激活区 */
        s_meta.active_zone = try_zone;
        meta_write(&s_meta);
        log_i("fw_flash: success, active_zone=%d", s_meta.active_zone);
        flash_ok = true;
        break;
    }

    return flash_ok;
}

void fw_manager_check_rollback(void)
{
    /* 读取最新元数据 */
    meta_read(&s_meta);
    if (!meta_validate(&s_meta))
    {
        /* 元数据无效，不进行回滚处理 */
        log_w("fw_mgr: check_rollback: metadata invalid, skip");
        return;
    }

    bool need_rollback = (s_meta.rollback_flag == 1U) ||
                         (s_meta.boot_fail_count >= BOOT_FAIL_THRESHOLD);

    if (!need_rollback)
        return;

    log_w("fw_mgr: rollback triggered (flag=%d, fail_count=%d)",
          s_meta.rollback_flag, s_meta.boot_fail_count);

    /* 切换active_zone到另一区 */
    s_meta.active_zone     = (uint8_t)(1U - s_meta.active_zone);
    s_meta.rollback_flag   = 0;
    s_meta.boot_fail_count = 0;
    meta_write(&s_meta);

    log_i("fw_mgr: switching to zone_%c",
          (s_meta.active_zone == 0) ? 'A' : 'B');

    /* 将备份固件烧写到内部Flash */
    if (!fw_manager_flash_firmware())
    {
        log_e("fw_mgr: rollback flash firmware failed");
        /* 即使失败也重启，让bootloader重新处理 */
    }

    /* 重启系统，从回滚后的固件启动 */
    log_w("fw_mgr: rollback complete, resetting system...");
    NVIC_SystemReset();
}

void fw_manager_mark_boot_success(void)
{
    meta_read(&s_meta);
    if (!meta_validate(&s_meta))
        return;

    s_meta.boot_fail_count = 0;
    s_meta.rollback_flag   = 0;
    meta_write(&s_meta);
    log_i("fw_mgr: boot success marked");
}

void fw_manager_mark_boot_fail(void)
{
    meta_read(&s_meta);
    if (!meta_validate(&s_meta))
        return;

    if (s_meta.boot_fail_count < 0xFFU)
        s_meta.boot_fail_count++;
    meta_write(&s_meta);
    log_w("fw_mgr: boot fail count=%d", s_meta.boot_fail_count);
}

void fw_manager_request_rollback(void)
{
    meta_read(&s_meta);
    if (!meta_validate(&s_meta))
        return;

    s_meta.rollback_flag = 1;
    meta_write(&s_meta);
    log_i("fw_mgr: rollback requested");
}

const fw_meta_t *fw_manager_get_meta(void)
{
    return &s_meta;
}

void fw_manager_print_status(void)
{
    meta_read(&s_meta);

    log_i("=== FW Manager Status ===");
    if (!meta_validate(&s_meta))
    {
        log_w("  Metadata: INVALID");
        return;
    }

    log_i("  Active zone   : %s", (s_meta.active_zone == 0) ? "A" : "B");
    log_i("  Rollback flag : %d", s_meta.rollback_flag);
    log_i("  Fail count    : %d", s_meta.boot_fail_count);
    log_i("  Zone A: state=%s, ver=%lu, size=%lu",
          (s_meta.zone_a_state == (uint8_t)ZONE_STATE_VALID)   ? "VALID"   :
          (s_meta.zone_a_state == (uint8_t)ZONE_STATE_INVALID) ? "INVALID" : "EMPTY",
          (unsigned long)s_meta.zone_a_fw_version,
          (unsigned long)s_meta.zone_a_size);
    log_i("  Zone B: state=%s, ver=%lu, size=%lu",
          (s_meta.zone_b_state == (uint8_t)ZONE_STATE_VALID)   ? "VALID"   :
          (s_meta.zone_b_state == (uint8_t)ZONE_STATE_INVALID) ? "INVALID" : "EMPTY",
          (unsigned long)s_meta.zone_b_fw_version,
          (unsigned long)s_meta.zone_b_size);
    log_i("=========================");
}

/* ========================================================================== */
/* 分片流式写入接口实现                                                          */
/* ========================================================================== */

bool fw_manager_begin_write(uint32_t total_size)
{
    uint32_t target_addr;
    uint8_t  target_zone;
    uint32_t erase_size;

    if (total_size == 0 || total_size > FW_ZONE_A_SIZE)
    {
        log_e("fw_begin: invalid total_size %lu", (unsigned long)total_size);
        return false;
    }

    /* 确定目标区（同 fw_manager_write_firmware 的选区逻辑）：
     * 若 ZONE_A 无效（首次写入），写入 ZONE_A；
     * 否则写入非激活区（active_zone=0→写B，active_zone=1→写A）。*/
    if (s_meta.zone_a_state != (uint8_t)ZONE_STATE_VALID)
    {
        target_zone = 0;
        target_addr = FW_ZONE_A_ADDR;
    }
    else
    {
        target_zone = (s_meta.active_zone == 0) ? 1U : 0U;
        target_addr = (target_zone == 0) ? FW_ZONE_A_ADDR : FW_ZONE_B_ADDR;
    }

    log_i("fw_begin: target zone_%c addr=0x%06lX, erase for %lu bytes",
          (target_zone == 0) ? 'A' : 'B',
          (unsigned long)target_addr,
          (unsigned long)total_size);

    /* 按4KB对齐向上取整擦除目标区 */
    erase_size = ((total_size + W25Q_SECTOR_SIZE - 1U) / W25Q_SECTOR_SIZE) * W25Q_SECTOR_SIZE;
    erase_zone(target_addr, erase_size);

    /* 记录流式写入状态 */
    s_write_zone_addr  = target_addr;
    s_write_zone_idx   = target_zone;
    s_write_total_size = total_size;

    return true;
}

bool fw_manager_write_chunk(uint32_t offset, const uint8_t *data, uint32_t size)
{
    if (s_write_zone_addr == 0)
    {
        log_e("fw_chunk: begin_write not called");
        return false;
    }
    if (offset + size > FW_ZONE_A_SIZE)
    {
        log_e("fw_chunk: offset+size overflow zone");
        return false;
    }

    w25qxx_write(s_write_zone_addr + offset, (uint8_t *)data, size);
    log_d("fw_chunk: wrote %lu bytes at zone offset 0x%08lX",
          (unsigned long)size, (unsigned long)offset);
    return true;
}

bool fw_manager_commit_write(const uint8_t nonce[8], uint32_t fw_version, uint32_t crc32)
{
    if (s_write_zone_addr == 0)
    {
        log_e("fw_commit: begin_write not called");
        return false;
    }
    if (s_write_total_size == 0)
    {
        log_e("fw_commit: total_size is zero");
        return false;
    }

    log_i("fw_commit: verifying zone_%c, size=%lu",
          (s_write_zone_idx == 0) ? 'A' : 'B',
          (unsigned long)s_write_total_size);

    /* 解密验证CRC32（从W25Q128读出加密数据，解密后计算CRC32并与预期比对） */
    if (!verify_zone(s_write_zone_addr, s_write_total_size, crc32, nonce))
    {
        log_e("fw_commit: CRC32 verify failed");
        return false;
    }

    /* 更新目标区元数据 */
    if (s_write_zone_idx == 0)
    {
        s_meta.zone_a_fw_version = fw_version;
        s_meta.zone_a_size       = s_write_total_size;
        s_meta.zone_a_crc32      = crc32;
        memcpy(s_meta.zone_a_nonce, nonce, 8);
        s_meta.zone_a_state      = (uint8_t)ZONE_STATE_VALID;
    }
    else
    {
        s_meta.zone_b_fw_version = fw_version;
        s_meta.zone_b_size       = s_write_total_size;
        s_meta.zone_b_crc32      = crc32;
        memcpy(s_meta.zone_b_nonce, nonce, 8);
        s_meta.zone_b_state      = (uint8_t)ZONE_STATE_VALID;
    }

    /* 将新写入的区域设为激活区（立即生效，旧区域变为备份） */
    s_meta.active_zone     = s_write_zone_idx;
    s_meta.rollback_flag   = 0;
    s_meta.boot_fail_count = 0;
    meta_write(&s_meta);

    log_i("fw_commit: metadata updated, zone_%c is now active",
          (s_write_zone_idx == 0) ? 'A' : 'B');

    /* 清除流式写入状态（防止重复提交） */
    s_write_zone_addr  = 0;
    s_write_zone_idx   = 0;
    s_write_total_size = 0;

    log_i("fw_commit: metadata updated, zone_%c active. Caller should flash+reset.",
          (s_meta.active_zone == 0) ? 'A' : 'B');

    return true; /* 验证和元数据更新成功；调用者负责执行烧录和复位 */
}
