namespace SerialUpgrader
{
    partial class Form1
    {
        /// <summary>
        ///  Required designer variable.
        /// </summary>
        private System.ComponentModel.IContainer components = null;

        /// <summary>
        ///  Clean up any resources being used.
        /// </summary>
        /// <param name="disposing">true if managed resources should be disposed; otherwise, false.</param>
        protected override void Dispose(bool disposing)
        {
            if (disposing && (components != null))
            {
                components.Dispose();
            }
            base.Dispose(disposing);
        }

        #region Windows Form Designer generated code

        /// <summary>
        ///  Required method for Designer support - do not modify
        ///  the contents of this method with the code editor.
        /// </summary>
        private void InitializeComponent()
        {
            label1 = new Label();
            textBoxFirmware = new TextBox();
            buttonSelectFirmware = new Button();
            label2 = new Label();
            comboBoxSerialPort = new ComboBox();
            labelVersion = new Label();
            numericUpDownFwVersion = new NumericUpDown();
            buttonQueryStatus = new Button();
            buttonRollback = new Button();
            textBoxLog = new TextBox();
            progressBar = new ProgressBar();
            buttonUpgrade = new Button();
            ((System.ComponentModel.ISupportInitialize)numericUpDownFwVersion).BeginInit();
            SuspendLayout();
            // 
            // label1
            // 
            label1.AutoSize = true;
            label1.Font = new Font("Microsoft YaHei UI", 10.5F, FontStyle.Regular, GraphicsUnit.Point, 134);
            label1.Location = new Point(27, 20);
            label1.Name = "label1";
            label1.Size = new Size(96, 28);
            label1.TabIndex = 0;
            label1.Text = "固件文件";
            // 
            // textBoxFirmware
            // 
            textBoxFirmware.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
            textBoxFirmware.BackColor = SystemColors.Window;
            textBoxFirmware.Location = new Point(122, 16);
            textBoxFirmware.Name = "textBoxFirmware";
            textBoxFirmware.ReadOnly = true;
            textBoxFirmware.Size = new Size(624, 30);
            textBoxFirmware.TabIndex = 1;
            // 
            // buttonSelectFirmware
            // 
            buttonSelectFirmware.Anchor = AnchorStyles.Top | AnchorStyles.Right;
            buttonSelectFirmware.Location = new Point(752, 16);
            buttonSelectFirmware.Name = "buttonSelectFirmware";
            buttonSelectFirmware.Size = new Size(46, 30);
            buttonSelectFirmware.TabIndex = 2;
            buttonSelectFirmware.Text = "...";
            buttonSelectFirmware.UseVisualStyleBackColor = true;
            buttonSelectFirmware.Click += buttonSelectFirmware_Click;
            // 
            // label2
            // 
            label2.AutoSize = true;
            label2.Font = new Font("Microsoft YaHei UI", 10.5F, FontStyle.Regular, GraphicsUnit.Point, 134);
            label2.Location = new Point(27, 56);
            label2.Name = "label2";
            label2.Size = new Size(54, 28);
            label2.TabIndex = 3;
            label2.Text = "端口";
            // 
            // comboBoxSerialPort
            // 
            comboBoxSerialPort.FormattingEnabled = true;
            comboBoxSerialPort.Location = new Point(90, 53);
            comboBoxSerialPort.Name = "comboBoxSerialPort";
            comboBoxSerialPort.Size = new Size(120, 32);
            comboBoxSerialPort.TabIndex = 4;
            // 
            // labelVersion
            // 
            labelVersion.AutoSize = true;
            labelVersion.Font = new Font("Microsoft YaHei UI", 10.5F, FontStyle.Regular, GraphicsUnit.Point, 134);
            labelVersion.Location = new Point(225, 56);
            labelVersion.Name = "labelVersion";
            labelVersion.Size = new Size(82, 28);
            labelVersion.TabIndex = 5;
            labelVersion.Text = "固件版本";
            // 
            // numericUpDownFwVersion
            // 
            numericUpDownFwVersion.Location = new Point(315, 54);
            numericUpDownFwVersion.Maximum = new decimal(new int[] { -1, 0, 0, 0 }); // uint.MaxValue
            numericUpDownFwVersion.Minimum = new decimal(new int[] { 1, 0, 0, 0 });
            numericUpDownFwVersion.Name = "numericUpDownFwVersion";
            numericUpDownFwVersion.Size = new Size(100, 30);
            numericUpDownFwVersion.TabIndex = 6;
            numericUpDownFwVersion.Value = new decimal(new int[] { 1, 0, 0, 0 });
            // 
            // buttonQueryStatus
            // 
            buttonQueryStatus.Font = new Font("Microsoft YaHei UI", 9F, FontStyle.Regular, GraphicsUnit.Point, 134);
            buttonQueryStatus.Location = new Point(428, 52);
            buttonQueryStatus.Name = "buttonQueryStatus";
            buttonQueryStatus.Size = new Size(150, 34);
            buttonQueryStatus.TabIndex = 7;
            buttonQueryStatus.Text = "查询 W25Q128 状态";
            buttonQueryStatus.UseVisualStyleBackColor = true;
            buttonQueryStatus.Click += buttonQueryStatus_Click;
            // 
            // buttonRollback
            // 
            buttonRollback.Font = new Font("Microsoft YaHei UI", 9F, FontStyle.Regular, GraphicsUnit.Point, 134);
            buttonRollback.Location = new Point(592, 52);
            buttonRollback.Name = "buttonRollback";
            buttonRollback.Size = new Size(100, 34);
            buttonRollback.TabIndex = 8;
            buttonRollback.Text = "回滚固件";
            buttonRollback.UseVisualStyleBackColor = true;
            buttonRollback.Click += buttonRollback_Click;
            // 
            // textBoxLog
            // 
            textBoxLog.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
            textBoxLog.Font = new Font("Microsoft YaHei UI", 10.5F, FontStyle.Regular, GraphicsUnit.Point, 134);
            textBoxLog.Location = new Point(27, 96);
            textBoxLog.Multiline = true;
            textBoxLog.Name = "textBoxLog";
            textBoxLog.ScrollBars = ScrollBars.Vertical;
            textBoxLog.Size = new Size(771, 396);
            textBoxLog.TabIndex = 9;
            // 
            // progressBar
            // 
            progressBar.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
            progressBar.Location = new Point(27, 498);
            progressBar.Name = "progressBar";
            progressBar.Size = new Size(771, 46);
            progressBar.TabIndex = 10;
            // 
            // buttonUpgrade
            // 
            buttonUpgrade.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
            buttonUpgrade.Font = new Font("微软雅黑", 18F, FontStyle.Regular, GraphicsUnit.Point, 134);
            buttonUpgrade.Location = new Point(27, 550);
            buttonUpgrade.Name = "buttonUpgrade";
            buttonUpgrade.Size = new Size(771, 82);
            buttonUpgrade.TabIndex = 11;
            buttonUpgrade.Text = "升 级（加密 OTA → W25Q128）";
            buttonUpgrade.UseVisualStyleBackColor = true;
            buttonUpgrade.Click += buttonUpgrade_Click;
            // 
            // Form1
            // 
            AutoScaleDimensions = new SizeF(11F, 24F);
            AutoScaleMode = AutoScaleMode.Font;
            ClientSize = new Size(822, 648);
            Controls.Add(progressBar);
            Controls.Add(numericUpDownFwVersion);
            Controls.Add(labelVersion);
            Controls.Add(comboBoxSerialPort);
            Controls.Add(buttonUpgrade);
            Controls.Add(buttonSelectFirmware);
            Controls.Add(buttonQueryStatus);
            Controls.Add(buttonRollback);
            Controls.Add(textBoxLog);
            Controls.Add(textBoxFirmware);
            Controls.Add(label2);
            Controls.Add(label1);
            FormBorderStyle = FormBorderStyle.FixedSingle;
            MaximizeBox = false;
            Name = "Form1";
            StartPosition = FormStartPosition.CenterScreen;
            Text = "串行升级工具 (IAP Bootloader)";
            Load += Form1_Load;
            ((System.ComponentModel.ISupportInitialize)numericUpDownFwVersion).EndInit();
            ResumeLayout(false);
            PerformLayout();
        }

        #endregion

        private Label label1;
        private TextBox textBoxFirmware;
        private Button buttonSelectFirmware;
        private Label label2;
        private ComboBox comboBoxSerialPort;
        private Label labelVersion;
        private NumericUpDown numericUpDownFwVersion;
        private Button buttonQueryStatus;
        private Button buttonRollback;
        private TextBox textBoxLog;
        private ProgressBar progressBar;
        private Button buttonUpgrade;
    }
}
