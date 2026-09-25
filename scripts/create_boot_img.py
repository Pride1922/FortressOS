#!/usr/bin/env python3
"""
scripts/create_boot_img.py - Generate a Dual-Boot GPT/ESP Raw Disk Image (bin/fortress.img)

Creates a bootable raw disk image featuring:
- Protective MBR (LBA 0) with Limine BIOS Stage 1
- Primary GPT Header (LBA 1) and Partition Array (LBAs 2..33)
- Embedded Limine BIOS Stage 2 in GPT gap (LBAs 34..2047)
- Partition 1 (ESP, FAT32): LBAs 2048..133119 (64 MiB) with UEFI/BIOS loaders,
  kernel, initramfs, configuration, and boot splash.
- Partition 2 (Linux FS, ext2): LBAs 133120..264191 (64 MiB) with clean ext2
  filesystem for persistent user storage mounted at /mnt.
- Backup GPT Partition Array (LBAs total-33..total-2) and Backup GPT Header (LBA total-1).
"""

import sys
import os
import struct
import binascii
import uuid
import subprocess
import tempfile
import argparse
from pathlib import Path

SECTOR_SIZE = 512
TOTAL_SECTORS = 266240  # 130 MiB (136,314,880 bytes)

# Partition 1: ESP (64 MiB = 131072 sectors)
PART1_START = 2048
PART1_SECTORS = 131072
PART1_END = PART1_START + PART1_SECTORS - 1  # 133119

# Partition 2: Persistent ext2 Data (64 MiB = 131072 sectors)
PART2_START = 133120
PART2_SECTORS = 131072
PART2_END = PART2_START + PART2_SECTORS - 1  # 264191

# Standard GPT GUIDs (little-endian byte representations)
# EFI System Partition: C12A7328-F81F-11D2-BA4B-00A0C93EC93B
GUID_ESP = bytes([
    0x28, 0x73, 0x2A, 0xC1,
    0x1F, 0xF8,
    0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
])

# Linux Filesystem Data: 0FC63DAF-8483-4772-8E79-3D69D8477DE4
GUID_LINUX_FS = bytes([
    0xAF, 0x3D, 0xC6, 0x0F,
    0x83, 0x84,
    0x72, 0x47,
    0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4
])


def crc32(data: bytes) -> int:
    return binascii.crc32(data) & 0xFFFFFFFF


def create_esp_partition(esp_path: Path, iso_root: Path, limine_dir: Path,
                         kernel_elf: Path, initramfs_tar: Path, limine_conf: Path,
                         splash_file: Path):
    """Formats a 64 MiB FAT32 ESP partition and populates it with boot files."""
    esp_bytes = PART1_SECTORS * SECTOR_SIZE
    with open(esp_path, "wb") as f:
        f.truncate(esp_bytes)

    # Format FAT32 using mformat
    cmd_format = ["mformat", "-i", str(esp_path), "-F", "-v", "FORTRESS_ESP", "::"]
    subprocess.run(cmd_format, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

    # Create directories in FAT32
    for d in ["::EFI", "::EFI/BOOT", "::boot", "::boot/limine"]:
        subprocess.run(["mmd", "-i", str(esp_path), d], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

    # Collect source files
    files_to_copy = []

    # If iso_root is provided and contains files, prefer those
    if iso_root and iso_root.is_dir():
        # Check for standard paths in iso_root
        k_src = iso_root / "boot" / "fortress.elf"
        init_src = iso_root / "boot" / "initramfs.tar"
        conf_src = iso_root / "boot" / "limine" / "limine.conf"
        if not conf_src.is_file():
            conf_src = iso_root / "boot" / "limine.conf"
        splash_src = iso_root / "boot" / "splash.png"
        bios_sys_src = iso_root / "boot" / "limine" / "limine-bios.sys"
        bootx64_src = iso_root / "EFI" / "BOOT" / "BOOTX64.EFI"
        bootia32_src = iso_root / "EFI" / "BOOT" / "BOOTIA32.EFI"
    else:
        k_src = kernel_elf
        init_src = initramfs_tar
        conf_src = limine_conf
        splash_src = splash_file
        bios_sys_src = limine_dir / "limine-bios.sys"
        bootx64_src = limine_dir / "BOOTX64.EFI"
        bootia32_src = limine_dir / "BOOTIA32.EFI"

    # Fallbacks from limine_dir if not found
    if not bios_sys_src.is_file() and limine_dir:
        bios_sys_src = limine_dir / "limine-bios.sys"
    if not bootx64_src.is_file() and limine_dir:
        bootx64_src = limine_dir / "BOOTX64.EFI"
    if not bootia32_src.is_file() and limine_dir:
        bootia32_src = limine_dir / "BOOTIA32.EFI"

    # Validate required files
    for path, name in [(k_src, "Kernel ELF"), (init_src, "Initramfs TAR"), (bootx64_src, "BOOTX64.EFI")]:
        if not path or not path.is_file():
            raise FileNotFoundError(f"Required boot file missing: {name} ({path})")

    # Mappings: (source_path, target_fat_path)
    mappings = [
        (bootx64_src, "::EFI/BOOT/BOOTX64.EFI"),
        (k_src, "::boot/fortress.elf"),
        (init_src, "::boot/initramfs.tar"),
        (init_src, "::initramfs.tar"),
    ]

    if bootia32_src and bootia32_src.is_file():
        mappings.append((bootia32_src, "::EFI/BOOT/BOOTIA32.EFI"))

    if bios_sys_src and bios_sys_src.is_file():
        mappings.append((bios_sys_src, "::boot/limine-bios.sys"))
        mappings.append((bios_sys_src, "::boot/limine/limine-bios.sys"))
        mappings.append((bios_sys_src, "::limine-bios.sys"))

    if conf_src and conf_src.is_file():
        mappings.append((conf_src, "::boot/limine.conf"))
        mappings.append((conf_src, "::boot/limine/limine.conf"))
        mappings.append((conf_src, "::EFI/BOOT/limine.conf"))
        mappings.append((conf_src, "::limine.conf"))

    if splash_src and splash_src.is_file():
        mappings.append((splash_src, "::boot/splash.png"))
        mappings.append((splash_src, "::boot/limine/splash.png"))

    for src, dst in mappings:
        res = subprocess.run(["mcopy", "-i", str(esp_path), str(src), dst],
                             stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        if res.returncode != 0:
            print(f"Warning: mcopy failed copying {src} -> {dst}: {res.stderr.decode()}", file=sys.stderr)


def create_ext2_partition(ext2_path: Path):
    """Formats a 64 MiB ext2 partition with clean superblock and initial files."""
    ext2_bytes = PART2_SECTORS * SECTOR_SIZE
    with open(ext2_path, "wb") as f:
        f.truncate(ext2_bytes)

    with tempfile.TemporaryDirectory() as staging:
        staging_dir = Path(staging) / "root"
        staging_dir.mkdir()
        readme = (
            "=====================================================\n"
            "FortressOS Persistent Storage Partition (/mnt)\n"
            "=====================================================\n"
            "Files created, modified, or saved here persist across\n"
            "system reboots.\n\n"
            "Filesystem: ext2 (1024-byte blocks, revision 1)\n"
            "Mountpoint: /mnt\n"
        )
        (staging_dir / "README.txt").write_text(readme)
        (staging_dir / "notes").mkdir()
        (staging_dir / "notes" / "welcome.txt").write_text(
            "Welcome to FortressOS persistent USB storage!\n"
            "Use the built-in shell commands (edit, mkdir, rm, mv) to manage your files.\n"
        )

        cmd_mke2fs = [
            "mke2fs", "-q", "-t", "ext2", "-b", "1024", "-I", "128",
            "-O", "none,filetype,sparse_super,large_file",
            "-L", "FORTRESS_DATA",
            "-d", str(staging_dir),
            "-F", str(ext2_path)
        ]
        subprocess.run(cmd_mke2fs, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

    # Verify integrity with e2fsck
    res = subprocess.run(["e2fsck", "-fn", str(ext2_path)],
                         stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if res.returncode != 0:
        raise RuntimeError(f"e2fsck verification failed on new ext2 partition: {res.stderr.decode()}")


def build_bootable_img(output_img: Path, iso_root: Path, limine_dir: Path,
                       kernel_elf: Path, initramfs_tar: Path, limine_conf: Path,
                       splash_file: Path):
    """Constructs the complete GPT partitioned dual-boot raw disk image."""
    total_bytes = TOTAL_SECTORS * SECTOR_SIZE

    disk_guid = uuid.uuid4().bytes_le
    part1_guid = uuid.uuid4().bytes_le
    part2_guid = uuid.uuid4().bytes_le

    # 1. Protective MBR (LBA 0)
    mbr = bytearray(SECTOR_SIZE)
    mbr_entry = struct.pack(
        "<B3sB3sII",
        0x00,
        bytes([0x00, 0x02, 0x00]),
        0xEE,  # GPT Protective MBR type
        bytes([0xFF, 0xFF, 0xFF]),
        1,
        TOTAL_SECTORS - 1
    )
    mbr[446:446 + 16] = mbr_entry
    mbr[510:512] = struct.pack("<H", 0xAA55)

    # 2. Partition Entry Array (128 entries x 128 bytes = 16384 bytes = 32 sectors)
    part_entry_size = 128
    num_part_entries = 128
    part_array = bytearray(num_part_entries * part_entry_size)

    # Partition 1: EFI System Partition (ESP)
    part1_name = "EFI System Partition".encode("utf-16le").ljust(72, b"\x00")[:72]
    part1_entry = struct.pack(
        "<16s16sQQQ72s",
        GUID_ESP,
        part1_guid,
        PART1_START,
        PART1_END,
        0,  # Attributes
        part1_name
    )
    part_array[0:part_entry_size] = part1_entry

    # Partition 2: Linux Filesystem (Persistent Data)
    part2_name = "Fortress Persistent Data".encode("utf-16le").ljust(72, b"\x00")[:72]
    part2_entry = struct.pack(
        "<16s16sQQQ72s",
        GUID_LINUX_FS,
        part2_guid,
        PART2_START,
        PART2_END,
        0,  # Attributes
        part2_name
    )
    part_array[part_entry_size:2 * part_entry_size] = part2_entry

    part_array_crc = crc32(part_array)

    # 3. Primary GPT Header (LBA 1)
    primary_hdr_lba = 1
    backup_hdr_lba = TOTAL_SECTORS - 1
    first_usable_lba = 34
    last_usable_lba = TOTAL_SECTORS - 34
    primary_part_array_lba = 2

    hdr_format = "<8sIIIIQQQQ16sQIII"
    hdr_size = 92

    primary_hdr_raw = struct.pack(
        hdr_format,
        b"EFI PART",
        0x00010000,
        hdr_size,
        0,  # Header CRC placeholder
        0,  # Reserved
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
    ).ljust(SECTOR_SIZE, b"\x00")

    # 4. Backup GPT Header & Partition Array
    backup_part_array_lba = TOTAL_SECTORS - 33
    backup_hdr_raw = struct.pack(
        hdr_format,
        b"EFI PART",
        0x00010000,
        hdr_size,
        0,  # Header CRC placeholder
        0,  # Reserved
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
    ).ljust(SECTOR_SIZE, b"\x00")

    part2_uuid = uuid.UUID(bytes_le=part2_guid)
    part2_guid_str = str(part2_uuid).upper()

    output_img.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory() as temp_dir:
        temp = Path(temp_dir)
        part1_img = temp / "part1_esp.img"
        part2_img = temp / "part2_ext2.img"
        custom_limine_conf = temp / "limine.conf"

        conf_content = (
            "# Limine Bootloader Configuration for FortressOS\n"
            "# Generated by scripts/create_boot_img.py. Do not edit in place;\n"
            "# edit the script and rebuild.\n"
            "timeout: 5\n"
            "wallpaper: boot():/boot/splash.png\n"
            "wallpaper_style: centered\n"
            "backdrop: 1A1B26\n\n"
            f"/FortressOS (Persistent Storage: PARTUUID={part2_guid_str})\n"
            "    protocol: limine\n"
            "    kernel_path: boot():/boot/fortress.elf\n"
            "    module_path: boot():/boot/initramfs.tar\n"
            f"    kernel_cmdline: usb_data=PARTUUID={part2_guid_str} usb_data_mode=rw\n\n"
            f"/FortressOS (SMP 6D VMM Lifecycle Test)\n"
            "    protocol: limine\n"
            "    kernel_path: boot():/boot/fortress.elf\n"
            "    module_path: boot():/boot/initramfs.tar\n"
            f"    kernel_cmdline: smp_memory_test=vmm_lifecycle usb_data=PARTUUID={part2_guid_str} usb_data_mode=rw\n\n"
            f"/FortressOS (SMP 6B Memory Stress Test)\n"
            "    protocol: limine\n"
            "    kernel_path: boot():/boot/fortress.elf\n"
            "    module_path: boot():/boot/initramfs.tar\n"
            f"    kernel_cmdline: smp_memory_test=stress usb_data=PARTUUID={part2_guid_str} usb_data_mode=rw\n\n"
            f"/FortressOS (Recovery: Read-only, PARTUUID={part2_guid_str})\n"
            "    protocol: limine\n"
            "    kernel_path: boot():/boot/fortress.elf\n"
            "    module_path: boot():/boot/initramfs.tar\n"
            f"    kernel_cmdline: usb_data=PARTUUID={part2_guid_str} usb_data_mode=ro\n"
        )
        custom_limine_conf.write_text(conf_content)

        print(f"  [IMG] Data partition PARTUUID: {part2_guid_str}")
        print("  [IMG] Formatting FAT32 EFI System Partition (64 MiB)...")
        create_esp_partition(part1_img, None, limine_dir, kernel_elf,
                             initramfs_tar, custom_limine_conf, splash_file)

        print("  [IMG] Formatting ext2 Persistent Data Partition (64 MiB)...")
        create_ext2_partition(part2_img)

        print(f"  [IMG] Assembling raw disk image ({TOTAL_SECTORS * SECTOR_SIZE // (1024 * 1024)} MiB)...")
        with open(output_img, "wb") as f:
            f.truncate(total_bytes)

            # LBA 0: Protective MBR
            f.seek(0)
            f.write(mbr)

            # LBA 1: Primary GPT Header
            f.seek(primary_hdr_lba * SECTOR_SIZE)
            f.write(primary_hdr)

            # LBAs 2..33: Primary Partition Array
            f.seek(primary_part_array_lba * SECTOR_SIZE)
            f.write(part_array)

            # LBAs 2048..133119: Partition 1 (ESP)
            f.seek(PART1_START * SECTOR_SIZE)
            f.write(part1_img.read_bytes())

            # LBAs 133120..264191: Partition 2 (ext2)
            f.seek(PART2_START * SECTOR_SIZE)
            f.write(part2_img.read_bytes())

            # Backup Partition Array
            f.seek(backup_part_array_lba * SECTOR_SIZE)
            f.write(part_array)

            # Backup GPT Header
            f.seek(backup_hdr_lba * SECTOR_SIZE)
            f.write(backup_hdr)

    # 5. Deploy Limine BIOS Stage 1 & Stage 2 boot code
    limine_bin = limine_dir / "limine"
    if limine_bin.is_file():
        print("  [IMG] Deploying Limine BIOS boot record to disk image...")
        res = subprocess.run([str(limine_bin), "bios-install", str(output_img)],
                             capture_output=True, text=True)
        if res.returncode != 0:
            print(f"Warning: limine bios-install reported: {res.stderr.strip()}", file=sys.stderr)
        else:
            print(f"  [IMG] Limine BIOS Stage 1/2 successfully installed.")

    print(f"[OK] Bootable Raw Disk Image generated: {output_img}")


def verify_image(img_path: Path) -> bool:
    """Verifies MBR, GPT, FAT32, and ext2 structures in an existing raw disk image."""
    print(f"[VERIFY] Checking raw disk image: {img_path}")
    if not img_path.is_file():
        print(f"Error: {img_path} does not exist", file=sys.stderr)
        return False

    with open(img_path, "rb") as f:
        # 1. Protective MBR check
        f.seek(0)
        mbr = f.read(SECTOR_SIZE)
        if len(mbr) != SECTOR_SIZE or mbr[510:512] != b"\x55\xAA":
            print("FAIL: Invalid MBR signature", file=sys.stderr)
            return False
        mbr_type = mbr[446 + 4]
        if mbr_type != 0xEE:
            print(f"FAIL: Protective MBR type is 0x{mbr_type:02X}, expected 0xEE", file=sys.stderr)
            return False

        # 2. Primary GPT Header check
        f.seek(1 * SECTOR_SIZE)
        hdr = f.read(SECTOR_SIZE)
        if hdr[:8] != b"EFI PART":
            print("FAIL: Primary GPT signature mismatch", file=sys.stderr)
            return False

        hdr_format = "<8sIIIIQQQQ16sQIII"
        hdr_fields = struct.unpack(hdr_format, hdr[:92])
        saved_crc = hdr_fields[3]
        raw_for_crc = struct.pack(hdr_format, *hdr_fields[:3], 0, *hdr_fields[4:])
        calc_crc = crc32(raw_for_crc)
        if saved_crc != calc_crc:
            print(f"FAIL: Primary GPT Header CRC mismatch: 0x{saved_crc:08X} != 0x{calc_crc:08X}", file=sys.stderr)
            return False

        # 3. Partition Array check
        part_array_lba = hdr_fields[10]
        num_part_entries = hdr_fields[11]
        part_entry_size = hdr_fields[12]
        part_array_crc_expected = hdr_fields[13]

        f.seek(part_array_lba * SECTOR_SIZE)
        part_array = f.read(num_part_entries * part_entry_size)
        array_crc = crc32(part_array)
        if array_crc != part_array_crc_expected:
            print(f"FAIL: Partition array CRC mismatch: 0x{array_crc:08X} != 0x{part_array_crc_expected:08X}", file=sys.stderr)
            return False

        # Check Partition 1 (ESP)
        p1 = struct.unpack("<16s16sQQQ72s", part_array[:part_entry_size])
        if p1[0] != GUID_ESP or p1[2] != PART1_START or p1[3] != PART1_END:
            print(f"FAIL: Partition 1 mismatch. GUID={p1[0].hex()} Start={p1[2]} End={p1[3]}", file=sys.stderr)
            return False

        # Check Partition 2 (Linux FS)
        p2 = struct.unpack("<16s16sQQQ72s", part_array[part_entry_size:2 * part_entry_size])
        if p2[0] != GUID_LINUX_FS or p2[2] != PART2_START or p2[3] != PART2_END:
            print(f"FAIL: Partition 2 mismatch. GUID={p2[0].hex()} Start={p2[2]} End={p2[3]}", file=sys.stderr)
            return False

        # 4. Check Backup GPT Header and Backup Partition Array
        total_sectors = os.path.getsize(img_path) // SECTOR_SIZE
        f.seek((total_sectors - 1) * SECTOR_SIZE)
        bak_hdr = f.read(SECTOR_SIZE)
        if bak_hdr[:8] != b"EFI PART":
            print("FAIL: Backup GPT signature mismatch", file=sys.stderr)
            return False
        bak_fields = struct.unpack(hdr_format, bak_hdr[:92])
        bak_saved_crc = bak_fields[3]
        bak_raw_for_crc = struct.pack(hdr_format, *bak_fields[:3], 0, *bak_fields[4:])
        if bak_saved_crc != crc32(bak_raw_for_crc):
            print("FAIL: Backup GPT Header CRC mismatch", file=sys.stderr)
            return False
        # Cross-check: primary and backup headers must agree on each other's LBAs.
        # The alternate LBA field (index 6) is the pointer to the other header.
        total_sectors = os.path.getsize(img_path) // SECTOR_SIZE
        expected_primary_lba = 1
        expected_backup_lba = total_sectors - 1
        if hdr_fields[5] != expected_primary_lba or hdr_fields[6] != expected_backup_lba:
            print(f"FAIL: primary header self/backup LBA mismatch "
                  f"(self={hdr_fields[5]}, backup={hdr_fields[6]}, "
                  f"expected self={expected_primary_lba}, backup={expected_backup_lba})",
                  file=sys.stderr)
            return False
        if bak_fields[5] != expected_backup_lba or bak_fields[6] != expected_primary_lba:
            print(f"FAIL: backup header self/primary LBA mismatch "
                  f"(self={bak_fields[5]}, primary={bak_fields[6]}, "
                  f"expected self={expected_backup_lba}, primary={expected_primary_lba})",
                  file=sys.stderr)
            return False

        # Check Backup Partition Array
        bak_array_lba = bak_fields[10]
        f.seek(bak_array_lba * SECTOR_SIZE)
        bak_array = f.read(bak_fields[11] * bak_fields[12])
        if crc32(bak_array) != bak_fields[13]:
            print("FAIL: Backup partition array CRC mismatch", file=sys.stderr)
            return False

    # 5. Check FAT32 ESP filesystem with mdir
    fat_offset = PART1_START * SECTOR_SIZE
    res = subprocess.run(["mdir", "-i", f"{img_path}@@{fat_offset}", "::EFI/BOOT"],
                         capture_output=True, text=True)
    if res.returncode != 0 or "BOOTX64" not in res.stdout:
        print(f"FAIL: FAT32 ESP missing BOOTX64.EFI:\n{res.stderr}", file=sys.stderr)
        return False

    # 6. Check ext2 filesystem with e2fsck
    with tempfile.TemporaryDirectory(prefix="fortress-verify-ext2-") as tmp:
        part2_file = Path(tmp) / "part2.ext2"
        ext2_offset = PART2_START * SECTOR_SIZE
        ext2_size = PART2_SECTORS * SECTOR_SIZE
        with open(img_path, "rb") as f:
            f.seek(ext2_offset)
            ext2_data = f.read(ext2_size)
            if len(ext2_data) != ext2_size:
                print("FAIL: Truncated ext2 partition data", file=sys.stderr)
                return False
            part2_file.write_bytes(ext2_data)

        res = subprocess.run(["e2fsck", "-fn", str(part2_file)],
                             capture_output=True, text=True)
        if res.returncode != 0:
            print(f"FAIL: ext2 filesystem check failed:\n{res.stdout}\n{res.stderr}", file=sys.stderr)
            return False

    print("  [PASS] Protective MBR verified (Type 0xEE, valid boot signature)")
    print("  [PASS] Primary GPT Header and Partition Array CRC verified")
    print("  [PASS] Partition 1: ESP (FAT32, 64 MiB) containing BOOTX64.EFI verified")
    print("  [PASS] Partition 2: Linux FS (ext2, 64 MiB) verified with 0 errors via e2fsck")
    print("  [PASS] Backup GPT Header and Partition Array verified")
    return True


def main():
    parser = argparse.ArgumentParser(description="Create FortressOS bootable raw disk image")
    parser.add_argument("output", help="Path to output image file (bin/fortress.img)")
    parser.add_argument("--iso-root", type=Path, default=Path("build/iso_root"),
                        help="Path to prepared ISO root directory containing boot files")
    parser.add_argument("--limine-dir", type=Path, default=Path("limine"),
                        help="Path to limine bootloader directory")
    parser.add_argument("--kernel", type=Path, default=Path("bin/fortress.elf"),
                        help="Path to fortress.elf kernel")
    parser.add_argument("--initramfs", type=Path, default=Path("bin/initramfs.tar"),
                        help="Path to initramfs.tar")
    parser.add_argument("--limine-conf", type=Path, default=Path("limine.conf"),
                        help="Path to limine.conf")
    parser.add_argument("--splash", type=Path, default=Path("assets/splash.png"),
                        help="Path to splash image")
    parser.add_argument("--verify", action="store_true",
                        help="Verify an existing image rather than generating one")

    args = parser.parse_args()

    out_path = Path(args.output)
    if args.verify:
        success = verify_image(out_path)
        sys.exit(0 if success else 1)

    build_bootable_img(
        output_img=out_path,
        iso_root=args.iso_root,
        limine_dir=args.limine_dir,
        kernel_elf=args.kernel,
        initramfs_tar=args.initramfs,
        limine_conf=args.limine_conf,
        splash_file=args.splash
    )

    # Automatically verify newly generated image
    if not verify_image(out_path):
        print("Error: Image verification failed post-generation", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
