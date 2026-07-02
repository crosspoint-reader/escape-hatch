#!/usr/bin/env python3
"""Patch the secure_version of an ESP32 app image and fix its integrity fields.

These X3/X4 images are hash-validated only (no secure-boot signature), so a
secure_version can be rewritten in place as long as the two integrity fields the
bootloader checks are recomputed to match:
  1. the 1-byte image XOR checksum (last byte of the padded segment region), and
  2. the appended SHA-256 trailer (present when header byte 23 != 0).

This lets an Escape Hatch build (secure_version 0) satisfy an anti-rollback
fleet: set it to the eFuse minimum so `esp_ota_set_boot_partition()` and the
bootloader both accept it. Match the minimum EXACTLY — a higher value can ratchet
the eFuse up on confirm and deepen the downgrade wall.

Usage: patch_secure_version.py <in.bin> <secure_version> [out.bin]
Verify the result with: esptool image-info <out.bin>
"""
import hashlib
import struct
import sys

HEADER_SIZE = 24          # esp_image_header_t
SEG_HEADER_SIZE = 8       # load_addr(4) + data_len(4)
CHECKSUM_SEED = 0xEF
MAGIC = 0xE9
# esp_app_desc_t sits at the start of the first segment's data (flash 0x20);
# secure_version is its second u32 -> flash offset 0x24.
SECVER_OFF = HEADER_SIZE + SEG_HEADER_SIZE + 4  # 0x24


APPVER_OFF = HEADER_SIZE + SEG_HEADER_SIZE + 0x10  # 0x30, char version[32]
APPVER_LEN = 32


def patch(data: bytearray, secure_version=None, app_version=None) -> bytearray:
    if data[0] != MAGIC:
        raise SystemExit(f"not an ESP image: magic 0x{data[0]:02X} != 0xE9")
    seg_count = data[1]
    hash_appended = data[23] != 0

    # 1. Overwrite the requested app-descriptor fields.
    if secure_version is not None:
        struct.pack_into("<I", data, SECVER_OFF, secure_version)  # u32 LE
    if app_version is not None:
        raw = app_version.encode()
        if len(raw) >= APPVER_LEN:
            raise SystemExit(f"app version too long: {len(raw)} >= {APPVER_LEN}")
        data[APPVER_OFF:APPVER_OFF + APPVER_LEN] = raw + b"\x00" * (APPVER_LEN - len(raw))

    # 2. Walk segments, XOR all segment *data* bytes (seed 0xEF) -> checksum.
    xor = CHECKSUM_SEED
    pos = HEADER_SIZE
    for i in range(seg_count):
        (_, data_len) = struct.unpack_from("<II", data, pos)
        pos += SEG_HEADER_SIZE
        for b in data[pos:pos + data_len]:
            xor ^= b
        pos += data_len

    # Checksum byte lives at the last byte of the 16-byte-aligned padded region.
    pad_end = (pos + 16) & ~15
    data[pad_end - 1] = xor & 0xFF

    # 3. Recompute the appended SHA-256 over everything up to (not incl.) it.
    if hash_appended:
        digest = hashlib.sha256(bytes(data[:pad_end])).digest()
        data[pad_end:pad_end + 32] = digest
        expected_len = pad_end + 32
    else:
        expected_len = pad_end
    if len(data) != expected_len:
        raise SystemExit(f"length mismatch: file {len(data)} != expected {expected_len}")
    return data


def main():
    # Usage: patch.py <in.bin> [--secure-version N] [--app-version STR] [--out FILE]
    args = sys.argv[1:]
    if not args:
        raise SystemExit(__doc__)
    src = args[0]
    secver = appver = dst = None
    i = 1
    while i < len(args):
        if args[i] == "--secure-version":
            secver = int(args[i + 1], 0); i += 2
        elif args[i] == "--app-version":
            appver = args[i + 1]; i += 2
        elif args[i] == "--out":
            dst = args[i + 1]; i += 2
        else:
            raise SystemExit(f"unknown arg: {args[i]}\n{__doc__}")
    if secver is None and appver is None:
        raise SystemExit("nothing to patch: pass --secure-version and/or --app-version")
    if dst is None:
        tag = (f"sv{secver}" if secver is not None else "") + (f"av{appver}" if appver else "")
        dst = src.rsplit(".", 1)[0] + f".{tag}.bin"
    with open(src, "rb") as f:
        data = bytearray(f.read())
    patch(data, secver, appver)
    with open(dst, "wb") as f:
        f.write(data)
    print(f"wrote {dst}: secure_version={secver} app_version={appver!r} {len(data)} bytes")


if __name__ == "__main__":
    main()
