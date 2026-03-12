using System.IO.Ports;

namespace SerialUpgrader
{
    public partial class Form1 : Form
    {
        public Form1()
        {
            InitializeComponent();
        }

        // ── Form events ───────────────────────────────────────────────────────

        private void Form1_Load(object sender, EventArgs e)
        {
            comboBoxSerialPort.Items.Clear();
            string[] ports = SerialPort.GetPortNames();
            if (ports.Length > 0)
            {
                comboBoxSerialPort.Items.AddRange(ports);
                comboBoxSerialPort.SelectedIndex = 0;
            }
        }

        // ── UI helpers ────────────────────────────────────────────────────────

        private void Log(string msg)
        {
            if (textBoxLog.IsDisposed) return;
            // May be called from non-UI thread
            if (InvokeRequired)
                BeginInvoke(() => AppendLog(msg));
            else
                AppendLog(msg);
        }

        private void AppendLog(string msg)
        {
            textBoxLog.AppendText(msg + "\r\n");
            textBoxLog.SelectionStart = textBoxLog.Text.Length;
            textBoxLog.ScrollToCaret();
        }

        private void SetProgress(double pct)
        {
            if (progressBar.IsDisposed) return;
            int v = Math.Clamp((int)pct, 0, 100);
            if (InvokeRequired) BeginInvoke(() => progressBar.Value = v);
            else progressBar.Value = v;
        }

        private void SetButtonsEnabled(bool enabled)
        {
            buttonUpgrade.Enabled    = enabled;
            buttonQueryStatus.Enabled = enabled;
            buttonRollback.Enabled    = enabled;
        }

        private string? GetSelectedPort()
        {
            string? port = comboBoxSerialPort.SelectedItem as string;
            if (string.IsNullOrEmpty(port))
                MessageBox.Show("请先选择串口。", "提示", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return port;
        }

        // ── Firmware file selection ───────────────────────────────────────────

        private void buttonSelectFirmware_Click(object sender, EventArgs e)
        {
            using var ofd = new OpenFileDialog
            {
                Title  = "选择固件文件",
                Filter = "固件文件 (*.xbin)|*.xbin|所有文件 (*.*)|*.*"
            };
            if (ofd.ShowDialog() == DialogResult.OK)
                textBoxFirmware.Text = ofd.FileName;
        }

        // ── Query W25Q128 status ──────────────────────────────────────────────

        private async void buttonQueryStatus_Click(object sender, EventArgs e)
        {
            string? port = GetSelectedPort();
            if (port == null) return;

            SetButtonsEnabled(false);
            textBoxLog.Clear();
            Log("正在查询 W25Q128 分区状态……");

            using var transfer = new Transfer(port);
            transfer.OnEvent += Log;

            if (!transfer.Open())
            {
                Log("无法打开串口。");
                SetButtonsEnabled(true);
                return;
            }

            try
            {
                FwStatus? status = await transfer.GetFwStatus();
                if (status == null)
                    Log("查询失败（超时或设备未响应）。");
                else
                {
                    Log($"激活区      : Zone {(status.ActiveZone == 0 ? 'A' : 'B')}");
                    Log($"Zone A 状态  : {status.ZoneAState}  版本={status.ZoneAFwVersion}");
                    Log($"Zone B 状态  : {status.ZoneBState}  版本={status.ZoneBFwVersion}");
                    Log($"启动失败计数 : {status.BootFailCount}");
                }
            }
            finally
            {
                transfer.Close();
                SetButtonsEnabled(true);
            }
        }

        // ── Rollback ──────────────────────────────────────────────────────────

        private async void buttonRollback_Click(object sender, EventArgs e)
        {
            string? port = GetSelectedPort();
            if (port == null) return;

            if (MessageBox.Show("确认回滚到备份区固件？设备将重启。",
                "确认回滚", MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes)
                return;

            SetButtonsEnabled(false);
            textBoxLog.Clear();
            Log("正在发送回滚指令……");

            using var transfer = new Transfer(port);
            transfer.OnEvent += Log;

            if (!transfer.Open())
            {
                Log("无法打开串口。");
                SetButtonsEnabled(true);
                return;
            }

            try
            {
                bool ok = await transfer.FwRollback();
                Log(ok
                    ? "回滚指令已发送，设备将重启并从备份区启动。"
                    : "回滚失败（备份区无有效固件或通信错误）。");
            }
            finally
            {
                transfer.Close();
                SetButtonsEnabled(true);
            }
        }

        // ── Upgrade (encrypted OTA via W25Q128) ──────────────────────────────

        /// <summary>
        /// Full OTA upgrade flow:
        ///   1. Parse .xbin magic header.
        ///   2. Write magic header section to internal Flash via ERASE+PROGRAM+VERIFY
        ///      (so the bootloader can validate the new app after reboot).
        ///   3. Encrypt the app binary with AES-128-CTR.
        ///   4. Stream encrypted app to W25Q128 via FW_WRITE (0x84) chunks.
        ///   5. Send FW_COMMIT (0x85) with nonce + fw_version + plaintext CRC32.
        ///   6. Device verifies CRC32, flashes to internal Flash, and resets.
        /// </summary>
        private async void buttonUpgrade_Click(object sender, EventArgs e)
        {
            // ── 1. Validate inputs ────────────────────────────────────────────
            string firmwarePath = textBoxFirmware.Text;
            if (string.IsNullOrEmpty(firmwarePath) || !File.Exists(firmwarePath))
            {
                MessageBox.Show("请选择有效的固件文件（.xbin）。",
                    "提示", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }

            string? port = GetSelectedPort();
            if (port == null) return;

            uint fwVersion = (uint)numericUpDownFwVersion.Value;

            // ── 2. Load and parse firmware file ──────────────────────────────
            textBoxLog.Clear();
            SetProgress(0);
            SetButtonsEnabled(false);

            byte[] xbin = File.ReadAllBytes(firmwarePath);
            Log($"固件文件: {Path.GetFileName(firmwarePath)}  ({xbin.Length / 1024.0:F2} KB)");

            MagicHeader? header = MagicHeader.Parse(xbin);
            if (header == null)
            {
                Log("错误：无效的固件文件（Magic Header 校验失败）。");
                SetButtonsEnabled(true);
                return;
            }

            // The .xbin layout:
            //   [0 .. DataOffset-1] = header section  (contains the magic header structure)
            //   [DataOffset .. end] = app binary       (the firmware written to internal Flash)
            byte[] headerSection = xbin[..(int)header.DataOffset];
            byte[] appBin        = xbin[(int)header.DataOffset..];

            if (appBin.Length != (int)header.DataLength)
            {
                Log($"错误：固件数据长度不匹配：文件中 {appBin.Length} B，头部声明 {header.DataLength} B。");
                SetButtonsEnabled(true);
                return;
            }

            Log($"头部地址 : 0x{header.ThisAddress:X8}");
            Log($"固件版本 : {header.Version}");
            Log($"固件地址 : 0x{header.DataAddress:X8}");
            Log($"固件大小 : {header.DataLength} B ({header.DataLength / 1024.0:F2} KB)");
            Log($"固件CRC32: 0x{header.DataCrc32:X8}");
            Log($"元数据版本号 (FW_COMMIT): {fwVersion}");

            // ── 3. Connect ───────────────────────────────────────────────────
            using var transfer = new Transfer(port);
            transfer.OnEvent    += Log;
            transfer.OnProgress += SetProgress;

            if (!transfer.Open())
            {
                Log("错误：无法打开串口。");
                SetButtonsEnabled(true);
                return;
            }

            try
            {
                // ── 4. Query bootloader version and MTU ───────────────────────
                string? blVersion = await transfer.GetBootloaderVersion();
                if (blVersion == null)
                {
                    Log("错误：无法获取 Bootloader 版本（超时或设备未进入 Bootloader 模式）。");
                    return;
                }
                Log($"Bootloader 版本: {blVersion}");

                int? mtu = await transfer.GetBootloaderMtu();
                if (mtu == null)
                {
                    Log("错误：无法获取 MTU。");
                    return;
                }
                Log($"MTU: {mtu} bytes");

                // ── 5. Write magic-header section to internal Flash ───────────
                // This embeds the metadata (app address, size, CRC32) so the
                // bootloader can validate the new app after it has been flashed
                // from W25Q128.
                Log("─── 写入 Magic Header 到内部 Flash ───");

                uint hdrAddr = header.ThisAddress;
                uint hdrSize = header.DataOffset;       // size of the header section
                uint hdrCrc  = CRC.CRC32(headerSection);

                Log($"擦除 0x{hdrAddr:X8} size={hdrSize}");
                if (!await transfer.Erase(hdrAddr, hdrSize))
                {
                    Log("错误：擦除 Magic Header 区失败。");
                    return;
                }

                SetProgress(0);
                Log("写入 Magic Header……");
                if (!await transfer.Program(hdrAddr, headerSection, mtu.Value))
                {
                    Log("错误：写入 Magic Header 失败。");
                    return;
                }

                Log("校验 Magic Header……");
                if (!await transfer.Verify(hdrAddr, hdrSize, hdrCrc))
                {
                    Log("错误：Magic Header 校验失败。");
                    return;
                }
                Log("Magic Header 写入成功 ✓");

                // ── 6. Encrypt app binary ─────────────────────────────────────
                Log("─── 加密固件（AES-128-CTR） ───");
                byte[] nonce        = FwCrypto.GenerateNonce();
                byte[] encryptedApp = FwCrypto.Xcrypt(appBin, nonce);
                Log($"Nonce: {BitConverter.ToString(nonce).Replace("-", "")}");

                // CRC32 of the PLAINTEXT app (what the bootloader verifies after decryption).
                // This must equal header.DataCrc32; recomputing provides an extra sanity check.
                uint plainCrc32 = CRC.CRC32(appBin);
                if (plainCrc32 != header.DataCrc32)
                {
                    Log($"错误：明文 CRC32 (0x{plainCrc32:X8}) 与头部 DataCrc32 (0x{header.DataCrc32:X8}) 不一致，固件文件可能损坏。");
                    Log("升级已中止。");
                    return;
                }
                Log($"明文 CRC32: 0x{plainCrc32:X8} ✓");

                // ── 7. Stream encrypted firmware to W25Q128 ───────────────────
                Log("─── FW_WRITE: 发送加密固件到 W25Q128 ───");
                SetProgress(0);
                if (!await transfer.FwWrite(encryptedApp, mtu.Value))
                {
                    Log("错误：FW_WRITE 失败。");
                    return;
                }
                Log("固件数据传输完成 ✓");

                // ── 8. Commit ─────────────────────────────────────────────────
                Log("─── FW_COMMIT: 验证 → 更新元数据 → 烧录到内部 Flash → 复位 ───");
                if (!await transfer.FwCommit(nonce, fwVersion, plainCrc32))
                {
                    Log("错误：FW_COMMIT 失败（CRC 不匹配或通信错误）。");
                    return;
                }

                Log("FW_COMMIT 成功 ✓  设备正在烧录新固件并重启……");
                Log("升级完成！请等待设备重新上线。");
                SetProgress(100);
            }
            finally
            {
                transfer.Close();
                SetButtonsEnabled(true);
            }
        }
    }
}
