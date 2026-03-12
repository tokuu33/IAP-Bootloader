# 上位机适配说明 (Upper-PC Adaptation)

本目录包含与新版 Bootloader 协议对齐后的上位机 C# 源文件，  
用于替换 [yhy666610/iap_upperpc](https://github.com/yhy666610/IAP_upperpc) 仓库中的同名文件。

---

## 固件打包工具 — `tools/gen_xbin.py`

在编译得到 `.bin` 后，使用此脚本生成供上位机使用的 `.xbin` 打包文件。

```bash
# 基本用法（输出同目录下 app.xbin）
python3 tools/gen_xbin.py app.bin --version v2.0.0

# 完整参数
python3 tools/gen_xbin.py app.bin \
    --version v2.0.0 \
    --app-address    0x08010000 \   # 默认值，App 在内部 Flash 的烧录地址
    --header-address 0x0800C000 \   # 默认值，Magic Header 在内部 Flash 的地址
    -o output/firmware_v2.0.0.xbin
```

### .xbin 文件布局

```
[0   .. 255] : 256 字节 magic_header_t  → 上位机通过 ERASE+PROGRAM+VERIFY 写入 Flash
[256 .. end] : App 二进制              → 上位机 AES-128-CTR 加密后通过 FW_WRITE+FW_COMMIT 写入
```

### magic_header_t 字段布局（256 字节，小端序）

| 偏移 | 大小 | 字段             | 打包时初始值          | 说明                                          |
|------|------|------------------|-----------------------|-----------------------------------------------|
|   0  |  4   | magic            | `0x4D414749`          | 魔数 'MAGI'                                   |
|   4  |  4   | update_flag      | `0`                   | 非零：W25Q128 有待应用的新固件                |
|   8  |  4   | rollback_flag    | `0`                   | 非零：App 请求 Bootloader 执行回滚            |
|  12  |  4   | boot_fail_count  | `0`                   | Bootloader 记录的连续启动失败次数             |
|  16  |  4   | data_type        | `0` (App)             | 固件类型                                      |
|  20  |  4   | data_offset      | `256`                 | App 二进制在 .xbin 文件中的偏移               |
|  24  |  4   | data_address     | `0x08010000`          | App 在内部 Flash 中的烧录地址                 |
|  28  |  4   | data_length      | len(app)              | App 二进制字节数                              |
|  32  |  4   | data_crc32       | CRC32(app)            | App 明文 CRC32（用于 Bootloader 校验）        |
|  36  |  4   | new_app_length   | `0`                   | W25Q128 中新固件大小（Bootloader 写入）       |
|  40  |  4   | new_app_crc32    | `0`                   | W25Q128 中新固件 CRC32（Bootloader 写入）     |
|  44  |  4   | backup_length    | `0`                   | 备份区固件大小（Bootloader 写入）             |
|  48  |  4   | backup_crc32     | `0`                   | 备份区固件 CRC32（Bootloader 写入）           |
|  52  | 128  | version          | ASCII 版本字符串      | 最长 127 字符 + NUL                           |
| 180  |  68  | reserved3        | 全零                  | 17 × uint32，保留                             |
| 248  |  4   | this_address     | `0x0800C000`          | 本结构体在 Flash 中的地址                     |
| 252  |  4   | this_crc32       | CRC32(header[0:252])  | 头部校验值（不含此字段本身）                  |

---

## 上位机 C# 替换文件

将以下文件直接复制到上位机项目根目录，覆盖原文件即可：

| 本目录文件               | 替换目标（iap_upperpc 仓库）    |
|--------------------------|-------------------------------|
| `FwCrypto.cs`            | *(新增，直接添加到项目)*        |
| `MagicHeader.cs`         | `MagicHeader.cs`              |
| `Transfer.cs`            | `Transfer.cs`                 |
| `Form1.cs`               | `Form1.cs`                    |
| `Form1.Designer.cs`      | `Form1.Designer.cs`           |

> `CRC.cs`、`Program.cs`、`SerialUpgrader.csproj`  
> **无需修改**，保持原版本即可。

---

## 三端联动完整 OTA 流程

```
①打包（PC）
  python3 tools/gen_xbin.py app.bin --version v2.0.0
      ↓ 生成 app.xbin（Magic Header 256B + App 二进制）

②上位机上传（PC → 设备 Bootloader）
  a. 解析 app.xbin → MagicHeader（含 DataOffset, DataAddress, DataCrc32）
  b. ERASE + PROGRAM + VERIFY 将 Magic Header（256B）写入 Flash @ ThisAddress
  c. AES-128-CTR 加密 App 二进制，生成随机 Nonce
  d. FW_WRITE (0x84)：分片发送加密固件 → W25Q128
  e. FW_COMMIT (0x85)：nonce + fw_version + DataCrc32
      → Bootloader 解密 W25Q128 数据并验证 CRC32
      → 验证通过：ACK → 解密烧录到内部 Flash (0x08010000) → 系统复位

③启动校验（Bootloader）
  a. 读取 Flash @ 0x0800C000 → 校验 magic_header_t (CRC32 of header[0:252])
  b. 读取 Flash @ 0x08010000 → 计算 CRC32 并与 data_crc32 比对
  c. 校验通过 → 跳转运行 App
  d. 校验失败 → 尝试从 W25Q128 备份区恢复（fw_manager_flash_firmware）
```

---

## 主要改动说明

### FwCrypto.cs（新增）

实现 **AES-128-CTR** 固件加密，与下位机 `fw_crypto.c` 算法完全一致：

- 计数器块格式：`[nonce(8B)][counter_be(4B)][zeros(4B)]`  
- 使用与下位机相同的默认密钥（**生产环境必须替换**）。  
- 提供 `FwCrypto.Xcrypt(data, nonce)` 和 `FwCrypto.GenerateNonce()`。

### MagicHeader.cs（更新）

解析新版 256 字节 `magic_header_t`（字段偏移与旧版不兼容）：
- 新增字段：`UpdateFlag / RollbackFlag / BootFailCount / NewAppLength / NewAppCrc32 / BackupLength / BackupCrc32`
- `DataType` 现在位于偏移 16（旧版 32），`Version` 现在位于偏移 52（旧版 96）。

### Transfer.cs（更新）

| 变更项 | 说明 |
|--------|------|
| **串口生命周期** | 新增 `Open()` / `Close()` 方法；避免多包操作重复开关端口 |
| **新指令枚举** | `FwWrite = 0x84`、`FwCommit = 0x85`、`FwRollback = 0x86` |
| **新查询子码** | `InquirySubcode.FwStatus = 0x02`，查询 W25Q128 双区状态 |
| **ERRCODE 修正** | `Failed = 6`（原缺失），`Param = 0xFF` |
| **新方法** | `GetFwStatus()` / `FwWrite()` / `FwCommit()` / `FwRollback()` |

### Form1.cs / Form1.Designer.cs（更新）

新增控件：`numericUpDownFwVersion`（固件版本号）、`buttonQueryStatus`、`buttonRollback`。

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

| Opcode | 名称        | 说明                                    |
|--------|-------------|----------------------------------------|
| 0x01   | INQUIRY     | 子码 0x00=版本, 0x01=MTU, 0x02=FW状态 |
| 0x81   | ERASE       | 擦除内部 Flash 指定区域                |
| 0x82   | PROGRAM     | 写入内部 Flash                         |
| 0x83   | VERIFY      | CRC32 校验内部 Flash                   |
| 0x84   | FW_WRITE    | 分片传输加密固件到 W25Q128             |
| 0x85   | FW_COMMIT   | 提交：解密验证→烧录→复位               |
| 0x86   | FW_ROLLBACK | 回滚到备份区并复位                     |
| 0x21   | RESET       | 系统复位                               |
| 0x22   | BOOT        | 跳转到 App                             |

### FW_COMMIT Payload（16 字节）

```
[nonce(8B)][fw_version(4B LE)][crc32(4B LE)]
```
- `crc32` = 明文 App 的 CRC32 = `magic_header_t.data_crc32`。

### FW_STATUS Response Payload（12 字节）

```
[active_zone(1)][zone_a_state(1)][zone_a_fw_ver(4 LE)]
[zone_b_state(1)][zone_b_fw_ver(4 LE)][boot_fail_count(1)]
```

---

## 安全提示

> **⚠️ 默认 AES 密钥以明文存储在固件和上位机源码中，仅供开发调试。**  
> 生产固件请在 `fw_crypto.c` 的 `s_key[]` 和 `FwCrypto.cs` 的 `DefaultKey`  
> 中同步替换为安全存储的项目专用密钥。

