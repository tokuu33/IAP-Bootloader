#ifndef __FW_CRYPTO_H__
#define __FW_CRYPTO_H__

#include <stdint.h>
#include <stdbool.h>

/* AES-128 密钥（16字节），实际使用时应通过安全方式存储 */
#define FW_CRYPTO_KEY_SIZE   16
#define FW_CRYPTO_BLOCK_SIZE 16

/* 初始化加密模块（设置默认密钥） */
void fw_crypto_init(void);

/* 设置加密密钥（16字节） */
void fw_crypto_set_key(const uint8_t *key);

/**
 * @brief AES-128 CTR 模式解密（同加密，CTR是对称的）
 * @param in       输入数据（密文）
 * @param out      输出数据（明文）
 * @param len      数据长度（字节，不需要是16的倍数）
 * @param nonce    8字节 nonce（Number Used Once）：AES-128 CTR 计数器块的前 8 字节 IV。
 *                 上位机加密时随机生成，由 FW_COMMIT 指令 payload 传入，用于还原与加密时
 *                 完全一致的计数器块序列，从而正确解密固件。
 *                 计数器块格式：[nonce(8B)][counter_be(4B)][zeros(4B)]
 * @param counter  起始计数器值（通常为 0，每处理 16 字节递增 1）
 */
void fw_crypto_decrypt(const uint8_t *in, uint8_t *out, uint32_t len,
                       const uint8_t nonce[8], uint32_t counter);

/**
 * @brief 验证固件CRC32
 * @param data   固件数据指针
 * @param len    固件长度
 * @param crc32  期望的CRC32值
 * @return true=校验通过
 */
bool fw_crypto_verify_crc32(const uint8_t *data, uint32_t len, uint32_t crc32);

#endif /* __FW_CRYPTO_H__ */
