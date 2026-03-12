#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "stm32f4xx.h"
#include "board.h"
#include "bl_usart.h"
#include "tim_delay.h"
#include "stm32_flash.h"
#include "magic_header.h"
#include "crc16.h"
#include "crc32.h"
#include "ringbuffer.h"
#include "utils.h"
#include "fw_manager.h"    /* 固件管理：双区备份 + 版本回滚 */

#define LOG_TAG      "bootload"
#define LOG_LVL      ELOG_LVL_INFO
#include "elog.h"

#define BL_VERSION       "v0.9.0"
#define BL_ADDRESS       0x08000000
#define BL_SIZE          (48 * 1024)   // 48KB
#define APP_BASE_ADDRESS 0x08010000
#define RX_TIMEOUT_MS    20
#define RX_BUFFER_SIZE   (5 * 1024)
#define PAYLOAD_SIZE_MAX (4096 + 8) //4096ΪProgram������󳤶ȣ�8ΪProgramָ��ĵ�ַ(4)�ʹ�С(4)�ֶγ���
#define PACKET_SIZE_MAX  (4 + PAYLOAD_SIZE_MAX + 2)  // header(1) + opcode(1) + length(2) + payload + crc16(2)
#define BOOT_DELAY       3000

// Э�鳣��
#define PACKET_HEADER_REQUEST 0xAA
#define PACKET_HEADER_RESPONSE 0x55

// ���ݰ��ṹ����
#define PACKET_HEADER_SIZE 1
#define PACKET_OPCODE_SIZE 1
#define PACKET_LENGTH_SIZE 2
#define PACKET_CRC_SIZE 2
#define PACKET_HEADER_OFFSET 0
#define PACKET_OPCODE_OFFSET 1
#define PACKET_LENGTH_OFFSET 2
#define PACKET_PAYLOAD_OFFSET 4
#define PACKET_MIN_SIZE (PACKET_HEADER_SIZE + PACKET_OPCODE_SIZE + PACKET_LENGTH_SIZE + PACKET_CRC_SIZE)

/* 指令参数长度常量 */
#define ADDR_SIZE_PARAM_LENGTH 8         /* uint32_t addr + uint32_t size */
#define ADDR_SIZE_CRC_PARAM_LENGTH 12    /* uint32_t addr + uint32_t size + uint32_t crc */

/*
 * 加密固件流式传输相关 payload 长度常量
 *
 * FW_WRITE_PARAM_HEADER_LEN (8)
 *   用途：FW_WRITE 指令 (0x84) payload 中固定元数据头部的字节长度。
 *   FW_WRITE payload 格式：
 *     [total_size (4B, uint32)] [chunk_offset (4B, uint32)] [encrypted_data (N B)]
 *   头部固定占 8 字节，紧跟其后的才是本片加密数据。
 *   处理函数用法：
 *     data_size = packet_payload_length - FW_WRITE_PARAM_HEADER_LEN  // 本片加密数据长度
 *
 * FW_COMMIT_PARAM_LEN (16)
 *   用途：FW_COMMIT 指令 (0x85) payload 的精确总字节长度（固定长度，不可多也不可少）。
 *   FW_COMMIT payload 格式：
 *     [nonce (8B)] [fw_version (4B, uint32)] [crc32 (4B, uint32)]
 *   处理函数用 packet_payload_length == FW_COMMIT_PARAM_LEN 做格式校验。
 */
#define FW_WRITE_PARAM_HEADER_LEN  8    /* FW_WRITE(0x84)  payload头部：total_size(4B) + chunk_offset(4B) */
#define FW_COMMIT_PARAM_LEN        16   /* FW_COMMIT(0x85) payload总长：nonce(8B) + fw_version(4B) + crc32(4B) */

typedef enum
{
    PACKET_STATE_HEADER,
    PACKET_STATE_OPCODE,
    PACKET_STATE_LENGTH,
    PACKET_STATE_PAYLOAD,
    PACKET_STATE_CRC16
} packet_state_machine_t;

typedef enum
{
    PACKET_OPCODE_INQUERY    = 0x01,
    PACKET_OPCODE_ERASE      = 0x81,
    PACKET_OPCODE_PROGRAM    = 0x82,
    PACKET_OPCODE_VERIFY     = 0x83,
    PACKET_OPCODE_FW_WRITE   = 0x84,  /* 向W25Q128写加密固件分片 */
    PACKET_OPCODE_FW_COMMIT  = 0x85,  /* 提交固件：验证+激活+烧录到内部Flash+复位 */
    PACKET_OPCODE_FW_ROLLBACK= 0x86,  /* 手动回滚到备份区 */
    PACKET_OPCODE_RESET      = 0x21,
    PACKET_OPCODE_BOOT       = 0x22
} packet_opcode_t;

typedef enum
{
    PACKET_ERRCODE_ERR_OK = 0,
    PACKET_ERRCODE_ERR_OPCODE,
    PACKET_ERRCODE_ERR_OVERFLOW,
    PACKET_ERRCODE_ERR_TIMEOUT,
    PACKET_ERRCODE_ERR_FORMAT,
    PACKET_ERRCODE_ERR_VERIFY,
    PACKET_ERRCODE_ERR_FAILED = 6,   /* 操作执行失败 */
    PACKET_ERRCODE_ERR_PARAM  = 0xFF
} PACKET_errcode_t;

typedef enum
{
    INQUERY_SUBCODE_VERSION   = 0x00,
    INQUERY_SUBCODE_MTU       = 0x01,
    INQUERY_SUBCODE_FW_STATUS = 0x02  /* 查询W25Q128固件版本/分区状态 */
} inquery_subcode_t;

static packet_state_machine_t packet_state = PACKET_STATE_HEADER;
static uint8_t packet_buf[PACKET_SIZE_MAX];
static uint16_t packet_index;
static packet_opcode_t packet_opcode;
static uint16_t packet_payload_length;

static uint8_t rb_buf[RX_BUFFER_SIZE];
static rb_t rxrb;

/**
 * @brief Validate the application firmware.
 *
 * @return true if the application is valid.
 * @return false if the application is invalid.
 */
static bool application_validate(void)
{
    if (!magic_header_validate())
    {
        log_e("Magic header validation failed.");
        return false;
    }

    uint32_t app_address = magic_header_get_address();
    uint32_t app_length = magic_header_get_length();
    uint32_t app_crc32 = magic_header_get_crc32();
    uint32_t calculated_crc = crc32((uint8_t *)app_address, app_length);
    if (calculated_crc != app_crc32)
    {
        log_e("Application CRC32 mismatch: calculated 0x%08X, expected 0x%08X", calculated_crc, app_crc32);
        return false;
    }

    return true;
}

static void boot_application(void)
{
    if (!application_validate())
    {
        /* 内部Flash应用无效，尝试从W25Q128备份区自动恢复 */
        log_w("Application invalid, trying to restore from W25Q128...");
        if (fw_manager_flash_firmware() && application_validate())
        {
            log_i("Firmware restored successfully from W25Q128.");
        }
        else
        {
            log_e("Application validation failed, cannot boot.");
            return;
        }
    }

    log_w("booting application ... ");
    tim_delay_ms(2);

    /* 准备跳转前标记本次启动成功（清除失败计数和回滚标志） */
    fw_manager_mark_boot_success();

    led_off(led1);
    TIM_DeInit(TIM6);
    USART_DeInit(USART1);
    USART_DeInit(USART3);
    NVIC_DisableIRQ(TIM6_DAC_IRQn);
    NVIC_DisableIRQ(USART1_IRQn);
    NVIC_DisableIRQ(USART3_IRQn);

    SCB->VTOR = APP_BASE_ADDRESS;
    extern void JumpApp(uint32_t appAddress);
    JumpApp(APP_BASE_ADDRESS);
}
static void bl_response(packet_opcode_t opcode, PACKET_errcode_t errcode, const uint8_t *data, uint16_t length)
{
    uint8_t *response = packet_buf, *prsp = response;
    put_u8_inc(&prsp, PACKET_HEADER_RESPONSE);
    put_u8_inc(&prsp, (uint8_t)opcode);
    put_u8_inc(&prsp, (uint8_t)errcode);
    put_u16_inc(&prsp, length);
    put_bytes_inc(&prsp, data, length);
    uint16_t crc = crc16(response, prsp - response);
    put_u16_inc(&prsp, crc);

    bl_usart_write(response, prsp - response);
}

// static inline void bl_opcode_response_ack(packet_opcode_t opcode, PACKET_errcode_t errcode)
// {
//     bl_response(opcode, errcode, NULL, 0);
// }

static void bl_opcode_inquery_handler(void)
{
    log_i("Inquary handler.");
    if (packet_payload_length != 1)
    {
        log_e("Inquary packet length error");
        return;
    }

    uint8_t subcode = get_u8(packet_buf + PACKET_PAYLOAD_OFFSET);
    switch (subcode)
    {
        case INQUERY_SUBCODE_VERSION:
		{
			bl_response(PACKET_OPCODE_INQUERY, PACKET_ERRCODE_ERR_OK, (const uint8_t *)BL_VERSION, strlen(BL_VERSION));
            break;
		}
        case INQUERY_SUBCODE_MTU:
		{
			uint8_t mtu[2];
            put_u16(mtu, PAYLOAD_SIZE_MAX);
            bl_response(PACKET_OPCODE_INQUERY, PACKET_ERRCODE_ERR_OK, (const uint8_t *)&mtu, sizeof(mtu));
            break;
		}
        case INQUERY_SUBCODE_FW_STATUS:
        {
            /* 返回W25Q128双区分区状态（12字节）：
             * [active_zone(1)][zone_a_state(1)][zone_a_fw_ver(4)]
             * [zone_b_state(1)][zone_b_fw_ver(4)][boot_fail_count(1)] */
            const fw_meta_t *meta = fw_manager_get_meta();
            uint8_t status[12];
            uint8_t *p = status;
            put_u8_inc(&p,  meta->active_zone);
            put_u8_inc(&p,  meta->zone_a_state);
            put_u32_inc(&p, meta->zone_a_fw_version);
            put_u8_inc(&p,  meta->zone_b_state);
            put_u32_inc(&p, meta->zone_b_fw_version);
            put_u8_inc(&p,  meta->boot_fail_count);
            bl_response(PACKET_OPCODE_INQUERY, PACKET_ERRCODE_ERR_OK, status, sizeof(status));
            break;
        }
        default:
		{
            log_w("Inquary unknown subcode: %02X", subcode);
            break;
		}
    }
}

static void bl_opcode_erase_handler(void)
{
    log_i("Erase handler.");

    if (packet_payload_length != ADDR_SIZE_PARAM_LENGTH)
    {
        log_e("Erase packet length error: %d", packet_payload_length);
        bl_response(PACKET_OPCODE_ERASE, PACKET_ERRCODE_ERR_FORMAT, NULL, 0);
        return;
    }

    uint8_t *payload = packet_buf + PACKET_PAYLOAD_OFFSET;
    uint32_t address = get_u32_inc(&payload);// ��payload����ȡ����ַ�ֶΣ�address��ʾҪ������flash�ڴ���ʼ��ַ
    uint32_t size = get_u32_inc(&payload);   // ��payload����ȡ����С�ֶΣ�size��ʾҪ���������ݳ���

    if (address < STM32_FLASH_BASE || address + size > STM32_FLASH_BASE + STM32_FLASH_SIZE)
    {
        log_e("Erase address: 0x%08X, size: %u out of range", address, size);
        bl_response(PACKET_OPCODE_ERASE, PACKET_ERRCODE_ERR_PARAM, NULL, 0);
        return;
    }

    if (address >= BL_ADDRESS && address < BL_ADDRESS + BL_SIZE)
    {
        log_e("address 0x%08X is protected", address);
        bl_response(PACKET_OPCODE_ERASE, PACKET_ERRCODE_ERR_PARAM, NULL, 0);
        return;
    }

    log_d("Erase address: 0x%08X, size: %u", address, size);

    stm32_flash_unlock();
    stm32_flash_erase(address, size);
    stm32_flash_lock();

    bl_response(PACKET_OPCODE_ERASE, PACKET_ERRCODE_ERR_OK, NULL, 0);
}

static void bl_opcode_program_handler(void)
{
    log_i("Program handler.");

    if (packet_payload_length <= ADDR_SIZE_PARAM_LENGTH)
    {
        log_e("Program packet length error: %d", packet_payload_length);
        bl_response(PACKET_OPCODE_PROGRAM, PACKET_ERRCODE_ERR_FORMAT, NULL, 0);
        return;
    }

    uint8_t *payload = packet_buf + PACKET_PAYLOAD_OFFSET;
    uint32_t address = get_u32_inc(&payload);// ��payload����ȡ����ַ�ֶΣ�address��ʾҪд���flash�ڴ���ʼ��ַ
    uint32_t size = get_u32_inc(&payload);   // ��payload����ȡ����С�ֶΣ�size��ʾҪд������ݳ���
    uint8_t *data = payload;                 // dataָ��ʣ���payload���ݣ���Ҫд��flash������

    if (address < STM32_FLASH_BASE || address + size > STM32_FLASH_BASE + STM32_FLASH_SIZE)
    {
        log_e("Program address: 0x%08X, size: %u out of range", address, size);
        bl_response(PACKET_OPCODE_PROGRAM, PACKET_ERRCODE_ERR_PARAM, NULL, 0);
        return;
    }

    if (address >= BL_ADDRESS && address < BL_ADDRESS + BL_SIZE)
    {
        log_e("address 0x%08X is in protected bootloader region", address);
        bl_response(PACKET_OPCODE_PROGRAM, PACKET_ERRCODE_ERR_PARAM, NULL, 0);
        return;
    }

    if (size !=packet_payload_length - ADDR_SIZE_PARAM_LENGTH)
    {
        log_e("Program size %u does not match packet payload length %u", size, packet_payload_length - ADDR_SIZE_PARAM_LENGTH);
        bl_response(PACKET_OPCODE_PROGRAM, PACKET_ERRCODE_ERR_FORMAT, NULL, 0);
        return;
    }

    log_i("Program address: 0x%08X, size: %u", address, size);

    stm32_flash_unlock();
    stm32_flash_program(address, data, size);
    stm32_flash_lock();

    bl_response(PACKET_OPCODE_PROGRAM, PACKET_ERRCODE_ERR_OK, NULL, 0);
}

static void bl_opcode_verify_handler(void)
{
    log_i("Verify handler.");

    if (packet_payload_length != ADDR_SIZE_CRC_PARAM_LENGTH)
    {
        log_e("Verify packet length error: %d", packet_payload_length);
        bl_response(PACKET_OPCODE_VERIFY, PACKET_ERRCODE_ERR_FORMAT, NULL, 0);
        return;
    }

    uint8_t *payload = packet_buf + PACKET_PAYLOAD_OFFSET;
    uint32_t address = get_u32_inc(&payload);// ��payload����ȡ����ַ�ֶΣ�address��ʾҪ��֤��flash�ڴ���ʼ��ַ
    uint32_t size = get_u32_inc(&payload);   // ��payload����ȡ����С�ֶΣ�size��ʾҪ��֤�����ݳ���
    uint32_t crc = get_u32_inc(&payload);    // ��payload����ȡ��CRC32�ֶΣ�crc��ʾԤ�ڵ�CRC32У��ֵ

    if (address < STM32_FLASH_BASE || address + size > STM32_FLASH_BASE + STM32_FLASH_SIZE)
    {
        log_e("Verify address: 0x%08X, size: %u out of range", address, size);
        bl_response(PACKET_OPCODE_VERIFY, PACKET_ERRCODE_ERR_PARAM, NULL, 0);
        return;
    }

    log_d("Verify address: 0x%08X, size: %u, CRC: 0x%08X", address, size, crc);

    uint32_t calculated_crc = crc32((uint8_t *)address, size);//����ָ����ַ�ʹ�С��Χ�ڵ����ݵ�CRC32У��ֵ��ָ����ַΪflash�ڴ���ʼ��ַ
    if (calculated_crc != crc)
    {
        log_e("Verify CRC mismatch: calculated 0x%08X, expected 0x%08X", calculated_crc, crc);
        bl_response(PACKET_OPCODE_VERIFY, PACKET_ERRCODE_ERR_VERIFY, NULL, 0);
        return;
    }

    bl_response(PACKET_OPCODE_VERIFY, PACKET_ERRCODE_ERR_OK, NULL, 0);
}

/**
 * @brief 0x84 FW_WRITE — 接收加密固件分片并写入W25Q128
 *
 * Payload格式：[total_size(4B)][chunk_offset(4B)][encrypted_data(N)]
 *   - total_size  : 固件总大小（每个包都携带，chunk_offset==0时触发擦区）
 *   - chunk_offset: 本片数据在固件中的字节偏移
 *   - N           : packet_payload_length - 8，最大4096字节
 */
static void bl_opcode_fw_write_handler(void)
{
    log_i("FW_Write handler.");

    if (packet_payload_length <= FW_WRITE_PARAM_HEADER_LEN)
    {
        log_e("FW_Write: payload too short: %d", packet_payload_length);
        bl_response(PACKET_OPCODE_FW_WRITE, PACKET_ERRCODE_ERR_FORMAT, NULL, 0);
        return;
    }

    uint8_t *payload      = packet_buf + PACKET_PAYLOAD_OFFSET;
    uint32_t total_size   = get_u32_inc(&payload);  /* 固件总大小 */
    uint32_t chunk_offset = get_u32_inc(&payload);  /* 本片在固件中的偏移 */
    uint8_t *data         = payload;                /* 加密数据起始指针 */
    uint16_t data_size    = (uint16_t)(packet_payload_length - FW_WRITE_PARAM_HEADER_LEN);

    /* 第一片（offset==0）：选区+擦除 */
    if (chunk_offset == 0)
    {
        log_i("FW_Write: begin_write, total_size=%lu", (unsigned long)total_size);
        if (!fw_manager_begin_write(total_size))
        {
            log_e("FW_Write: begin_write failed");
            bl_response(PACKET_OPCODE_FW_WRITE, PACKET_ERRCODE_ERR_FAILED, NULL, 0);
            return;
        }
    }

    /* 写入本片数据 */
    if (!fw_manager_write_chunk(chunk_offset, data, data_size))
    {
        log_e("FW_Write: write_chunk failed at offset 0x%08lX", (unsigned long)chunk_offset);
        bl_response(PACKET_OPCODE_FW_WRITE, PACKET_ERRCODE_ERR_FAILED, NULL, 0);
        return;
    }

    log_d("FW_Write: chunk offset=0x%08lX size=%u OK", (unsigned long)chunk_offset, data_size);
    bl_response(PACKET_OPCODE_FW_WRITE, PACKET_ERRCODE_ERR_OK, NULL, 0);
}

/**
 * @brief 0x85 FW_COMMIT — 提交固件：解密验证CRC32 → 更新元数据 → 烧录到内部Flash → 复位
 *
 * Payload格式：[nonce(8B)][fw_version(4B)][crc32(4B)] = 16字节
 *
 *   nonce (8B)
 *     AES-128 CTR 模式的初始化向量前半段（Nonce = Number Used Once）。
 *     上位机在加密固件时随机生成此 8 字节值，并将其与加密后的固件一同管理。
 *     CTR 计数器块格式为 [nonce(8B)][counter_be(4B)][zeros(4B)]，counter 从 0 开始
 *     每处理 16 字节递增一次。bootloader 拿到 nonce 后用相同参数对 W25Q128 中存储的
 *     密文进行解密，从而还原出原始固件内容并进行 CRC32 校验。
 *     同一份固件每次升级建议使用不同的 nonce，以防重放攻击。
 *
 *   fw_version (4B)  固件版本号，uint32_t，写入存储区元数据供后续版本管理使用。
 *   crc32 (4B)       明文固件的 CRC32 校验值，bootloader 解密后对比此值以验证完整性。
 *
 * 流程：先完成验证+元数据更新，验证通过后发送ACK，再执行烧录并复位。
 * 上位机收到ACK后等待设备重连即可；若收到ERR_VERIFY则说明固件CRC校验失败。
 */
static void bl_opcode_fw_commit_handler(void)
{
    log_i("FW_Commit handler.");

    if (packet_payload_length != FW_COMMIT_PARAM_LEN)
    {
        log_e("FW_Commit: payload length error: %d (expected %d)",
              packet_payload_length, FW_COMMIT_PARAM_LEN);
        bl_response(PACKET_OPCODE_FW_COMMIT, PACKET_ERRCODE_ERR_FORMAT, NULL, 0);
        return;
    }

    /* 使用增量指针解析payload，避免魔法数字偏移 */
    const uint8_t *p = packet_buf + PACKET_PAYLOAD_OFFSET;
    uint8_t nonce[8];
    get_bytes_inc(&p, nonce, 8);                    /* nonce: 8字节 */
    uint32_t fw_version = get_u32_inc((uint8_t **)&p); /* fw_version: 4字节 */
    uint32_t crc32_val  = get_u32_inc((uint8_t **)&p); /* crc32: 4字节 */

    log_i("FW_Commit: fw_version=%lu, crc32=0x%08lX",
          (unsigned long)fw_version, (unsigned long)crc32_val);

    /* 先执行验证+元数据更新（可能需要数秒完成CRC解密校验）：
     * commit_write 返回 false 表示验证失败，此时发送错误响应并返回。
     * commit_write 返回 true 表示验证和元数据更新成功，可以进入烧录阶段。 */
    if (!fw_manager_commit_write(nonce, fw_version, crc32_val))
    {
        log_e("FW_Commit: CRC32 verify failed");
        bl_response(PACKET_OPCODE_FW_COMMIT, PACKET_ERRCODE_ERR_VERIFY, NULL, 0);
        return;
    }

    /* 验证通过，发送ACK（上位机收到OK后等待设备重连） */
    bl_response(PACKET_OPCODE_FW_COMMIT, PACKET_ERRCODE_ERR_OK, NULL, 0);
    tim_delay_ms(5); /* 确保ACK帧发送完成 */

    /* 解密烧录到内部Flash，烧录成功后复位系统 */
    log_i("FW_Commit: flashing firmware...");
    fw_manager_flash_firmware();
    NVIC_SystemReset();
}

/**
 * @brief 0x86 FW_ROLLBACK — 手动请求回滚到备份区
 *
 * Payload：空（长度为0）
 * 调用成功后系统将复位并从备份区固件启动。
 *
 * 响应策略：先检查备份区有效性，再决定响应码：
 *   - 备份区有固件(ZONE_STATE_VALID)：发送 ERR_OK，执行回滚+复位（不会返回）
 *   - 备份区无有效固件              ：发送 ERR_FAILED，直接返回（不触发复位）
 * 这样可以确保上位机收到的响应码与实际操作结果一致，避免误导性的 ERR_OK。
 */
static void bl_opcode_fw_rollback_handler(void)
{
    log_i("FW_Rollback handler.");

    if (packet_payload_length != 0)
    {
        log_e("FW_Rollback: payload length error: %d", packet_payload_length);
        bl_response(PACKET_OPCODE_FW_ROLLBACK, PACKET_ERRCODE_ERR_FORMAT, NULL, 0);
        return;
    }

    /* 在响应前检查备份区是否有有效固件，避免先发 ERR_OK 后执行失败 */
    const fw_meta_t *meta = fw_manager_get_meta();
    uint8_t backup_zone  = (meta->active_zone == 0U) ? 1U : 0U;
    uint8_t backup_state = (backup_zone == 0U) ? meta->zone_a_state : meta->zone_b_state;

    if (backup_state != (uint8_t)ZONE_STATE_VALID)
    {
        log_e("FW_Rollback: no valid firmware in backup zone_%c",
              (backup_zone == 0U) ? 'A' : 'B');
        bl_response(PACKET_OPCODE_FW_ROLLBACK, PACKET_ERRCODE_ERR_FAILED, NULL, 0);
        return;
    }

    /* 备份区有效 — 先发送 ACK，再执行回滚+复位（复位后不会返回此处） */
    bl_response(PACKET_OPCODE_FW_ROLLBACK, PACKET_ERRCODE_ERR_OK, NULL, 0);
    tim_delay_ms(5); /* 确保 ACK 帧发送完成 */

    log_w("FW_Rollback: initiating rollback to zone_%c...",
          (backup_zone == 0U) ? 'A' : 'B');
    fw_manager_request_rollback();
    fw_manager_check_rollback(); /* 内部调用 NVIC_SystemReset()，正常不会返回 */

    /* Safety fallsafe: if check_rollback unexpectedly returns, force a reset */
    log_e("FW_Rollback: unexpected return from check_rollback, forcing reset");
    NVIC_SystemReset();
}

static void bl_opcode_reset_handler(void)
{
    log_i("Reset handler.");
    bl_response(PACKET_OPCODE_RESET, PACKET_ERRCODE_ERR_OK, NULL, 0);
    log_w("system resetting...");
    tim_delay_ms(2);
    // ִ��ϵͳ��λ
    NVIC_SystemReset();
}

static void bl_opcode_boot_handler(void)
{
    log_i("Boot handler.");
    bl_response(PACKET_OPCODE_BOOT, PACKET_ERRCODE_ERR_OK, NULL, 0);
    boot_application();
}

static void bl_packet_handler(void)
{
    switch (packet_opcode)
    {
        case PACKET_OPCODE_INQUERY:
            bl_opcode_inquery_handler();
            break;
        case PACKET_OPCODE_ERASE:
             bl_opcode_erase_handler();
            break;
        case PACKET_OPCODE_PROGRAM:
             bl_opcode_program_handler();
            break;
        case PACKET_OPCODE_VERIFY:
             bl_opcode_verify_handler();
            break;
        case PACKET_OPCODE_FW_WRITE:
             bl_opcode_fw_write_handler();
            break;
        case PACKET_OPCODE_FW_COMMIT:
             bl_opcode_fw_commit_handler();
            break;
        case PACKET_OPCODE_FW_ROLLBACK:
             bl_opcode_fw_rollback_handler();
            break;
        case PACKET_OPCODE_RESET:
             bl_opcode_reset_handler();
            break;
        case PACKET_OPCODE_BOOT:
             bl_opcode_boot_handler();
            break;
        default:
            log_w("Unknown opcode: %02X", packet_opcode);
            break;
    }
}

static bool bl_byte_handler(uint8_t byte)
{
    bool full_packet = false;
    // �����ֽ����ݳ�ʱ����
    static uint64_t last_byte_ms;
    uint64_t now_ms = tim_get_ms();
    if (now_ms - last_byte_ms > RX_TIMEOUT_MS)
    {
        if (packet_state != PACKET_STATE_HEADER)
            log_w("last packet rx timeout");
        packet_index = 0;
        packet_state = PACKET_STATE_HEADER;
    }
    last_byte_ms = now_ms;

    //�ֽڽ���״̬������
    //printf("recv: %02X", byte); // ������������ֽ�,��ʱ,����Ҫ�ر�
    log_v("recv: %02X", byte); // ��ϸ��־������������ֽ�
    packet_buf[packet_index++] = byte;
    switch (packet_state)
    {
        case PACKET_STATE_HEADER:
            if (packet_buf[PACKET_HEADER_OFFSET] == PACKET_HEADER_REQUEST)
                {
					log_d("header ok");
                    packet_state = PACKET_STATE_OPCODE;
                }
                else
                {
                    log_w("header error: %02X", packet_buf[PACKET_HEADER_OFFSET]);
                    // �������ͷ���ֽڣ�����״̬��
                    packet_index = 0;
                    packet_state = PACKET_STATE_HEADER;
                }
                break;
            case PACKET_STATE_OPCODE:
                if (packet_buf[PACKET_OPCODE_OFFSET] == PACKET_OPCODE_INQUERY ||
                    packet_buf[PACKET_OPCODE_OFFSET] == PACKET_OPCODE_ERASE ||
                    packet_buf[PACKET_OPCODE_OFFSET] == PACKET_OPCODE_PROGRAM ||
                    packet_buf[PACKET_OPCODE_OFFSET] == PACKET_OPCODE_VERIFY ||
                    packet_buf[PACKET_OPCODE_OFFSET] == PACKET_OPCODE_FW_WRITE ||
                    packet_buf[PACKET_OPCODE_OFFSET] == PACKET_OPCODE_FW_COMMIT ||
                    packet_buf[PACKET_OPCODE_OFFSET] == PACKET_OPCODE_FW_ROLLBACK ||
                    packet_buf[PACKET_OPCODE_OFFSET] == PACKET_OPCODE_RESET ||
                    packet_buf[PACKET_OPCODE_OFFSET] == PACKET_OPCODE_BOOT)
                {
					log_d("opcode ok: %02X", packet_buf[PACKET_OPCODE_OFFSET]);
                    packet_opcode = (packet_opcode_t)packet_buf[PACKET_OPCODE_OFFSET];
                    packet_state = PACKET_STATE_LENGTH;
                }
                else
                {
                    log_w("opcode error: %02X", packet_buf[PACKET_OPCODE_OFFSET]);
                    // ���������Ч�Ĳ����룬����״̬��
                    packet_index = 0;
                    packet_state = PACKET_STATE_HEADER;
                }
                break;
            case PACKET_STATE_LENGTH:
                if (packet_index == PACKET_PAYLOAD_OFFSET)
                {
                    uint16_t payload_length = get_u16(packet_buf + PACKET_LENGTH_OFFSET); // ��һ��д����&packet_buf[PACKET_LENGTH_OFFSET]
                    if (payload_length <= PAYLOAD_SIZE_MAX)
                    {
						log_d("length ok: %u", payload_length);
                        packet_payload_length = payload_length;
                        if (packet_payload_length > 0)
                        packet_state = PACKET_STATE_PAYLOAD;
                        else
                        packet_state = PACKET_STATE_CRC16;
                    }
                    else
                    {
                        log_w("length error: %u, max is %u", payload_length, PAYLOAD_SIZE_MAX);
                        // ������ȳ������ֵ������״̬��
                        packet_index = 0;
                        packet_state = PACKET_STATE_HEADER;
                    }
                }
                break;
            case PACKET_STATE_PAYLOAD:
                if (packet_index == PACKET_PAYLOAD_OFFSET + packet_payload_length)
                {
					log_d("payload receive ok");
                    packet_state = PACKET_STATE_CRC16;
                }
                break;
                case PACKET_STATE_CRC16:
                if (packet_index == PACKET_MIN_SIZE + packet_payload_length)
                {
                    uint16_t crc = get_u16(packet_buf + PACKET_PAYLOAD_OFFSET + packet_payload_length);
                    uint16_t calculated_crc = crc16(packet_buf, PACKET_PAYLOAD_OFFSET + packet_payload_length);
                    if (crc == calculated_crc)
                    {
                        full_packet = true;
                        log_d("crc16 ok: %04X", crc);
                        log_d("packet received: optcode=%02X, length=%u", packet_opcode, packet_payload_length);
                        if (LOG_LVL >=ELOG_LVL_VERBOSE)
                            elog_hexdump("payload", 16, packet_buf, PACKET_MIN_SIZE + packet_payload_length);

                            //��ʱ�ĵ������,����Ҫ�ر�
                        // printf("payloda: ");
                        // for (uint32_t i = 0; i < packet_payload_length; i++)
                        // {
                        //     printf("%02X ", packet_buf[4 + i]);
                        // }
                        // printf("");
                    }
                    else
                    {
                        log_w("crc16 error: expected %04X, got %04X", crc, calculated_crc);
                    }

                    packet_index = 0;
                    packet_state = PACKET_STATE_HEADER;
                }
                break;
            default:
            break;
    }
    return full_packet;
}

static void bl_rx_handler(const uint8_t *data, uint32_t length)
{
    rb_puts(rxrb, data, length);
}

static bool key_trap_check(void)
{   // ��ⰴ���Ƿ���BOOT_DELAYʱ���ڱ�����
    for (uint32_t t = 0; t < BOOT_DELAY; t += 10)
    {
        tim_delay_ms(10);
        if (!key_read(key3))
            return false;
    }
    log_w("Key pressed, entering bootloader mode.");
    return true;
}

static void wait_key_release(void)
{
    while (key_read(key3))
        tim_delay_ms(10);
}

static bool key_press_check(void)
{
    if (!key_read(key3))
        return false;

    tim_delay_ms(10);
    if (!key_read(key3))
        return false;

    return true;
}

bool magic_header_trap_boot(void)
{
    /* 应用有效，直接启动（无需进入Bootloader模式） */
    if (magic_header_validate() && application_validate())
        return false;

    /* 内部Flash应用无效，尝试从W25Q128备份区自动恢复 */
    log_w("Application invalid, attempting auto-restore from W25Q128 backup...");
    const fw_meta_t *meta = fw_manager_get_meta();
    bool has_backup = (meta->zone_a_state == (uint8_t)ZONE_STATE_VALID) ||
                      (meta->zone_b_state == (uint8_t)ZONE_STATE_VALID);

    if (has_backup)
    {
        log_i("Restoring firmware from W25Q128...");
        if (fw_manager_flash_firmware())
        {
            if (magic_header_validate() && application_validate())
            {
                log_i("Auto-restore successful, booting application.");
                return false;  /* 恢复成功，正常启动，不进入Bootloader模式 */
            }
        }
        log_e("Auto-restore failed, entering bootloader mode.");
    }
    else
    {
        log_w("No valid backup in W25Q128, entering bootloader mode.");
    }

    return true;
}

bool rx_trap_boot(void)
{
    for (uint32_t t = 0; t < BOOT_DELAY; t += 10)
    {
        tim_delay_ms(10);
        if (!rb_empty(rxrb))
        {
            log_w("Data received, entering bootloader mode.");
            return true;
        }
    }

    return false;
}

void bootloader_main(void)
{
    log_i("Bootloader started.");

    key_init(key3);

    rxrb = rb_new(rb_buf, RX_BUFFER_SIZE);
    bl_usart_init();
    bl_usart_register_rx_callback(bl_rx_handler);


    bool trap_boot = false;

    if (!trap_boot)
        trap_boot = magic_header_trap_boot();

    if (!trap_boot)
        trap_boot = key_trap_check();

    if (!trap_boot)
        trap_boot = rx_trap_boot();

    if (!trap_boot)
        boot_application();

    led_init(led1);
    led_on(led1);
    wait_key_release();

    while (1)
    {
        if (key_press_check())
        {
            log_w("key pressed, rebooting...");
            tim_delay_ms(2);
            NVIC_SystemReset();
        }
        if (!rb_empty(rxrb))
        {
            uint8_t byte;
            rb_get(rxrb, &byte);
            if (bl_byte_handler(byte))
            {
                bl_packet_handler();
            }
        }
    }
}
