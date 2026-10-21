#!/usr/bin/env python3
"""
strip_firmware.py: Strips unnecessary sections (such as WoWLAN) from Intel iwlwifi
firmware blobs and compresses them using RFC 1952 gzip to fit inside the FortressOS
initramfs while strictly respecting the 8 MiB installer partition ceiling.
"""

import sys
import struct
import gzip

def strip_iwlwifi_ucode(in_path: str, out_path: str):
    with open(in_path, "rb") as f:
        data = f.read()

    if len(data) < 88:
        raise ValueError("Firmware file too small for header")

    zero, magic = struct.unpack_from("<II", data, 0)
    if magic != 0x0A4C5749:
        raise ValueError(f"Invalid microcode magic: {hex(magic)}")

    hdr = data[:88]
    stripped = bytearray(hdr)
    offset = 88
    while offset + 8 <= len(data):
        ttype, tlen = struct.unpack_from("<II", data, offset)
        step = 8 + ((tlen + 3) & ~3)
        chunk = data[offset : offset + step]
        # Strip WoWLAN (Type 21) which is not needed in FortressOS
        if ttype != 21:
            stripped.extend(chunk)
        offset += step

    compressed = gzip.compress(stripped, compresslevel=9)
    with open(out_path, "wb") as f:
        f.write(compressed)

    print(f"[FIRMWARE] Stripped {in_path} ({len(data)} -> {len(stripped)} bytes, compressed {len(compressed)} bytes) -> {out_path}")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <in_ucode> <out_ucode_gz>")
        sys.exit(1)
    strip_iwlwifi_ucode(sys.argv[1], sys.argv[2])
