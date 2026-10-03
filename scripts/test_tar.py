#!/usr/bin/env python3
"""QEMU Acceptance test for FortressOS tar tool on disposable ext2 fixture.
Verifies extraction into a fresh directory, tree integrity, rejection cases,
clean ACPI S5 shutdown, and offline e2fsck -fn validation (0 errors).
"""
import io
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tarfile
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
from test_nmi_transitions import connect


def make_tar_fixtures(dest_dir: Path):
    dest_dir.mkdir(parents=True, exist_ok=True)

    # 1. Valid archive
    good_tar = dest_dir / "good.tar"
    with tarfile.open(good_tar, "w", format=tarfile.USTAR_FORMAT) as tar:
        t1 = tarfile.TarInfo("file1.txt")
        b1 = b"FortressOS tar test content 1\n"
        t1.size = len(b1)
        t1.mode = 0o644
        tar.addfile(t1, io.BytesIO(b1))

        td = tarfile.TarInfo("dir1")
        td.type = tarfile.DIRTYPE
        td.mode = 0o755
        tar.addfile(td)

        t2 = tarfile.TarInfo("dir1/nested.txt")
        b2 = b"Nested content byte exact 2\n"
        t2.size = len(b2)
        t2.mode = 0o644
        tar.addfile(t2, io.BytesIO(b2))

    # 2. Symlink archive
    sym_tar = dest_dir / "symlink.tar"
    with tarfile.open(sym_tar, "w", format=tarfile.USTAR_FORMAT) as tar:
        t = tarfile.TarInfo("bad_symlink")
        t.type = tarfile.SYMTYPE
        t.linkname = "file1.txt"
        tar.addfile(t)

    # 3. Traversal archive
    trav_tar = dest_dir / "traversal.tar"
    with tarfile.open(trav_tar, "w", format=tarfile.USTAR_FORMAT) as tar:
        t = tarfile.TarInfo("../escape.txt")
        b = b"evil"
        t.size = len(b)
        tar.addfile(t, io.BytesIO(b))

    # 4. Truncated archive
    trunc_tar = dest_dir / "corrupt.tar"
    trunc_tar.write_bytes(good_tar.read_bytes()[:512])


def check_e2fsck(img_path):
    with tempfile.TemporaryDirectory(prefix="fortress-tar-e2fsck-") as tmp:
        part_file = Path(tmp) / "part1.ext2"
        # Partition 1 starts at sector 2048 (1 MiB) with 8192 sectors (4 MiB)
        part_offset = 2048 * 512
        part_size = 8192 * 512
        with open(img_path, "rb") as f:
            f.seek(part_offset)
            data = f.read(part_size)
            assert len(data) == part_size, "Truncated partition data"
            part_file.write_bytes(data)

        res = subprocess.run(["e2fsck", "-fn", str(part_file)], capture_output=True, text=True)
        print("      - e2fsck output:\n" + "\n".join("        " + l for l in res.stdout.strip().splitlines()))
        assert res.returncode == 0, f"e2fsck reported errors (code {res.returncode}):\n{res.stdout}\n{res.stderr}"


def main():
    print("[QEMU TAR ACCEPTANCE] Starting QEMU tar acceptance test...", flush=True)

    build_dir = ROOT / "build"
    build_dir.mkdir(exist_ok=True)
    orig_img = build_dir / "nvme_gpt.img"
    if not orig_img.exists():
        subprocess.run(["make", "nvme-gpt-disk"], cwd=ROOT, check=True)

    with tempfile.TemporaryDirectory(prefix="fortress-tar-qemu-") as tmp:
        tmp_path = Path(tmp)
        test_img = tmp_path / "test_nvme.img"
        shutil.copyfile(orig_img, test_img)

        # Build custom ISO with test fixtures in initramfs
        iso_root = tmp_path / "iso_root"
        shutil.copytree(build_dir / "iso_root", iso_root)

        fixtures_dir = tmp_path / "fixtures"
        make_tar_fixtures(fixtures_dir)

        # Add fixtures to boot/initramfs.tar
        initramfs_path = iso_root / "boot/initramfs.tar"
        with tarfile.open(initramfs_path, "a", format=tarfile.USTAR_FORMAT) as archive:
            td = tarfile.TarInfo("tests")
            td.type = tarfile.DIRTYPE
            td.mode = 0o755
            archive.addfile(td)

            for fix_name in ("good.tar", "symlink.tar", "traversal.tar", "corrupt.tar"):
                fpath = fixtures_dir / fix_name
                info = tarfile.TarInfo(f"tests/{fix_name}")
                info.size = fpath.stat().st_size
                info.mode = 0o644
                with open(fpath, "rb") as f:
                    archive.addfile(info, f)

        shutil.copyfile(initramfs_path, iso_root / "initramfs.tar")

        test_iso = tmp_path / "tar_test.iso"
        subprocess.run([
            "xorriso", "-as", "mkisofs", "-b", "boot/limine/limine-bios-cd.bin",
            "-no-emul-boot", "-boot-load-size", "4", "-boot-info-table",
            "--efi-boot", "boot/limine/limine-uefi-cd.bin", "-efi-boot-part",
            "--efi-boot-image", "--protective-msdos-label", str(iso_root), "-o", str(test_iso)
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run([str(ROOT / "limine/limine"), "bios-install", str(test_iso)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        uart_path = tmp_path / "uart"
        log_path = tmp_path / "serial.log"

        qemu_cmd = [
            "qemu-system-x86_64", "-M", "q35", "-m", "2G", "-display", "none",
            "-no-reboot", "-monitor", "none", "-boot", "d", "-cdrom", str(test_iso),
            "-chardev", f"socket,id=uart,path={uart_path},server=on,wait=off,logfile={log_path}",
            "-serial", "chardev:uart",
            "-drive", f"file={test_img},if=none,id=nvm0,format=raw,snapshot=off",
            "-device", "nvme,serial=fortress0,drive=nvm0",
            "-fw_cfg", "name=opt/fortress/write_test,string=1"
        ]

        child = subprocess.Popen(qemu_cmd, cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            uart = connect(uart_path)
            uart.settimeout(0.2)
            stop_drain = threading.Event()

            def drain():
                while not stop_drain.is_set():
                    try:
                        if not uart.recv(65536): return
                    except socket.timeout:
                        continue

            drain_thread = threading.Thread(target=drain, daemon=True)
            drain_thread.start()

            def output():
                if not log_path.exists(): return ""
                return log_path.read_text(errors="replace").replace("\r", "")

            def wait_for(pattern, timeout=45):
                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline:
                    txt = output()
                    if pattern in txt:
                        time.sleep(0.05)
                        return txt
                    assert child.poll() is None, child.stderr.read().decode()
                    time.sleep(0.05)
                raise AssertionError(f"Timeout waiting for {pattern!r} in serial log:\n{output()[-1500:]}")

            def send_str(s):
                for b in s.encode():
                    uart.send(bytes([b]))
                    time.sleep(0.005)

            # Wait for shell prompt
            wait_for("fortress:/ $ ")
            print("  [OK] Fortress shell prompt reached", flush=True)

            commands = [
                ("tar --help\n", "Usage: tar"),
                ("tar -tf /tests/good.tar\n", "dir1/nested.txt"),
                ("tar -xf /tests/good.tar -C /mnt/extracted\n", "fortress:/ $ "),
                ("cat /mnt/extracted/file1.txt\n", "FortressOS tar test content 1"),
                ("cat /mnt/extracted/dir1/nested.txt\n", "Nested content byte exact 2"),
                ("ls /mnt/extracted\n", "dir1"),
                # Rejection: existing destination
                ("tar -xf /tests/good.tar -C /mnt/extracted\n", "destination already exists"),
                # Rejection: symlink
                ("tar -xf /tests/symlink.tar -C /mnt/symlink_dir\n", "symlinks not supported"),
                # Rejection: traversal
                ("tar -xf /tests/traversal.tar -C /mnt/trav_dir\n", "path traversal not allowed"),
                # Rejection: corrupt
                ("tar -xf /tests/corrupt.tar -C /mnt/corrupt_dir\n", "archive truncated"),
                ("sync\n", "fortress:/ $ "),
                ("shutdown\n", None)
            ]

            for cmd, expect in commands:
                send_str(cmd)
                if expect:
                    wait_for(expect)

            exit_code = child.wait(timeout=20)
            assert exit_code == 0, f"QEMU exited with code {exit_code}"
            stop_drain.set()
            print("  [OK] QEMU cleanly powered off after extraction & rejection tests", flush=True)

            # Verify ext2 integrity with e2fsck -fn
            print("  [AUDIT] Running offline host e2fsck -fn on persisted partition...", flush=True)
            check_e2fsck(test_img)
            print("  [PASS] Offline ext2 filesystem verified clean with 0 errors.", flush=True)

        finally:
            if child.poll() is None:
                child.kill()
                child.wait()

    print("PASS: QEMU tar acceptance test complete.", flush=True)


if __name__ == "__main__":
    main()
