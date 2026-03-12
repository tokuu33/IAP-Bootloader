using System;
using System.Diagnostics;
using System.Text;

namespace SerialUpgrader;

/// <summary>
/// Parses the 256-byte <c>magic_header_t</c> from a .xbin firmware package.
///
/// .xbin file layout
/// ─────────────────
///   [0   .. 255] : 256-byte magic header  → programmed to ThisAddress in internal Flash
///   [256 .. end] : raw app binary          → encrypted and sent to W25Q128 via FW_WRITE
///
/// magic_header_t layout (256 bytes, little-endian)
/// ─────────────────────────────────────────────────
///   Offset  Size  Field
///   ------  ----  -----
///    0        4   Magic           = 0x4D414749 ('MAGI')
///    4        4   UpdateFlag      — non-zero when new firmware is pending in W25Q128
///    8        4   RollbackFlag    — non-zero when app requests rollback
///   12        4   BootFailCount   — consecutive failed-boot counter
///   16        4   DataType        — 0 = Application
///   20        4   DataOffset      — offset of app binary within .xbin (always 256)
///   24        4   DataAddress     — Flash address where app binary is programmed
///   28        4   DataLength      — app binary size in bytes
///   32        4   DataCrc32       — CRC32 of the plaintext app binary
///   36        4   NewAppLength    — new firmware size in W25Q128 (filled by bootloader)
///   40        4   NewAppCrc32     — new firmware CRC32 in W25Q128 (filled by bootloader)
///   44        4   BackupLength    — backup zone firmware size
///   48        4   BackupCrc32     — backup zone firmware CRC32
///   52      128   Version         — ASCII version string, zero-padded
///  180       68   Reserved3       — 17 × uint32, reserved
///  248        4   ThisAddress     — Flash address of this header struct
///  252        4   ThisCrc32       — CRC32 of header[0:252]
/// </summary>
public sealed class MagicHeader
{
    private const uint  MAGIC_VALUE     = 0x4D414749u; // 'MAGI'
    private const int   HEADER_SIZE     = 256;
    private const int   CRC_COVERS_UPTO = 252;         // bytes [0..251] covered by ThisCrc32

    // ── Parsed fields ─────────────────────────────────────────────────────────

    /// <summary>Non-zero when a new firmware image is pending in W25Q128.</summary>
    public uint UpdateFlag     { get; private set; }
    /// <summary>Non-zero when the application has requested a rollback.</summary>
    public uint RollbackFlag   { get; private set; }
    /// <summary>Consecutive boot-fail counter (incremented by the bootloader).</summary>
    public uint BootFailCount  { get; private set; }

    /// <summary>Data type: 0 = Application.</summary>
    public uint DataType       { get; private set; }
    /// <summary>Byte offset of the app binary within the .xbin file (always 256).</summary>
    public uint DataOffset     { get; private set; }
    /// <summary>Flash address where the app binary is programmed (e.g. 0x08010000).</summary>
    public uint DataAddress    { get; private set; }
    /// <summary>App binary size in bytes.</summary>
    public uint DataLength     { get; private set; }
    /// <summary>CRC32 of the plaintext app binary.</summary>
    public uint DataCrc32      { get; private set; }

    /// <summary>Size of the new firmware image stored in W25Q128 (filled by bootloader).</summary>
    public uint NewAppLength   { get; private set; }
    /// <summary>CRC32 of the new firmware image in W25Q128 (filled by bootloader).</summary>
    public uint NewAppCrc32    { get; private set; }
    /// <summary>Size of the firmware in the backup zone (filled by bootloader).</summary>
    public uint BackupLength   { get; private set; }
    /// <summary>CRC32 of the firmware in the backup zone (filled by bootloader).</summary>
    public uint BackupCrc32    { get; private set; }

    /// <summary>Firmware version string from the magic header.</summary>
    public string Version      { get; private set; } = string.Empty;
    /// <summary>Flash address at which this header struct is stored.</summary>
    public uint ThisAddress    { get; private set; }
    /// <summary>CRC32 of header[0:252] stored inside the header.</summary>
    public uint ThisCrc32      { get; private set; }

    // ── Private constructor — use Parse() ────────────────────────────────────

    private MagicHeader() { }

    // ── Factory ───────────────────────────────────────────────────────────────

    /// <summary>
    /// Parse the magic header at the beginning of a .xbin byte array.
    /// </summary>
    /// <param name="xbin">Full .xbin file contents (≥ 256 bytes).</param>
    /// <returns>A populated <see cref="MagicHeader"/>, or <c>null</c> on failure.</returns>
    public static MagicHeader? Parse(byte[] xbin)
    {
        if (xbin.Length < HEADER_SIZE)
        {
            Trace.WriteLine($"MagicHeader: file too short ({xbin.Length} < {HEADER_SIZE})");
            return null;
        }

        // ── Magic check ───────────────────────────────────────────────────────
        uint magic = BitConverter.ToUInt32(xbin, 0);
        if (magic != MAGIC_VALUE)
        {
            Trace.WriteLine($"MagicHeader: bad magic 0x{magic:X8} (expected 0x{MAGIC_VALUE:X8})");
            return null;
        }

        // ── CRC32 check ───────────────────────────────────────────────────────
        // this_crc32 covers header[0..251].
        uint storedCrc     = BitConverter.ToUInt32(xbin, 252);
        uint calculatedCrc = CRC.CRC32(xbin[..CRC_COVERS_UPTO]);
        if (calculatedCrc != storedCrc)
        {
            Trace.WriteLine($"MagicHeader: CRC32 mismatch — stored 0x{storedCrc:X8}, calculated 0x{calculatedCrc:X8}");
            return null;
        }

        // ── DataOffset sanity check ───────────────────────────────────────────
        // DataOffset must be exactly HEADER_SIZE (256). A value of 0 or anything
        // other than 256 indicates an old-format or corrupt .xbin file that was
        // not generated by the current gen_xbin.py. Regenerate the .xbin using
        //   python tools/gen_xbin.py app.bin
        uint rawDataOffset = BitConverter.ToUInt32(xbin, 20);
        if (rawDataOffset != HEADER_SIZE)
        {
            Trace.WriteLine(
                $"MagicHeader: invalid DataOffset {rawDataOffset} " +
                $"(expected {HEADER_SIZE}). " +
                "The .xbin may have been generated by an older packaging tool. " +
                "Please regenerate with gen_xbin.py.");
            return null;
        }

        // ── Field extraction ──────────────────────────────────────────────────
        var h = new MagicHeader
        {
            // Offset  4 : update_flag
            UpdateFlag    = BitConverter.ToUInt32(xbin,  4),
            // Offset  8 : rollback_flag
            RollbackFlag  = BitConverter.ToUInt32(xbin,  8),
            // Offset 12 : boot_fail_count
            BootFailCount = BitConverter.ToUInt32(xbin, 12),
            // Offset 16 : data_type
            DataType      = BitConverter.ToUInt32(xbin, 16),
            // Offset 20 : data_offset
            DataOffset    = BitConverter.ToUInt32(xbin, 20),
            // Offset 24 : data_address
            DataAddress   = BitConverter.ToUInt32(xbin, 24),
            // Offset 28 : data_length
            DataLength    = BitConverter.ToUInt32(xbin, 28),
            // Offset 32 : data_crc32
            DataCrc32     = BitConverter.ToUInt32(xbin, 32),
            // Offset 36 : new_app_length
            NewAppLength  = BitConverter.ToUInt32(xbin, 36),
            // Offset 40 : new_app_crc32
            NewAppCrc32   = BitConverter.ToUInt32(xbin, 40),
            // Offset 44 : backup_length
            BackupLength  = BitConverter.ToUInt32(xbin, 44),
            // Offset 48 : backup_crc32
            BackupCrc32   = BitConverter.ToUInt32(xbin, 48),
            // Offset 52 : version[128]
            Version       = Encoding.ASCII.GetString(xbin, 52, 128).TrimEnd('\0'),
            // Offset 248: this_address
            ThisAddress   = BitConverter.ToUInt32(xbin, 248),
            // Offset 252: this_crc32 (already checked above)
            ThisCrc32     = storedCrc,
        };

        return h;
    }
}
