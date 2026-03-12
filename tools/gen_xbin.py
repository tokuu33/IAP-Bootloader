#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_xbin.py — Generate a .xbin firmware package for IAP Bootloader

The .xbin file layout
─────────────────────
  [0   .. 255] : 256-byte magic_header_t  → programmed to HEADER_ADDR in internal Flash
  [256 .. end] : raw app binary           → encrypted by upper PC, sent to W25Q128,
                                            then decrypted and programmed to APP_ADDR

magic_header_t layout (256 bytes, little-endian)
─────────────────────────────────────────────────
  Offset  Size  Field
  ------  ----  -----
   0       4    magic           = 0x4D414749  ('MAGI')
   4       4    update_flag     = 0  (initially; set by app/bootloader on OTA trigger)
   8       4    rollback_flag   = 0  (set by app when rollback is requested)
  12       4    boot_fail_count = 0  (incremented by bootloader on failed boots)
  16       4    data_type       = 0  (MAGIC_HEADER_TYPE_APP)
  20       4    data_offset     = 256  (offset of app binary within .xbin file)
  24       4    data_address    = APP_ADDR  (where app is programmed in internal Flash)
  28       4    data_length     = len(app binary)
  32       4    data_crc32      = CRC32(app binary)
  36       4    new_app_length  = 0  (filled by bootloader after FW_COMMIT)
  40       4    new_app_crc32   = 0  (filled by bootloader after FW_COMMIT)
  44       4    backup_length   = 0  (filled by bootloader after backup write)
  48       4    backup_crc32    = 0  (filled by bootloader after backup write)
  52     128    version         = ASCII string, null-padded to 128 bytes
 180      68    reserved3       = 17 × uint32 zeros
 248       4    this_address    = HEADER_ADDR  (where header lives in internal Flash)
 252       4    this_crc32      = CRC32(header[0:252])
"""

import argparse
import os
import struct
import sys
import zlib

# ── Constants ─────────────────────────────────────────────────────────────────

MAGIC_HEADER_MAGIC    = 0x4D414749   # 'MAGI' in ASCII
DATA_TYPE_APP         = 0            # MAGIC_HEADER_TYPE_APP
MAGIC_HEADER_SIZE     = 256
MAGIC_HEADER_CRC_SIZE = 252          # bytes covered by this_crc32 (entire header - last 4 B)

# Default Flash addresses for STM32F407VET6:
#   Sector 3 (16 KB)  @ 0x0800C000 — magic header
#   Sector 4 (64 KB)  @ 0x08010000 — application
DEFAULT_HEADER_ADDR = 0x0800_C000
DEFAULT_APP_ADDR    = 0x0801_0000

# ── CRC32 helper ──────────────────────────────────────────────────────────────

def crc32(data: bytes) -> int:
    """Standard CRC32B (IEEE 802.3, poly 0xEDB88320).
    Matches crc32() in third_lib/crc/crc32.c and CRC.CRC32() in CRC.cs."""
    return zlib.crc32(data) & 0xFFFF_FFFF

# ── Magic header builder ──────────────────────────────────────────────────────

def build_magic_header(
    app_bin: bytes,
    version: str,
    header_addr: int,
    app_addr: int,
) -> bytes:
    """Build the 256-byte magic_header_t for *app_bin*.

    Parameters
    ----------
    app_bin      : raw (unencrypted) application binary
    version      : firmware version string  (max 127 ASCII chars + NUL)
    header_addr  : Flash address where this header will be programmed
    app_addr     : Flash address where the app binary will be programmed
    """
    data_length = len(app_bin)
    data_crc32  = crc32(app_bin)
    data_offset = MAGIC_HEADER_SIZE  # app binary starts right after the header

    # Version: ASCII, truncated to 127 chars, zero-padded to 128 bytes
    ver_bytes = version.encode("ascii")[:127]
    ver_field = ver_bytes + b"\x00" * (128 - len(ver_bytes))

    # Build header[0:252]  (everything before this_crc32)
    prefix = (
        struct.pack("<13I",
            MAGIC_HEADER_MAGIC,  # magic
            0,                   # update_flag
            0,                   # rollback_flag
            0,                   # boot_fail_count
            DATA_TYPE_APP,       # data_type
            data_offset,         # data_offset
            app_addr,            # data_address
            data_length,         # data_length
            data_crc32,          # data_crc32
            0,                   # new_app_length  (filled later by bootloader)
            0,                   # new_app_crc32   (filled later by bootloader)
            0,                   # backup_length   (filled later by bootloader)
            0,                   # backup_crc32    (filled later by bootloader)
        )
        + ver_field                         # version[128]
        + struct.pack("<17I", *([0] * 17))  # reserved3[17]
        + struct.pack("<I", header_addr)    # this_address
    )

    assert len(prefix) == MAGIC_HEADER_CRC_SIZE, \
        f"Header prefix length mismatch: {len(prefix)} != {MAGIC_HEADER_CRC_SIZE}"

    # Append this_crc32 = CRC32 of header[0:252]
    this_crc32 = crc32(prefix)
    header = prefix + struct.pack("<I", this_crc32)

    assert len(header) == MAGIC_HEADER_SIZE, \
        f"Header length mismatch: {len(header)} != {MAGIC_HEADER_SIZE}"

    return header

# ── Main ──────────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(
        prog="gen_xbin.py",
        description="Generate a .xbin firmware package for IAP Bootloader",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "input",
        metavar="INPUT.bin",
        help="Input raw application binary (.bin file from linker/objcopy)",
    )
    parser.add_argument(
        "--version",
        default="v1.0.0",
        help="Firmware version string written into magic header (default: v1.0.0)",
    )
    parser.add_argument(
        "--app-address",
        metavar="ADDR",
        type=lambda x: int(x, 0),
        default=DEFAULT_APP_ADDR,
        help=f"Flash address where the app binary is programmed "
             f"(default: 0x{DEFAULT_APP_ADDR:08X})",
    )
    parser.add_argument(
        "--header-address",
        metavar="ADDR",
        type=lambda x: int(x, 0),
        default=DEFAULT_HEADER_ADDR,
        help=f"Flash address where the magic header is programmed "
             f"(default: 0x{DEFAULT_HEADER_ADDR:08X})",
    )
    parser.add_argument(
        "-o", "--output",
        metavar="OUTPUT.xbin",
        help="Output file path (default: INPUT with .xbin extension)",
    )
    args = parser.parse_args()

    # ── Read input binary ─────────────────────────────────────────────────────
    if not os.path.isfile(args.input):
        print(f"ERROR: input file not found: {args.input}", file=sys.stderr)
        sys.exit(1)

    with open(args.input, "rb") as fh:
        app_bin = fh.read()

    if not app_bin:
        print("ERROR: input file is empty", file=sys.stderr)
        sys.exit(1)

    # ── Build .xbin ───────────────────────────────────────────────────────────
    output_path = args.output or (os.path.splitext(args.input)[0] + ".xbin")

    header = build_magic_header(
        app_bin,
        args.version,
        args.header_address,
        args.app_address,
    )
    xbin = header + app_bin

    with open(output_path, "wb") as fh:
        fh.write(xbin)

    # ── Summary ───────────────────────────────────────────────────────────────
    app_crc = crc32(app_bin)
    hdr_crc = crc32(header[:MAGIC_HEADER_CRC_SIZE])
    print(f"Output        : {output_path}  ({len(xbin)} bytes)")
    print(f"Header addr   : 0x{args.header_address:08X}")
    print(f"Header CRC32  : 0x{hdr_crc:08X}")
    print(f"App addr      : 0x{args.app_address:08X}")
    print(f"App size      : {len(app_bin)} bytes ({len(app_bin) / 1024:.2f} KB)")
    print(f"App CRC32     : 0x{app_crc:08X}")
    print(f"Version       : {args.version}")


if __name__ == "__main__":
    main()
