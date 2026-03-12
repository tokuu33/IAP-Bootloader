# IAP-Bootloader

基于 STM32F407VET6 的 IAP (In-Application Programming) 引导加载程序，支持 UART 固件升级、AES-128 软件加密、W25Q128 双区备份与版本回滚。

---

## 硬件平台

| 项目 | 详情 |
|------|------|
| MCU | STM32F407VET6（无硬件 AES） |
| 内部 Flash | 512 KB（0x08000000） |
| 外部 Flash | W25Q128（128 Mbit，SPI1 @ PA5/PA6/PA7，CS=PE13） |
| 通信串口 | USART3（PB10=TX, PB11=RX，115200-8N1） |
| 调试串口 | USART1 |
| 指示 LED | PE9（LED1） |
| 进入按键 | PC5（KEY3，持续按住 3 s 进入升级模式） |

---

## 内部 Flash 内存映射

```
地址范围                大小    用途
─────────────────────────────────────────────────────
0x08000000–0x0800BFFF   48 KB   Bootloader 代码
0x0800C000–0x0800FFFF   16 KB   Magic Header（固件元数据）
0x08010000–0x0807FFFF  448 KB   应用程序（Sector 4–7）
```

---

## W25Q128 地址映射（双区备份）

```
地址范围                大小    用途
─────────────────────────────────────────────────────
0x000000–0x000FFF        4 KB   备份区 0 元数据 Header
0x001000–0x07FFFF      508 KB   备份区 0 固件数据（最新备份）
0x200000–0x200FFF        4 KB   备份区 1 元数据 Header
0x201000–0x27FFFF      508 KB   备份区 1 固件数据（上一版本，回滚目标）
```

---

## 功能说明

### 1. UART 升级协议

波特率 115200-8N1；数据包格式：

```
[Header(1B)] [Opcode(1B)] [Length(2B)] [Payload(0–4120B)] [CRC16(2B)]

请求包 Header = 0xAA
响应包 Header = 0x55
```

| Opcode | 值 | 功能 | 载荷说明 |
|--------|-----|------|---------|
| INQUERY | 0x01 | 查询版本/MTU | 子码 0x00=版本, 0x01=MTU |
| ERASE | 0x81 | 擦除内部 Flash | addr(4B) + size(4B) |
| PROGRAM | 0x82 | 写明文固件 | addr(4B) + size(4B) + data |
| VERIFY | 0x83 | 校验 Flash CRC32 | addr(4B) + size(4B) + crc32(4B) |
| RESET | 0x21 | 系统复位 | 无 |
| BOOT | 0x22 | 跳转至应用（自动备份） | 无 |
| ROLLBACK | 0x23 | **手动回滚到上一备份** | 无 |
| BACKUP | 0x24 | **手动备份当前固件** | 无 |
| CRYPTO_INIT | 0x84 | **初始化 AES-CBC 上下文（设置 IV）** | iv(16B) |
| PROGRAM_ENC | 0x85 | **写加密固件（AES-128-CBC 解密后写入）** | addr(4B) + size(4B) + encrypted_data |

---

### 2. AES-128-CBC 固件加密

STM32F407 不带硬件 AES 加速单元，本方案使用 TinyAES（软件实现）进行 AES-128-CBC 解密。

**加密流程（主机侧）：**

1. 使用与 Bootloader 相同的 16 字节密钥对固件二进制文件进行 AES-128-CBC 加密（填充至 16 字节对齐）。
2. 生成一个随机 16 字节 IV。
3. 发送 `CRYPTO_INIT`（0x84）携带 IV。
4. 按顺序发送 `PROGRAM_ENC`（0x85）分包（每包数据最大 4096 字节，须为 16 字节倍数）。
5. 发送 `VERIFY`（0x83）对解密后写入的明文数据计算 CRC32 校验。

**密钥配置：**

密钥为编译期常量，定义于 `app/fw_crypto.c`。生产环境请通过编译选项覆盖：

```
-DFW_CRYPTO_DEFAULT_KEY="{0x11,0x22,...,0xNN}"
```

同一密钥须配置到主机端固件加密工具中。

---

### 3. W25Q128 双区备份与版本回滚

**自动备份（BOOT 命令触发）：**

执行 BOOT（0x22）命令跳转应用之前，Bootloader 自动将当前固件备份到 W25Q128：

- 若备份区 0 已有有效固件，先将其复制到备份区 1（保留上一版本供回滚）；
- 然后将当前应用固件保存到备份区 0（最新版本）。

**手动备份（BACKUP 命令）：**

发送 BACKUP（0x24）可随时显式触发备份操作。

**手动回滚（ROLLBACK 命令）：**

发送 ROLLBACK（0x23），Bootloader 将：

1. 查找最佳可用备份区（优先备份区 0，其次备份区 1）；
2. 擦除并重写内部 Flash 中的应用区；
3. 重建 Magic Header；
4. 返回成功响应，主机可再发 BOOT 命令启动回滚后的固件。

**自动回滚（上电检测）：**

上电时若 Magic Header 无效或应用 CRC32 校验失败，Bootloader 自动尝试从 W25Q128 最新有效备份恢复固件，无需主机介入。恢复成功后直接启动，失败则进入升级模式等待主机连接。

---

## 启动流程

```
上电
 │
 ├─ Magic Header 有效 & 应用 CRC 正确？
 │   否 ──→ 自动回滚（W25Q128）
 │             ├─ 回滚成功 ──→ 跳转应用
 │             └─ 回滚失败 ──→ 进入升级模式
 │   是
 ├─ KEY3 持续按住 3 s？是 ──→ 进入升级模式
 ├─ UART 3 s 内收到数据？是 ──→ 进入升级模式
 └─ 否 ──→ 跳转应用
```

---

## 第三方库

| 库 | 路径 | 用途 |
|----|------|------|
| TinyAES | `third_lib/tinyaes/` | AES-128 ECB/CBC/CTR 软件实现 |
| CRC16/CRC32 | `third_lib/crc/` | 数据包及固件完整性校验 |
| EasyLogger | `third_lib/easylogger/` | 分级日志输出 |
| RingBuffer | `third_lib/ringbuffer/` | UART 接收环形缓冲区 |

---

## 目录结构

```
IAP-Bootloader/
├── app/
│   ├── main.c            # 入口，初始化外设
│   ├── bootload.c        # 协议状态机、命令处理、启动逻辑
│   ├── magic_header.c/h  # 内部 Flash 固件元数据读写
│   ├── fw_crypto.c/h     # AES-128-CBC 固件解密模块（新增）
│   ├── backup_mgr.c/h    # W25Q128 双区备份管理器（新增）
│   ├── board.c/h         # 硬件初始化（GPIO/时钟）
│   └── jumpapp.s         # ARM 汇编：跳转至应用
├── driver/
│   ├── w25q128.c/h       # W25Q128 SPI Flash 驱动
│   ├── stm32_flash.c/h   # 内部 Flash 擦写驱动
│   ├── bl_usart.c/h      # 升级用 USART3 驱动
│   └── ...
├── third_lib/
│   ├── tinyaes/          # AES 软件库
│   ├── crc/              # CRC 计算库
│   ├── easylogger/       # 日志库
│   └── ringbuffer/       # 环形缓冲区
└── firmware/             # STM32F4 标准外设库（CMSIS + HAL）
```

