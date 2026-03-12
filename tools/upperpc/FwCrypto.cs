using System.Security.Cryptography;

namespace SerialUpgrader;

/// <summary>
/// AES-128-CTR firmware encryption/decryption.
///
/// Matches the bootloader's fw_crypto.c implementation exactly:
///   Counter block format: [nonce (8 B)] [counter_be (4 B)] [zeros (4 B)]
///   The counter is a big-endian uint32 starting at 0 and incremented
///   once per 16-byte AES block (including a partial final block).
///
/// CTR mode is symmetric — the same operation both encrypts and decrypts.
///
/// WARNING: The default AES key below is identical to the key hardcoded in
///          fw_crypto.c.  Change it to a project-specific secret before
///          shipping any production firmware.
/// </summary>
public static class FwCrypto
{
    /// <summary>
    /// Default AES-128 key — must match <c>s_key[]</c> in fw_crypto.c.
    /// </summary>
    public static readonly byte[] DefaultKey =
    [
        0x2B, 0x7E, 0x15, 0x16, 0x28, 0xAE, 0xD2, 0xA6,
        0xAB, 0xF7, 0x15, 0x88, 0x09, 0xCF, 0x4F, 0x3C
    ];

    /// <summary>
    /// AES-128-CTR encrypt (or decrypt — CTR is symmetric).
    /// </summary>
    /// <param name="data">Input plaintext or ciphertext.</param>
    /// <param name="nonce">8-byte nonce (Number Used Once).  Generate with
    ///   <see cref="GenerateNonce"/> and pass the same value to
    ///   <c>FW_COMMIT</c> so the bootloader can decrypt.</param>
    /// <param name="key">16-byte AES key.  <c>null</c> → use
    ///   <see cref="DefaultKey"/>.</param>
    /// <returns>Encrypted/decrypted output, same length as <paramref name="data"/>.</returns>
    public static byte[] Xcrypt(byte[] data, byte[] nonce, byte[]? key = null)
    {
        ArgumentNullException.ThrowIfNull(data);
        ArgumentNullException.ThrowIfNull(nonce);
        if (nonce.Length != 8)
            throw new ArgumentException("Nonce must be exactly 8 bytes.", nameof(nonce));

        key ??= DefaultKey;
        if (key.Length != 16)
            throw new ArgumentException("AES key must be exactly 16 bytes.", nameof(key));

        int length = data.Length;
        if (length == 0) return [];

        // Build all AES-CTR counter blocks in one contiguous buffer.
        // Block i  =  [nonce[0..7]] [i >> 24, i >> 16, i >> 8, i & 0xFF] [0, 0, 0, 0]
        int numBlocks    = (length + 15) / 16;
        byte[] ctrBlocks = new byte[numBlocks * 16]; // zero-initialised

        for (int i = 0; i < numBlocks; i++)
        {
            int b = i * 16;
            // nonce occupies bytes 0-7
            ctrBlocks[b + 0] = nonce[0];
            ctrBlocks[b + 1] = nonce[1];
            ctrBlocks[b + 2] = nonce[2];
            ctrBlocks[b + 3] = nonce[3];
            ctrBlocks[b + 4] = nonce[4];
            ctrBlocks[b + 5] = nonce[5];
            ctrBlocks[b + 6] = nonce[6];
            ctrBlocks[b + 7] = nonce[7];
            // counter (big-endian uint32) in bytes 8-11
            uint ctr         = (uint)i;
            ctrBlocks[b + 8]  = (byte)(ctr >> 24);
            ctrBlocks[b + 9]  = (byte)(ctr >> 16);
            ctrBlocks[b + 10] = (byte)(ctr >> 8);
            ctrBlocks[b + 11] = (byte)ctr;
            // bytes 12-15 remain 0
        }

        // Encrypt all counter blocks at once with AES-128-ECB (no padding — buffer
        // is already a multiple of 16 bytes) to produce the keystream.
        using var aes = Aes.Create();
        aes.Key = key;
        byte[] keystream = aes.EncryptEcb(ctrBlocks, PaddingMode.None);

        // XOR keystream with input
        byte[] result = new byte[length];
        for (int i = 0; i < length; i++)
            result[i] = (byte)(data[i] ^ keystream[i]);

        return result;
    }

    /// <summary>
    /// Generate a cryptographically random 8-byte nonce.
    /// Use a fresh nonce for every firmware upload.
    /// </summary>
    public static byte[] GenerateNonce()
    {
        byte[] nonce = new byte[8];
        RandomNumberGenerator.Fill(nonce);
        return nonce;
    }
}
