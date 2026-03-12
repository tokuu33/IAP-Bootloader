# 上位机适配说明 (Upper-PC Adaptation)

本目录包含与新版 Bootloader 协议对齐后的上位机 C# 源文件，  
用于替换 [yhy666610/iap_upperpc](https://github.com/yhy666610/IAP_upperpc) 仓库中的同名文件。

## 替换方式

将以下四个文件直接复制到上位机项目根目录，覆盖原文件即可：

| 本目录文件               | 替换目标（iap_upperpc 仓库）    |
|--------------------------|-------------------------------|
| `FwCrypto.cs`            | *(新增，直接添加到项目)*        |
| `Transfer.cs`            | `Transfer.cs`                 |
| `Form1.cs`               | `Form1.cs`                    |
| `Form1.Designer.cs`      | `Form1.Designer.cs`           |

> `CRC.cs`、`MagicHeader.cs`、`Program.cs`、`SerialUpgrader.csproj`  
> **无需修改**，保持原版本即可。

---

## 主要改动说明

### 1. FwCrypto.cs（新增）

实现 **AES-128-CTR** 固件加密，与下位机 `fw_crypto.c` 算法完全一致：

- 计数器块格式：`[nonce(8B)][counter_be(4B)][zeros(4B)]`  
- 使用与下位机相同的默认密钥（**生产环境必须替换**）。  
- 提供 `FwCrypto.Xcrypt(data, nonce)` 和 `FwCrypto.GenerateNonce()`。

### 2. Transfer.cs（更新）

| 变更项 | 说明 |
|--------|------|
| **串口生命周期** | 新增 `Open()` / `Close()` 方法；`PacketRequest` 不再自行打开/关闭串口，避免多包操作（FW_WRITE 逐片发送）重复开关端口 |
| **新指令枚举** | `FwWrite = 0x84`、`FwCommit = 0x85`、`FwRollback = 0x86` |
| **新查询子码** | `InquirySubcode.FwStatus = 0x02`，查询 W25Q128 双区状态 |
| **ERRCODE 修正** | `Failed = 6`（原缺失），`Param = 0xFF`（原为 6 错误值） |
| **新数据类** | `FwStatus` 含 `ActiveZone / ZoneA/BState / ZoneA/BFwVersion / BootFailCount` |
| **新方法** | `GetFwStatus()` / `FwWrite()` / `FwCommit()` / `FwRollback()` |

### 3. Form1.cs / Form1.Designer.cs（更新）

**UI 新增控件：**

| 控件 | 用途 |
|------|------|
| `numericUpDownFwVersion` | 输入写入元数据的固件版本号（uint32） |
| `buttonQueryStatus` | 查询 W25Q128 双区分区状态 |
| `buttonRollback` | 发送回滚指令（设备切换到备份区并重启） |

**"升级"按钮新流程（加密 OTA → W25Q128）：**

```
1. 解析 .xbin 文件（Magic Header + App 二进制）
2. 通过 ERASE + PROGRAM + VERIFY 将 Magic Header 写入内部 Flash
   （让 Bootloader 能在重启后校验新 App）
3. AES-128-CTR 加密 App 二进制，生成随机 Nonce
4. FW_WRITE (0x84)：分片发送加密固件到 W25Q128
5. FW_COMMIT (0x85)：发送 nonce + fw_version + plaintext_crc32
   → 下位机解密验证 CRC → 烧录到内部 Flash → 系统复位
6. 等待设备重新上线
```

---

## 协议速查

### 请求帧格式
```
[0xAA][opcode(1)][length_le(2)][payload(N)][crc16(2)]
```

### 响应帧格式
```
[0x55][opcode(1)][errcode(1)][length_le(2)][payload(N)][crc16(2)]
```

### Opcode 一览

| Opcode | 名称       | 方向      | 说明                                    |
|--------|------------|-----------|----------------------------------------|
| 0x01   | INQUIRY    | PC→Device | 子码 0x00=版本, 0x01=MTU, 0x02=FW状态 |
| 0x81   | ERASE      | PC→Device | 擦除内部 Flash 指定区域                |
| 0x82   | PROGRAM    | PC→Device | 写入内部 Flash                         |
| 0x83   | VERIFY     | PC→Device | CRC32 校验内部 Flash                   |
| 0x84   | FW_WRITE   | PC→Device | 分片传输加密固件到 W25Q128             |
| 0x85   | FW_COMMIT  | PC→Device | 提交：解密验证→烧录→复位               |
| 0x86   | FW_ROLLBACK| PC→Device | 回滚到备份区并复位                     |
| 0x21   | RESET      | PC→Device | 系统复位                               |
| 0x22   | BOOT       | PC→Device | 跳转到 App                             |

### FW_WRITE Payload

```
[total_size(4B LE)][chunk_offset(4B LE)][encrypted_data(N B)]
```
- `chunk_offset == 0` 时触发目标区选择与擦除。
- 最大 `N = MTU - 8`（MTU 由 INQUIRY 0x01 获取，通常 4104）。

### FW_COMMIT Payload

```
[nonce(8B)][fw_version(4B LE)][crc32(4B LE)]
```
- `crc32` 为**明文** App 固件的 CRC32（与 Magic Header 中 `DataCrc32` 相同）。
- 设备发送 ACK 后约 2 ms 内会执行烧录并复位，上位机应等待重连。

### FW_STATUS Response Payload (12 B)

```
[active_zone(1)][zone_a_state(1)][zone_a_fw_ver(4 LE)]
[zone_b_state(1)][zone_b_fw_ver(4 LE)][boot_fail_count(1)]
```
- `zone_state`: `0xFF`=空, `0x01`=有效, `0x00`=无效。
- `active_zone`: `0`=Zone A, `1`=Zone B。

---

## 安全提示

> **⚠️ 默认 AES 密钥以明文存储在固件和上位机源码中，仅供开发调试。**  
> 生产固件请在 `fw_crypto.c` 的 `s_key[]` 和 `FwCrypto.cs` 的 `DefaultKey`  
> 中同步替换为安全存储的项目专用密钥（建议从 STM32 OTP 区读取或通过安全渠道烧录）。
