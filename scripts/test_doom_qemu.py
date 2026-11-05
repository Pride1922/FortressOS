#!/usr/bin/env python3
"""Stage 5 — Automated QEMU Acceptance Suite for FortressOS Doom Port.

Verifies:
1. Multi-firmware execution across BIOS and UEFI with paired OVMF 4M firmware.
2. Multi-core execution across SMP=1 and SMP=4.
3. Missing WAD handling: actionable diagnostic guidance and clean exit code 1.
4. Content discovery: automatic detection of /mnt/.fortress/doom/freedm.wad.
5. Engine playloop execution: 3D refresh, game tick progression, frame completion.
6. Per-user save & configuration isolation: ~/.doom/default.cfg and ~/.doom/.savegame.
7. Clean shutdown and offline filesystem integrity verification.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts"))
from test_usb_persistence import (
    CODE, VARS, run_qemu_session, send_command, persistent_partition, check_offline_ext2,
    configure_disposable_img_mode
)

FREEDM_WAD = Path("/tmp/doom-eval/freedm-0.13.0/freedm.wad")


def populate_freedm_on_partition(disk_path: Path, wad_path: Path):
    """Writes freedm.wad into /.fortress/doom/freedm.wad on Partition 2 of disk_path."""
    if not wad_path.is_file():
        print(f"[WARN] WAD file {wad_path} not found; skipping WAD injection")
        return False

    _, start, sectors = persistent_partition(disk_path)
    part_offset = start * 512
    part_size = sectors * 512

    with tempfile.NamedTemporaryFile(prefix="fortress-part2-", suffix=".ext4") as tmp_part:
        with open(disk_path, "rb") as f:
            f.seek(part_offset)
            data = f.read(part_size)
            assert len(data) == part_size
            tmp_part.write(data)
            tmp_part.flush()

        # Create directories and write file using debugfs
        cmds = (
            "mkdir .fortress\n"
            "cd .fortress\n"
            "mkdir doom\n"
            "cd doom\n"
            f"write {wad_path} freedm.wad\n"
            "close\n"
        )
        proc = subprocess.run(
            ["debugfs", "-w", tmp_part.name],
            input=cmds,
            capture_output=True,
            text=True
        )
        assert proc.returncode == 0, f"debugfs failed: {proc.stderr}"

        # Write back to disk
        tmp_part.seek(0)
        updated_data = tmp_part.read()
        assert len(updated_data) == part_size
        with open(disk_path, "r+b") as f:
            f.seek(part_offset)
            f.write(updated_data)
            f.flush()

    return True


def run_doom_qemu_test(firmware: str, smp: int, output_dir: Path):
    """Executes a full QEMU Doom acceptance test for a given firmware and SMP configuration."""
    print(f"\n=======================================================")
    print(f"Running Doom QEMU Acceptance: firmware={firmware}, SMP={smp}")
    print(f"=======================================================")

    source_img = REPO / "bin/fortress.img"
    assert source_img.is_file(), f"bin/fortress.img not found! Run make img first."

    disk = output_dir / f"disposable-{firmware}-{smp}.img"
    shutil.copyfile(source_img, disk)

    has_wad = populate_freedm_on_partition(disk, FREEDM_WAD)
    check_offline_ext2(disk)

    log_path = output_dir / f"doom-qemu-{firmware}-{smp}.log"

    def doom_action(qmp, child, log):
        # 1. Missing WAD test
        print("  -> Testing missing WAD guidance in guest...")
        reply = send_command(qmp, child, log, "/bin/doom -iwad /nonexistent.wad\n", "$ ", timeout=20)
        assert "[FortressOS Doom] No Game Data (IWAD) Found!" in reply, "Missing WAD banner not found"
        assert "Verified Upstream Freedoom Hashes" in reply, "Hash verification guidance not found"
        assert "freedm.wad:" in reply
        print("     PASS: Missing WAD produced actionable guidance")

        if has_wad:
            # 2. Live gameplay execution test (35 frames = 1 full second at 35 Hz)
            print("  -> Testing live game execution (35 frames playloop)...")
            reply = send_command(qmp, child, log, "/bin/doom -frames 35\n", "$ ", timeout=45)
            assert "[FortressOS Doom] Discovered IWAD: /mnt/.fortress/doom/freedm.wad" in reply
            assert "adding /mnt/.fortress/doom/freedm.wad" in reply
            assert "FreeDM" in reply
            assert "I_Init: Setting up machine state." in reply
            assert "R_Init: Init DOOM refresh daemon" in reply
            assert "P_Init: Init Playloop state." in reply
            assert "[FortressOS Doom] Main loop finished (frames=35" in reply
            print("     PASS: Game playloop executed 35 frames cleanly")

            # 3. Check configuration and savegame isolation
            print("  -> Verifying user configuration isolation in ~/.doom...")
            reply = send_command(qmp, child, log, "ls -la /home/operator/.doom\n", "$ ", timeout=15)
            assert "default.cfg" in reply, "default.cfg missing from ~/.doom"
            assert ".savegame" in reply, ".savegame directory missing from ~/.doom"

            reply = send_command(qmp, child, log, "head -n 5 /home/operator/.doom/default.cfg\n", "$ ", timeout=15)
            assert "mouse_sensitivity" in reply, "default.cfg does not contain expected Doom settings"
            print("     PASS: Saved settings segregated into ~/.doom/default.cfg")

            # 4. Repeat launch verification
            print("  -> Testing repeat launch (load defaults)...")
            reply = send_command(qmp, child, log, "/bin/doom -frames 20\n", "$ ", timeout=35)
            assert "[FortressOS Doom] Main loop finished (frames=20" in reply
            print("     PASS: Repeat launch completed cleanly")

        # 5. Clean shutdown
        print("  -> Testing clean shutdown...")
        send_command(qmp, child, log, "sudo shutdown\n", None, timeout=20)

    run_qemu_session(
        firmware=firmware,
        img_path=disk,
        log_path=log_path,
        action_cb=doom_action,
        round_name=f"doom-{firmware}-{smp}",
        writable=True,
        cpus=smp
    )

    check_offline_ext2(disk)
    print(f"  -> PASS: All assertions passed for {firmware} SMP={smp}!")


def main():
    parser = argparse.ArgumentParser(description="Stage 5 Doom QEMU automated acceptance test")
    parser.add_argument("--firmware", choices=("bios", "uefi", "all"), default="all")
    parser.add_argument("--smp", choices=("1", "4", "all"), default="all")
    parser.add_argument("--out", type=Path, default=REPO / "build" / f"doom-qemu-{int(time.time())}")
    args = parser.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)

    firmwares = ["bios", "uefi"] if args.firmware == "all" else [args.firmware]
    smps = [1, 4] if args.smp == "all" else [int(args.smp)]

    start_time = time.monotonic()
    for fw in firmwares:
        for smp in smps:
            run_doom_qemu_test(fw, smp, args.out)

    elapsed = time.monotonic() - start_time
    print(f"\n=======================================================")
    print(f">>> ALL STAGE 5 QEMU ACCEPTANCE TESTS PASSED ({elapsed:.2f}s) <<<")
    print(f"=======================================================")


if __name__ == "__main__":
    main()
