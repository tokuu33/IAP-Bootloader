using System.Diagnostics;
using System.IO.Ports;
using System.Text;
using System.Threading.Channels;
using System.Timers;
using Timer = System.Timers.Timer;

namespace SerialUpgrader;

// ─────────────────────────────────────────────────────────────────────────────
// W25Q128 partition status returned by INQUIRY subcode FW_STATUS (0x02).
// ─────────────────────────────────────────────────────────────────────────────

/// <summary>Zone state values matching <c>fw_zone_state_t</c> in fw_partition.h.</summary>
public enum ZoneState : byte
{
    Empty   = 0xFF,
    Valid   = 0x01,
    Invalid = 0x00,
}

/// <summary>W25Q128 dual-zone partition status returned by INQUIRY FW_STATUS.</summary>
public sealed class FwStatus
{
    /// <summary>Currently active boot zone: 0 = Zone A, 1 = Zone B.</summary>
    public byte ActiveZone     { get; init; }
    /// <summary>Zone A partition state.</summary>
    public ZoneState ZoneAState    { get; init; }
    /// <summary>Firmware version stored in Zone A.</summary>
    public uint ZoneAFwVersion { get; init; }
    /// <summary>Zone B partition state.</summary>
    public ZoneState ZoneBState    { get; init; }
    /// <summary>Firmware version stored in Zone B.</summary>
    public uint ZoneBFwVersion { get; init; }
    /// <summary>Consecutive boot-failure count (rollback triggers at ≥ 3).</summary>
    public byte BootFailCount  { get; init; }

    public override string ToString() =>
        $"激活区={ActiveZone} | " +
        $"ZoneA: {ZoneAState} v{ZoneAFwVersion} | " +
        $"ZoneB: {ZoneBState} v{ZoneBFwVersion} | " +
        $"启动失败计数={BootFailCount}";
}

// ─────────────────────────────────────────────────────────────────────────────
// Transfer — serial protocol implementation
// ─────────────────────────────────────────────────────────────────────────────

/// <summary>
/// Implements the IAP bootloader UART protocol.
///
/// Port lifecycle
/// ──────────────
/// Call <see cref="Open"/> before any operation and <see cref="Close"/>
/// (or <see cref="Dispose"/>) afterwards.  Keeping the port open across all
/// operations in a session (e.g. FW_WRITE with hundreds of chunks) avoids
/// per-packet reconnection overhead.
/// </summary>
public sealed class Transfer : IDisposable
{
    // ── Protocol constants ────────────────────────────────────────────────────
    private const byte PACKET_HEADER_REQUEST  = 0xAA;
    private const byte PACKET_HEADER_RESPONSE = 0x55;
    private const int  DEFAULT_BAUD_RATE      = 115200;

    // Packet field offsets (response frame)
    private const int PACKET_HEADER_OFFSET  = 0;
    private const int PACKET_OPCODE_OFFSET  = 1;
    private const int PACKET_ERRCODE_OFFSET = 2;
    private const int PACKET_LENGTH_OFFSET  = 3;
    private const int PACKET_PAYLOAD_OFFSET = 5;
    private const int PACKET_CRC_SIZE       = 2;
    private const int PACKET_MIN_SIZE       = 7; // header+opcode+errcode+length(2)+crc(2)

    // Operation timeouts
    private const int DEFAULT_TIMEOUT_SECONDS  = 3;
    private const int ERASE_TIMEOUT_SECONDS    = 10;
    private const int PROGRAM_TIMEOUT_SECONDS  = 10;
    private const int VERIFY_TIMEOUT_SECONDS   = 10;
    private const int FW_WRITE_TIMEOUT_SECONDS = 10;  // per chunk
    private const int FW_COMMIT_TIMEOUT_SECONDS= 60;  // decrypt + flash + respond

    // Payload length constants
    private const int ADDR_SIZE_PARAM_LEN    = 8;   // uint addr(4) + uint size(4)
    private const int ADDR_SIZE_CRC_PARAM_LEN= 12;  // uint addr(4) + uint size(4) + uint crc(4)
    private const int FW_WRITE_HEADER_LEN    = 8;   // total_size(4) + chunk_offset(4)
    private const int FW_COMMIT_PARAM_LEN    = 16;  // nonce(8) + fw_version(4) + crc32(4)
    private const int FW_STATUS_RESPONSE_LEN = 12;  // see INQUERY_SUBCODE_FW_STATUS handler

    // ── Enumerations ──────────────────────────────────────────────────────────

    enum Opcode : byte
    {
        Inquiry    = 0x01,
        Erase      = 0x81,
        Program    = 0x82,
        Verify     = 0x83,
        FwWrite    = 0x84,  // encrypted firmware chunk → W25Q128
        FwCommit   = 0x85,  // verify + flash + reset
        FwRollback = 0x86,  // manual rollback to backup zone
        Reset      = 0x21,
        Boot       = 0x22,
    }

    enum InquirySubcode : byte
    {
        Version  = 0x00,
        Mtu      = 0x01,
        FwStatus = 0x02,  // W25Q128 dual-zone status
    }

    enum Errcode : byte
    {
        Ok       = 0,
        Opcode   = 1,
        Overflow = 2,
        Timeout  = 3,
        Format   = 4,
        Verify   = 5,
        Failed   = 6,
        Param    = 0xFF,
        Unknown  = 0xFE,  // internal sentinel: no/invalid response received
    }

    enum ResponseState { Header, Opcode, Errcode, Length, Payload, Crc16 }

    sealed class Response(Opcode opcode, Errcode errcode, byte[]? param)
    {
        public readonly Opcode  Op      = opcode;
        public readonly Errcode ErrCode = errcode;
        public readonly byte[]? Param   = param;
    }

    // ── Fields ────────────────────────────────────────────────────────────────

    /// <summary>Invoked with a progress percentage [0–100] during multi-packet operations.</summary>
    public Action<double>? OnProgress;
    /// <summary>Invoked with human-readable status messages.</summary>
    public Action<string>? OnEvent;

    private readonly SerialPort Port;
    private Channel<byte>       DataChannel;
    private bool                disposed;

    // ── Construction ──────────────────────────────────────────────────────────

    /// <param name="portName">Serial port name, e.g. "COM3" or "/dev/ttyUSB0".</param>
    /// <param name="baudRate">Baud rate (default 115200).</param>
    public Transfer(string portName, int baudRate = DEFAULT_BAUD_RATE)
    {
        Port        = new SerialPort(portName, baudRate);
        DataChannel = Channel.CreateUnbounded<byte>();
        Port.DataReceived += OnDataReceived;
    }

    // ── Port lifecycle ────────────────────────────────────────────────────────

    /// <summary>Open the serial port.  Must be called before any operation.</summary>
    public bool Open()
    {
        try
        {
            DataChannel = Channel.CreateUnbounded<byte>(); // fresh channel
            Port.Open();
            return true;
        }
        catch (Exception ex)
        {
            OnEvent?.Invoke($"串口打开失败: {ex.Message}");
            return false;
        }
    }

    /// <summary>Close the serial port.</summary>
    public void Close()
    {
        try { if (Port.IsOpen) Port.Close(); }
        catch { /* swallow */ }
    }

    public void Dispose()
    {
        if (!disposed)
        {
            Close();
            Port.Dispose();
            disposed = true;
        }
    }

    // ── Internal helpers ──────────────────────────────────────────────────────

    void OnDataReceived(object sender, SerialDataReceivedEventArgs e)
    {
        if (!Port.IsOpen) return;
        byte[] buf = new byte[Port.BytesToRead];
        Port.Read(buf, 0, buf.Length);
        foreach (byte b in buf) DataChannel.Writer.TryWrite(b);
    }

    async Task<Response?> WaitResponse(Opcode opcode, TimeSpan timeout)
    {
        var response      = new List<byte>();
        var state         = ResponseState.Header;
        int payloadLength = 0;
        var errcode       = Errcode.Unknown;

        using var cts   = new CancellationTokenSource();
        using var timer = new Timer(timeout.TotalMilliseconds) { AutoReset = false };
        timer.Elapsed  += (_, _) => cts.Cancel();
        timer.Start();

        DateTime lastRx = DateTime.Now;
        try
        {
            await foreach (byte b in DataChannel.Reader.ReadAllAsync(cts.Token))
            {
                // Inter-byte timeout: reset state machine on long silence
                if (DateTime.Now - lastRx > TimeSpan.FromSeconds(2))
                {
                    response.Clear();
                    state = ResponseState.Header;
                }
                lastRx = DateTime.Now;

                Trace.WriteLine($"<- {b:X2}");
                response.Add(b);

                switch (state)
                {
                    case ResponseState.Header:
                        if (b == PACKET_HEADER_RESPONSE)
                            state = ResponseState.Opcode;
                        else
                            response.Clear();
                        break;

                    case ResponseState.Opcode:
                        if (b == (byte)opcode)
                            state = ResponseState.Errcode;
                        else
                        {
                            Trace.WriteLine($"opcode mismatch: got 0x{b:X2}, expected 0x{(byte)opcode:X2}");
                            response.Clear();
                            state = ResponseState.Header;
                        }
                        break;

                    case ResponseState.Errcode:
                        errcode = (Errcode)b;
                        state   = ResponseState.Length;
                        break;

                    case ResponseState.Length:
                        if (response.Count == PACKET_PAYLOAD_OFFSET)
                        {
                            payloadLength = BitConverter.ToUInt16(response.ToArray(), PACKET_LENGTH_OFFSET);
                            state = payloadLength > 0 ? ResponseState.Payload : ResponseState.Crc16;
                        }
                        break;

                    case ResponseState.Payload:
                        if (response.Count == PACKET_PAYLOAD_OFFSET + payloadLength)
                            state = ResponseState.Crc16;
                        break;

                    case ResponseState.Crc16:
                        if (response.Count == PACKET_PAYLOAD_OFFSET + payloadLength + PACKET_CRC_SIZE)
                        {
                            byte[] raw    = response.ToArray();
                            ushort rxCrc  = BitConverter.ToUInt16(raw, PACKET_PAYLOAD_OFFSET + payloadLength);
                            ushort calCrc = CRC.CRC16(raw[..(PACKET_PAYLOAD_OFFSET + payloadLength)]);
                            if (rxCrc == calCrc)
                            {
                                byte[]? param = payloadLength > 0
                                    ? raw[PACKET_PAYLOAD_OFFSET..(PACKET_PAYLOAD_OFFSET + payloadLength)]
                                    : null;
                                Trace.WriteLine($"packet ok: opcode=0x{(byte)opcode:X2} errcode={errcode} len={payloadLength}");
                                return new Response(opcode, errcode, param);
                            }
                            Trace.WriteLine($"CRC16 mismatch: rx={rxCrc:X4} calc={calCrc:X4}");
                            return null;
                        }
                        break;
                }
            }
        }
        catch (OperationCanceledException)
        {
            OnEvent?.Invoke($"等待 0x{(byte)opcode:X2} 响应超时");
        }
        catch (Exception ex)
        {
            OnEvent?.Invoke($"等待响应异常: {ex.Message}");
        }
        return null;
    }

    byte[] BuildPacket(Opcode opcode, byte[]? payload)
    {
        var pkt = new List<byte>
        {
            PACKET_HEADER_REQUEST,
            (byte)opcode,
        };
        pkt.AddRange(BitConverter.GetBytes((ushort)(payload?.Length ?? 0)));
        if (payload != null) pkt.AddRange(payload);
        pkt.AddRange(BitConverter.GetBytes(CRC.CRC16([.. pkt])));
        return [.. pkt];
    }

    async Task<Response?> PacketRequest(Opcode opcode, byte[]? payload, TimeSpan timeout)
    {
        if (!Port.IsOpen)
        {
            OnEvent?.Invoke("串口未打开");
            return null;
        }
        try
        {
            byte[] pkt = BuildPacket(opcode, payload);
            Port.Write(pkt, 0, pkt.Length);
            return await WaitResponse(opcode, timeout);
        }
        catch (Exception ex)
        {
            OnEvent?.Invoke($"发送异常: {ex.Message}");
            return null;
        }
    }

    // ── Public API ────────────────────────────────────────────────────────────

    /// <summary>Query the bootloader version string.</summary>
    public async Task<string?> GetBootloaderVersion()
    {
        var r = await PacketRequest(Opcode.Inquiry, [(byte)InquirySubcode.Version],
            TimeSpan.FromSeconds(DEFAULT_TIMEOUT_SECONDS));
        return r?.ErrCode == Errcode.Ok && r.Param != null
            ? Encoding.ASCII.GetString(r.Param) : null;
    }

    /// <summary>Query the maximum payload size (MTU) in bytes.</summary>
    public async Task<int?> GetBootloaderMtu()
    {
        var r = await PacketRequest(Opcode.Inquiry, [(byte)InquirySubcode.Mtu],
            TimeSpan.FromSeconds(DEFAULT_TIMEOUT_SECONDS));
        return r?.ErrCode == Errcode.Ok && r.Param != null
            ? (int?)BitConverter.ToUInt16(r.Param) : null;
    }

    /// <summary>
    /// Query the W25Q128 dual-zone partition status (INQUIRY subcode 0x02).
    /// Response payload: active_zone(1) + zone_a_state(1) + zone_a_fw_ver(4)
    ///                 + zone_b_state(1) + zone_b_fw_ver(4) + boot_fail_count(1)
    /// </summary>
    public async Task<FwStatus?> GetFwStatus()
    {
        var r = await PacketRequest(Opcode.Inquiry, [(byte)InquirySubcode.FwStatus],
            TimeSpan.FromSeconds(DEFAULT_TIMEOUT_SECONDS));
        if (r?.ErrCode != Errcode.Ok || r.Param == null || r.Param.Length < FW_STATUS_RESPONSE_LEN)
            return null;

        int idx = 0;
        byte   activeZone    = r.Param[idx++];
        byte   zoneAState    = r.Param[idx++];
        uint   zoneAVer      = BitConverter.ToUInt32(r.Param, idx); idx += 4;
        byte   zoneBState    = r.Param[idx++];
        uint   zoneBVer      = BitConverter.ToUInt32(r.Param, idx); idx += 4;
        byte   bootFailCount = r.Param[idx];

        return new FwStatus
        {
            ActiveZone     = activeZone,
            ZoneAState     = (ZoneState)zoneAState,
            ZoneAFwVersion = zoneAVer,
            ZoneBState     = (ZoneState)zoneBState,
            ZoneBFwVersion = zoneBVer,
            BootFailCount  = bootFailCount,
        };
    }

    /// <summary>Erase a region of internal Flash.</summary>
    public async Task<bool> Erase(uint addr, uint size)
    {
        byte[] p = new byte[ADDR_SIZE_PARAM_LEN];
        BitConverter.GetBytes(addr).CopyTo(p, 0);
        BitConverter.GetBytes(size).CopyTo(p, 4);
        var r = await PacketRequest(Opcode.Erase, p, TimeSpan.FromSeconds(ERASE_TIMEOUT_SECONDS));
        return r?.ErrCode == Errcode.Ok;
    }

    /// <summary>
    /// Write <paramref name="data"/> to internal Flash starting at <paramref name="addr"/>,
    /// splitting into chunks no larger than <c>mtu - 8</c> bytes.
    /// </summary>
    public async Task<bool> Program(uint addr, byte[] data, int mtu)
    {
        int  chunkMax = mtu - 8;
        uint total    = (uint)data.Length;
        int  srcOff   = 0;

        while (srcOff < data.Length)
        {
            int chunk = Math.Min(data.Length - srcOff, chunkMax);
            OnProgress?.Invoke((double)srcOff / total * 100.0);
            OnEvent?.Invoke($"Program 0x{addr + srcOff:X8}  {srcOff}/{total} bytes ({srcOff * 100.0 / total:F1}%)");

            byte[] p = new byte[8 + chunk];
            BitConverter.GetBytes(addr + (uint)srcOff).CopyTo(p, 0);
            BitConverter.GetBytes((uint)chunk).CopyTo(p, 4);
            Array.Copy(data, srcOff, p, 8, chunk);

            var r = await PacketRequest(Opcode.Program, p, TimeSpan.FromSeconds(PROGRAM_TIMEOUT_SECONDS));
            if (r?.ErrCode != Errcode.Ok) return false;
            srcOff += chunk;
        }
        return true;
    }

    /// <summary>Verify a region of internal Flash against a CRC32.</summary>
    public async Task<bool> Verify(uint addr, uint size, uint crc32)
    {
        byte[] p = new byte[ADDR_SIZE_CRC_PARAM_LEN];
        BitConverter.GetBytes(addr).CopyTo(p, 0);
        BitConverter.GetBytes(size).CopyTo(p, 4);
        BitConverter.GetBytes(crc32).CopyTo(p, 8);
        var r = await PacketRequest(Opcode.Verify, p, TimeSpan.FromSeconds(VERIFY_TIMEOUT_SECONDS));
        return r?.ErrCode == Errcode.Ok;
    }

    /// <summary>
    /// Stream encrypted firmware to W25Q128 in chunks (opcode 0x84 FW_WRITE).
    ///
    /// Packet payload: [total_size (4 B)] [chunk_offset (4 B)] [encrypted_data (N B)]
    /// The first packet (offset == 0) triggers zone selection and erasure on the device.
    /// </summary>
    /// <param name="encryptedData">AES-128-CTR encrypted firmware binary.</param>
    /// <param name="mtu">Max payload size from <see cref="GetBootloaderMtu"/>.</param>
    public async Task<bool> FwWrite(byte[] encryptedData, int mtu)
    {
        int  chunkMax = mtu - FW_WRITE_HEADER_LEN;
        uint total    = (uint)encryptedData.Length;
        uint offset   = 0;

        while (offset < total)
        {
            uint chunk = (uint)Math.Min(total - offset, chunkMax);
            OnProgress?.Invoke((double)offset / total * 100.0);
            OnEvent?.Invoke($"FW_Write offset=0x{offset:X8}  {offset}/{total} ({offset * 100.0 / total:F1}%)");

            byte[] p = new byte[FW_WRITE_HEADER_LEN + chunk];
            BitConverter.GetBytes(total).CopyTo(p, 0);          // total_size
            BitConverter.GetBytes(offset).CopyTo(p, 4);         // chunk_offset
            Array.Copy(encryptedData, (int)offset, p, FW_WRITE_HEADER_LEN, (int)chunk);

            var r = await PacketRequest(Opcode.FwWrite, p, TimeSpan.FromSeconds(FW_WRITE_TIMEOUT_SECONDS));
            if (r?.ErrCode != Errcode.Ok)
            {
                OnEvent?.Invoke($"FW_Write 失败 offset=0x{offset:X8}: errcode={r?.ErrCode}");
                return false;
            }
            offset += chunk;
        }
        OnProgress?.Invoke(100.0);
        return true;
    }

    /// <summary>
    /// Commit the firmware upload (opcode 0x85 FW_COMMIT).
    ///
    /// Payload: [nonce (8 B)] [fw_version (4 B LE)] [crc32 (4 B LE)] = 16 bytes.
    ///
    /// The device will:
    ///   1. Decrypt the W25Q128 data and verify CRC32.
    ///   2. If OK: reply ACK, then flash to internal Flash and reset.
    ///   3. If CRC fails: reply ERR_VERIFY (no reset).
    ///
    /// On success the caller should wait for the device to reconnect.
    /// </summary>
    /// <param name="nonce">The 8-byte nonce used during AES-128-CTR encryption.</param>
    /// <param name="fwVersion">Firmware version number stored in metadata.</param>
    /// <param name="crc32">CRC32 of the <em>plaintext</em> firmware binary.</param>
    public async Task<bool> FwCommit(byte[] nonce, uint fwVersion, uint crc32)
    {
        if (nonce.Length != 8)
            throw new ArgumentException("Nonce must be exactly 8 bytes.", nameof(nonce));

        byte[] p = new byte[FW_COMMIT_PARAM_LEN];
        Array.Copy(nonce, 0, p, 0, 8);
        BitConverter.GetBytes(fwVersion).CopyTo(p, 8);
        BitConverter.GetBytes(crc32).CopyTo(p, 12);

        var r = await PacketRequest(Opcode.FwCommit, p, TimeSpan.FromSeconds(FW_COMMIT_TIMEOUT_SECONDS));
        if (r == null) return false;
        if (r.ErrCode == Errcode.Verify)
        {
            OnEvent?.Invoke("FW_Commit: 固件CRC32校验失败（固件损坏或密钥不匹配）");
            return false;
        }
        return r.ErrCode == Errcode.Ok;
    }

    /// <summary>
    /// Request manual rollback to the backup zone (opcode 0x86 FW_ROLLBACK).
    /// The device will reply ACK and then reset into the backup firmware.
    /// </summary>
    public async Task<bool> FwRollback()
    {
        var r = await PacketRequest(Opcode.FwRollback, null,
            TimeSpan.FromSeconds(DEFAULT_TIMEOUT_SECONDS));
        if (r?.ErrCode == Errcode.Failed)
        {
            OnEvent?.Invoke("FW_Rollback: 备份区无有效固件，无法回滚");
            return false;
        }
        return r?.ErrCode == Errcode.Ok;
    }

    /// <summary>Reset the device (opcode 0x21).</summary>
    public async Task<bool> Reset()
    {
        var r = await PacketRequest(Opcode.Reset, null, TimeSpan.FromSeconds(5));
        return r?.ErrCode == Errcode.Ok;
    }

    /// <summary>Boot the application (opcode 0x22).</summary>
    public async Task<bool> Boot()
    {
        var r = await PacketRequest(Opcode.Boot, null, TimeSpan.FromSeconds(5));
        return r?.ErrCode == Errcode.Ok;
    }
}
