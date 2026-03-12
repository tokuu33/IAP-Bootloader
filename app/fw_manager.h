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

#endif /* __FW_MANAGER_H__ */
