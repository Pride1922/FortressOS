#!/usr/bin/env python3
"""Bounded BIOS/UEFI pipe ABI probe; disposable ISO, no data disks."""
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import time

REPO = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="fortress-pipe-abi-") as name:
    tmp = Path(name)
    root = tmp / "iso"
    shutil.copytree(REPO / "build/iso_root", root)
    archive = tmp / "initramfs.tar"
    with tarfile.open(REPO / "bin/initramfs.tar") as src, \
            tarfile.open(archive, "w", format=tarfile.USTAR_FORMAT) as dst:
        for member in src.getmembers():
            if member.name.lstrip("./") != "bin/shell":
                dst.addfile(member, src.extractfile(member) if member.isfile() else None)
        dst.add(REPO / "build/pipe_user.elf", arcname="bin/shell")
    for path in (root / "boot/initramfs.tar", root / "initramfs.tar"):
        shutil.copyfile(archive, path)
    config = ("timeout: 0\n/FortressOS Pipe ABI Test\n    protocol: limine\n"
              "    kernel_path: boot():/boot/fortress.elf\n"
              "    module_path: boot():/boot/initramfs.tar\n")
    for path in (root / "limine.conf", root / "boot/limine.conf", root / "boot/limine/limine.conf"):
        path.write_text(config)
    iso = tmp / "pipe.iso"
    subprocess.run(["xorriso", "-as", "mkisofs", "-b", "boot/limine/limine-bios-cd.bin",
                    "-no-emul-boot", "-boot-load-size", "4", "-boot-info-table",
                    "--efi-boot", "boot/limine/limine-uefi-cd.bin", "-efi-boot-part",
                    "--efi-boot-image", "--protective-msdos-label", str(root), "-o", str(iso)],
                   check=True, timeout=60, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run([str(REPO / "limine/limine"), "bios-install", str(iso)],
                   check=True, timeout=30, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for mode in ("bios", "uefi"):
        log = REPO / "build" / f"pipe-{mode}.log"
        log.unlink(missing_ok=True)
        cmd = ["qemu-system-x86_64", "-accel", "tcg", "-M", "q35", "-m", "2G",
               "-smp", "1", "-display", "none", "-monitor", "none", "-no-reboot",
               "-boot", "d", "-cdrom", str(iso), "-serial", f"file:{log}",
               "-fw_cfg", "name=opt/fortress/pipe_test,string=1"]
        if mode == "uefi":
            variables = tmp / "vars.fd"
            shutil.copyfile("/usr/share/OVMF/OVMF_VARS_4M.fd", variables)
            cmd += ["-drive", "if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
                    "-drive", f"if=pflash,format=raw,unit=1,file={variables}"]
        with (tmp / f"{mode}.stderr").open("wb") as stderr:
            child = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=stderr)
            try:
                deadline = time.monotonic() + 90
                while time.monotonic() < deadline:
                    text = log.read_text(errors="replace") if log.exists() else ""
                    assert "PIPE USER FAIL" not in text and "PIPE KERNEL FAIL" not in text, text[-2000:]
                    if "PIPE USER PASS" in text:
                        for marker in (
                            "PIPE KERNEL PASS blocked read and writer-close EOF",
                            "PIPE KERNEL PASS 256 KiB, 32 observed writer block/wake cycles",
                            "PIPE KERNEL PASS reader-close EPIPE, blocked and immediate",
                            "PIPE KERNEL PASS CLOEXEC sweep and heap/stack reclamation",
                        ):
                            assert marker in text, marker
                        print(f"PASS pipe ABI: {mode}, 1 CPU, no data disks", flush=True)
                        break
                    assert child.poll() is None, text[-2000:]
                    time.sleep(0.1)
                else:
                    raise AssertionError(f"Pipe ABI timeout: {log}\n{text[-2000:]}")
            finally:
                child.terminate()
                try:
                    child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=5)
