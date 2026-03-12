#ifndef __FW_MANAGER_H__
#define __FW_MANAGER_H__

#include <stdint.h>
#include <stdbool.h>
#include "fw_partition.h"

/* 初始化固件管理器（初始化w25q128、读取元数据） */
void fw_manager_init(void);

/**
 * @brief 将接收到的加密固件写入W25Q128
 *        先写入备用区，校验通过后提升为主区（双写策略）
 * @param encrypted_data 加密后的固件数据
 * @param size           固件大小
 * @param crc32          固件明文CRC32（由上位机提供，用于解密后验证）
 * @param nonce          8字节AES CTR nonce
 * @param fw_version     固件版本号
 * @return true=成功
 */
bool fw_manager_write_firmware(const uint8_t *encrypted_data, uint32_t size,
                               uint32_t crc32, const uint8_t nonce[8],
                               uint32_t fw_version);

/**
 * @brief 从W25Q128将固件解密并烧写到内部Flash
 *        自动选择有效区域（优先主区，主区无效则用备份区）
 * @return true=成功
 */
bool fw_manager_flash_firmware(void);

/**
 * @brief 检查是否需要回滚，如需要则执行回滚
 *        回滚逻辑：若rollback_flag=1，或boot_fail_count >= 3，则切换到另一区域
 */
void fw_manager_check_rollback(void);

/**
 * @brief 标记当前启动成功（清除失败计数和回滚标志）
 */
void fw_manager_mark_boot_success(void);

/**
 * @brief 标记当前启动失败（增加失败计数）
 */
void fw_manager_mark_boot_fail(void);

/**
 * @brief 请求手动回滚到备份区固件
 */
void fw_manager_request_rollback(void);

/**
 * @brief 获取当前元数据（只读）
 */
const fw_meta_t *fw_manager_get_meta(void);

/**
 * @brief 打印当前分区状态（使用elog）
 */
void fw_manager_print_status(void);

/* ========================================================================== */
/* 分片流式写入接口（供bootloader UART协议层调用）                               */
/* ========================================================================== */

/**
 * @brief 开始接收新固件：自动选择目标区（非激活区），按需擦除
 * @param total_size 本次固件完整大小（字节），用于计算需擦除的扇区数
 * @return true=成功（目标区已选定并擦除）
 */
bool fw_manager_begin_write(uint32_t total_size);

/**
 * @brief 将一片加密固件数据写入W25Q128目标区
 *        必须在 fw_manager_begin_write() 之后调用
 * @param offset 本片数据在固件中的字节偏移（从0开始）
 * @param data   加密数据缓冲区
 * @param size   本片数据字节数
 * @return true=成功
 */
bool fw_manager_write_chunk(uint32_t offset, const uint8_t *data, uint32_t size);

/**
 * @brief 提交固件写入：解密验证CRC32 → 更新元数据 → 解密烧录到内部Flash → 系统复位
 *        全部成功后调用 NVIC_SystemReset()，调用者在收到响应后系统会重启。
 *        必须在所有 fw_manager_write_chunk() 调用完毕后调用。
 * @param nonce      AES-128 CTR nonce（8字节）
 * @param fw_version 固件版本号
 * @param crc32      明文固件CRC32（由上位机提供，用于解密后校验）
 * @return true=写入成功（实际会触发系统复位，不会返回）；false=失败
 */
bool fw_manager_commit_write(const uint8_t nonce[8], uint32_t fw_version, uint32_t crc32);

#endif /* __FW_MANAGER_H__ */
