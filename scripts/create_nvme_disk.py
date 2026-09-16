#!/usr/bin/env python3
import sys
import struct
import binascii
import uuid

def crc32(data: bytes) -> int:
    return binascii.crc32(data) & 0xFFFFFFFF

def generate_raw_image(img_path: str, total_sectors: int, sector_size: int):
    total_bytes = total_sectors * sector_size
    print(f"[NVMe] Generating dedicated raw test fixture: {img_path}")
    with open(img_path, "wb") as f:
        f.truncate(total_bytes)
        raw_patterns = [
            (0, "FORTRESS_NVME_LBA0_BOOT_MAGIC_PATTERN_TEST_#0000#_"),
            (1, "FORTRESS_NVME_LBA1_METADATA_HEADER_PATTERN_#0001#_"),
            (100, "FORTRESS_NVME_LBA100_MIDRANGE_INTEGRITY_DATA_#0100#_"),
            (499, "FORTRESS_NVME_LBA499_NEIGHBOUR_GUARD_LOWER_#0499#_"),
            (500, "FORTRESS_NVME_LBA500_VIRGIN_PRE_WRITE_PATTERN_#0500#_"),
            (501, "FORTRESS_NVME_LBA501_NEIGHBOUR_GUARD_UPPER_#0501#_"),
            (600, "FORTRESS_NVME_LBA600_QUEUE_WRAPAROUND_TARGET_#0600#_"),
            (1000, "FORTRESS_NVME_LBA1000_HIGHRANGE_DATA_VERIFY_#1000#_"),
        ]
        for lba, text in raw_patterns:
            f.seek(lba * sector_size)
            block = (text * (sector_size // len(text) + 1)).encode("ascii")[:sector_size]
            f.write(block)
    print("      - Populated raw-sector test patterns at LBAs 0, 1, 100, 499, 500, 501, 600, 1000")

def generate_gpt_image(img_path: str, total_sectors: int, sector_size: int):
    total_bytes = total_sectors * sector_size
    total_size_mb = total_bytes // (1024 * 1024)

    # GUIDs
    linux_fs_guid = bytes([
        0xAF, 0x3D, 0xC6, 0x0F,
        0x83, 0x84,
        0x72, 0x47,
        0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4
    ])
    disk_guid = uuid.UUID("a1b2c3d4-e5f6-7890-1234-56789abcdef0").bytes_le
    part1_guid = uuid.UUID("11223344-5566-7788-99aa-bbccddeeff00").bytes_le

    # 1. Protective MBR (LBA 0)
    mbr = bytearray(sector_size)
    mbr_entry = struct.pack(
        "<B3sB3sII",
        0x00,
        bytes([0x00, 0x02, 0x00]),
        0xEE,
        bytes([0xFF, 0xFF, 0xFF]),
        1,
        total_sectors - 1
    )
    mbr[446:446 + 16] = mbr_entry
    mbr[510:512] = struct.pack("<H", 0xAA55)

    # 2. Partition Entry Array (128 entries x 128 bytes = 16384 bytes = 32 sectors)
    part_entry_size = 128
    num_part_entries = 128
    part_array = bytearray(num_part_entries * part_entry_size)

    # Partition 1: Linux Filesystem Data
    # Start LBA: 2048 (1 MiB), End LBA: 10239 (inclusive, 8192 sectors = 4 MiB)
    part1_start = 2048
    part1_end = 10239
    part1_name = "Fortress Storage".encode("utf-16le").ljust(72, b"\x00")[:72]

    part1_entry = struct.pack(
        "<16s16sQQQ72s",
        linux_fs_guid,
        part1_guid,
        part1_start,
        part1_end,
        0, # Attributes
        part1_name
    )
    part_array[0:part_entry_size] = part1_entry
    # Exact CRC over num_part_entries * part_entry_size bytes
    part_array_crc = crc32(part_array)

    # 3. Primary GPT Header (LBA 1)
    primary_hdr_lba = 1
    backup_hdr_lba = total_sectors - 1
    first_usable_lba = 34
    last_usable_lba = total_sectors - 34
    primary_part_array_lba = 2

    hdr_format = "<8sIIIIQQQQ16sQIII"
    hdr_size = 92
    primary_hdr_raw = struct.pack(
        hdr_format,
        b"EFI PART",
        0x00010000,
        hdr_size,
        0, # Header CRC placeholder
        0, # Reserved
        primary_hdr_lba,
        backup_hdr_lba,
        first_usable_lba,
        last_usable_lba,
        disk_guid,
        primary_part_array_lba,
        num_part_entries,
        part_entry_size,
        part_array_crc
    )
    primary_hdr_crc = crc32(primary_hdr_raw)
    primary_hdr = struct.pack(
        hdr_format,
        b"EFI PART",
        0x00010000,
        hdr_size,
        primary_hdr_crc,
        0,
        primary_hdr_lba,
        backup_hdr_lba,
        first_usable_lba,
        last_usable_lba,
        disk_guid,
        primary_part_array_lba,
        num_part_entries,
        part_entry_size,
        part_array_crc
    ).ljust(sector_size, b"\x00")

    # 4. Backup GPT Header (LBA total_sectors - 1) & Backup Partition Array
    backup_part_array_lba = total_sectors - 33
    backup_hdr_raw = struct.pack(
        hdr_format,
        b"EFI PART",
        0x00010000,
        hdr_size,
        0, # Header CRC placeholder
        0, # Reserved
        backup_hdr_lba,
        primary_hdr_lba,
        first_usable_lba,
        last_usable_lba,
        disk_guid,
        backup_part_array_lba,
        num_part_entries,
        part_entry_size,
        part_array_crc
    )
    backup_hdr_crc = crc32(backup_hdr_raw)
    backup_hdr = struct.pack(
        hdr_format,
        b"EFI PART",
        0x00010000,
        hdr_size,
        backup_hdr_crc,
        0,
        backup_hdr_lba,
        primary_hdr_lba,
        first_usable_lba,
        last_usable_lba,
        disk_guid,
        backup_part_array_lba,
        num_part_entries,
        part_entry_size,
        part_array_crc
    ).ljust(sector_size, b"\x00")

    with open(img_path, "wb") as f:
        f.truncate(total_bytes)

        # LBA 0: Protective MBR
        f.seek(0)
        f.write(mbr)

        # LBA 1: Primary GPT Header
        f.seek(primary_hdr_lba * sector_size)
        f.write(primary_hdr)

        # LBAs 2..33: Primary Partition Array
        f.seek(primary_part_array_lba * sector_size)
        f.write(part_array)

        # Populate Partition 1 (LBAs 2048..10239, 4 MiB)
        part1_bytes = (part1_end - part1_start + 1) * sector_size
        ext2_success = False
        try:
            import subprocess
            import tempfile
            import os

            with tempfile.NamedTemporaryFile(delete=False) as tmp_part:
                tmp_part_name = tmp_part.name
                tmp_part.truncate(part1_bytes)

            res = subprocess.run(
                ["mke2fs", "-t", "ext2", "-b", "1024", "-F", tmp_part_name],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL
            )
            if res.returncode == 0:
                with tempfile.NamedTemporaryFile("w", delete=False) as hello_tmp:
                    hello_tmp.write("Hello from FortressOS ext2 NVMe partition!\n")
                    hello_tmp_name = hello_tmp.name

                subprocess.run(
                    ["debugfs", "-w", "-R", f"write {hello_tmp_name} hello.txt", tmp_part_name],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL
                )
                try:
                    os.unlink(hello_tmp_name)
                except OSError:
                    pass

                with open(tmp_part_name, "rb") as pf:
                    ext2_data = bytearray(pf.read())

                lba0_pat = ("FORTRESS_EXT2_PARTITION1_START_MAGIC_#0000#_" * 16).encode("ascii")[:sector_size]
                ext2_data[0:sector_size] = lba0_pat

                last_sec_pat = ("FORTRESS_EXT2_PARTITION1_LAST_SECTOR_#8191#_" * 16).encode("ascii")[:sector_size]
                ext2_data[-sector_size:] = last_sec_pat

                f.seek(part1_start * sector_size)
                f.write(ext2_data)
                ext2_success = True
                print("      - Formatted Partition 1 as ext2 filesystem (Magic: 0xEF53)")
            try:
                os.unlink(tmp_part_name)
            except OSError:
                pass
        except Exception:
            ext2_success = False

        if not ext2_success:
            part_patterns = [
                (part1_start, "FORTRESS_EXT2_PARTITION1_START_MAGIC_#0000#_"),
                (part1_start + 1, "FORTRESS_EXT2_PARTITION1_SUPERBLOCK_ZONE_#0001#_"),
                (part1_end, "FORTRESS_EXT2_PARTITION1_LAST_SECTOR_#8191#_"),
            ]
            for lba, text in part_patterns:
                f.seek(lba * sector_size)
                block = (text * (sector_size // len(text) + 1)).encode("ascii")[:sector_size]
                f.write(block)
            f.seek((part1_start + 2) * sector_size)
            sb_block = bytearray(sector_size)
            sb_block[0x38:0x3A] = struct.pack("<H", 0xEF53)
            f.write(sb_block)
            print("      - Populated Partition 1 with fallback ext2 superblock structure")

        # Backup Partition Array & Backup GPT Header
        f.seek(backup_part_array_lba * sector_size)
        f.write(part_array)

        f.seek(backup_hdr_lba * sector_size)
        f.write(backup_hdr)

    print(f"[GPT] Generated {total_size_mb} MiB GPT partitioned disk: {img_path}")
    print(f"      - Protective MBR at LBA 0")
    print(f"      - Primary GPT Header at LBA 1 (CRC: 0x{primary_hdr_crc:08X})")
    print(f"      - Partition Array at LBA 2..33 (CRC: 0x{part_array_crc:08X})")
    print(f"      - Partition 1 (Linux FS Data): LBA {part1_start}..{part1_end} ({part1_end - part1_start + 1} sectors, 4 MiB)")
    print(f"      - Backup Partition Array at LBA {backup_part_array_lba}..{backup_hdr_lba - 1}")
    print(f"      - Backup GPT Header at LBA {backup_hdr_lba} (CRC: 0x{backup_hdr_crc:08X})")

def main():
    if len(sys.argv) < 2:
        print("Usage: create_nvme_disk.py <output_image> [--gpt | --raw | --disposable]")
        sys.exit(1)

    img_path = sys.argv[1]
    mode = "--gpt"
    for arg in sys.argv[2:]:
        if arg in ("--raw", "--disposable"):
            mode = "--raw"
        elif arg == "--gpt":
            mode = "--gpt"

    sector_size = 512
    total_size_mb = 32
    total_sectors = (total_size_mb * 1024 * 1024) // sector_size

    if mode == "--raw":
        generate_raw_image(img_path, total_sectors, sector_size)
    else:
        generate_gpt_image(img_path, total_sectors, sector_size)

if __name__ == "__main__":
    main()
