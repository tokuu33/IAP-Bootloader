# IAP-Bootloader

基于 STM32F407VET6 的 IAP（In-Application Programming）Bootloader，支持通过 USART3 进行固件升级。

---

## 功能概述

| 功能 | 说明 |
|------|------|
| 固件升级 | 通过 USART3（115200 baud, 8-N-1）接收并烧写固件 |
| 固件加密 | AES-128-CTR 软件加密（不依赖硬件 AES） |
| 双区备份 | W25Q128 外部 Flash 双区（Zone A / Zone B）固件备份 |
| 版本回滚 | 一键回滚到 W25Q128 中存储的上一版本固件 |
| 自检验证 | CRC32 校验 + Magic Header 完整性检查 |
| 按键进入 | 长按 KEY3 ≥ 3 秒强制进入 Bootloader 模式 |

---

## 硬件平台

- MCU：STM32F407VET6（512 KB 内部 Flash，192 KB RAM）
- 外部 Flash：W25Q128（16 MB，Winbond），通过 SPI1 连接
  - SCK → PA5，MISO → PA6，MOSI → PA7，CS → PE13
- 升级串口：USART3（TX → PB10，RX → PB11）
- 日志串口：USART1（TX → PA9，RX → PA10）

---

## Flash 地址分配

### STM32F407 内部 Flash（512 KB）

| 区域 | 地址范围 | 大小 | 说明 |
|------|---------|------|------|
| Bootloader | 0x08000000 – 0x0800BFFF | 48 KB | 本程序 |
| Magic Header | 0x0800C000 – 0x0800FFFF | 16 KB | 固件元数据（版本、CRC32 等） |
| Application | 0x08010000 – 0x080FFFFF | 最大 448 KB | 用户应用程序 |

### W25Q128 外部 Flash（16 MB）

| 区域 | 地址范围 | 大小 | 说明 |
|------|---------|------|------|
| Zone A Header | 0x000000 – 0x000FFF | 4 KB | Zone A 元数据（版本、CRC、IV 等） |
| Zone A Data   | 0x001000 – 0x7FFFFF | ~8 MB | Zone A 加密固件数据 |
| Zone B Header | 0x800000 – 0x800FFF | 4 KB | Zone B 元数据 |
| Zone B Data   | 0x801000 – 0xFFFFFF | ~8 MB | Zone B 加密固件数据 |

---

## 通信协议

### 数据包格式

```
[Header(1B)] [Opcode(1B)] [Length(2B)] [Payload(N B)] [CRC16(2B)]
```

- Header：0xAA = 请求，0x55 = 响应
- CRC16：XMODEM 多项式，覆盖 Header 至 Payload 末尾

### 操作码

| 操作码 | 名称 | 说明 |
|--------|------|------|
| 0x01 | INQUERY | 查询版本号/MTU |
| 0x81 | ERASE | 擦除 STM32 内部 Flash |
| 0x82 | PROGRAM | 向 STM32 内部 Flash 写入明文固件 |
| 0x83 | VERIFY | 校验 STM32 内部 Flash CRC32 |
| **0x84** | **EXT_ERASE** | **擦除 W25Q128 指定 Zone** |
| **0x85** | **EXT_PROGRAM** | **向 W25Q128 Zone 写入加密固件数据** |
| **0x86** | **EXT_SEAL** | **写入 Zone Header（完成上传流程）** |
| **0x87** | **INSTALL** | **从 W25Q128 Zone 解密并安装固件** |
| **0x88** | **ROLLBACK** | **回滚到 W25Q128 中的上一版本固件** |
| 0x21 | RESET | 系统复位 |
| 0x22 | BOOT | 跳转到应用程序 |

---

## 固件加密说明

- 算法：**AES-128-CTR**（流密码，无需填充，软件实现）
- 密钥：16 字节，位于 `app/fw_crypto.c` 中的 `fw_crypto_default_key`
  - **⚠️ 量产前必须替换为自定义密钥，且主机工具必须使用同一密钥加密固件**
- IV（初始向量）：每次上传固件时由主机生成随机 16 字节，通过 EXT_SEAL 写入 Zone Header
- 加密数据长度 = 明文数据长度（CTR 模式无需填充）

---

## 固件升级流程（加密 + 备份）

```
主机端：
  1. 使用 AES-128-CTR 加密固件
  2. 计算明文 CRC32 和加密数据 CRC32
  3. 生成随机 IV（16 字节）

通过 UART 发送：
  4. EXT_ERASE(zone, fw_size)        → 擦除 Zone 空间
  5. EXT_PROGRAM(zone, 0, chunk1)    → 写加密数据（分块，每块 ≤ 4096B）
     EXT_PROGRAM(zone, n, chunk2)
     ...
  6. EXT_SEAL(zone, ver, ver_str,    → 写 Zone Header，完成上传
             fw_size, fw_crc32,
             enc_size, enc_crc32,
             iv, app_address)
  7. INSTALL(zone)                   → 解密 + 写入 STM32 Flash + 验证 CRC32
  8. BOOT                            → 跳转到新固件
```

---

## 版本回滚流程

```
  1. ROLLBACK  →  Bootloader 自动在 W25Q128 中寻找与当前版本不同的有效 Zone
                  找到后执行 INSTALL，完成回滚
  2. BOOT      →  跳转到回滚后的固件
```

---

## 版本历史

- v0.9.0：基础 IAP 功能（USART 升级、CRC 校验、按键进入）
- v1.0.0：新增 AES-128-CTR 固件加密、W25Q128 双区备份、版本回滚

