#!/usr/bin/env python3
"""Piece 6A only: explicit test ISO, no data disks, bounded BIOS/UEFI boots.

Defaults to 2 GiB at 1/4/8 CPUs. Use --ram 32G --cpus 8 for all high probes.
This does not test the later 6B allocator stress or 6C/6D lifetime protocol.
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time

REPO = Path(__file__).resolve().parent.parent
CODE = Path("/usr/share/OVMF/OVMF_CODE_4M.fd")
VARS = Path("/usr/share/OVMF/OVMF_VARS_4M.fd")


def make_iso(temp):
    root = temp / "iso-root"
    boot = root / "boot"
    limine = boot / "limine"
    limine.mkdir(parents=True)
    efi = root / "EFI" / "BOOT"
    efi.mkdir(parents=True)
    for name in ("fortress.elf", "initramfs.tar"):
        shutil.copyfile(REPO / "bin" / name, boot / name)
    shutil.copyfile(REPO / "bin" / "initramfs.tar", root / "initramfs.tar")
    for name in ("limine-bios.sys", "limine-bios-cd.bin", "limine-uefi-cd.bin"):
        shutil.copyfile(REPO / "limine" / name, limine / name)
    shutil.copyfile(REPO / "limine" / "BOOTX64.EFI", efi / "BOOTX64.EFI")
    config = (
        "timeout: 0\n/FortressOS SMP memory boot test\n"
        "    protocol: limine\n"
        "    kernel_path: boot():/boot/fortress.elf\n"
        "    module_path: boot():/boot/initramfs.tar\n"
        "    kernel_cmdline: smp_memory_test=boot\n"
    )
    (boot / "limine.conf").write_text(config)
    (limine / "limine.conf").write_text(config)
    iso = temp / "memory-boot.iso"
    subprocess.run([
        "xorriso", "-as", "mkisofs", "-b", "boot/limine/limine-bios-cd.bin",
        "-no-emul-boot", "-boot-load-size", "4", "-boot-info-table",
        "--efi-boot", "boot/limine/limine-uefi-cd.bin", "-efi-boot-part",
        "--efi-boot-image", "--protective-msdos-label", str(root), "-o", str(iso),
    ], cwd=REPO, check=True, timeout=60)
    subprocess.run([str(REPO / "limine" / "limine"), "bios-install", str(iso)],
                   cwd=REPO, check=True, timeout=30)
    return iso


def run_case(iso, temp, firmware, cpus, ram, timeout):
    name = f"smp-memory-boot-{firmware}-{cpus}-{ram}"
    log = REPO / "build" / f"{name}.log"
    error = REPO / "build" / f"{name}.stderr"
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_text("")
    cmd = ["qemu-system-x86_64", "-accel", "tcg", "-M", "q35", "-m", ram,
           "-smp", str(cpus), "-display", "none", "-no-reboot", "-monitor", "none",
           "-serial", f"file:{log}", "-boot", "d", "-cdrom", str(iso)]
    drives = []
    if firmware == "uefi":
        if not CODE.is_file() or not VARS.is_file():
            raise RuntimeError("Paired OVMF 4M code/vars required")
        variables = temp / f"{name}-vars.fd"
        shutil.copyfile(VARS, variables)
        drives = [f"if=pflash,format=raw,unit=0,readonly=on,file={CODE}",
                  f"if=pflash,format=raw,unit=1,file={variables}"]
        for drive in drives:
            cmd += ["-drive", drive]
    # Final argv: only boot CD and paired firmware, no fixture/data disks.
    assert [cmd[i + 1] for i, arg in enumerate(cmd) if arg == "-drive"] == drives
    assert not any(arg in cmd for arg in ("-blockdev", "-hda", "-hdb", "-device"))
    started = time.monotonic()
    with error.open("wb") as err:
        child = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=err)
        try:
            while time.monotonic() - started < timeout:
                output = log.read_text(errors="replace")
                if "[FAIL] SMP memory 6A:" in output:
                    raise AssertionError(f"Memory boot assertion failed: {log}")
                if "fortress> " in output:
                    break
                if child.poll() is not None:
                    raise RuntimeError(f"QEMU exited early; see {log} and {error}")
                time.sleep(0.1)
            else:
                raise TimeoutError(f"No shell within {timeout}s: {log}")
        finally:
            if child.poll() is None:
                child.terminate()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait(timeout=5)
    output = log.read_text(errors="replace")
    milestones = [
        "[PASS] SMP memory 6A: boot ceiling, early unlock rejection, exact cleanup",
        "[PMM] High-memory allocation unlocked",
        "[PASS] SMP memory 6A: kernel CR3, high-memory unlock, exact cleanup",
        "[SMP] Boot memory readiness verified before AP release",
    ]
    positions = [output.index(marker) for marker in milestones]
    assert positions == sorted(positions), "Readiness ordering mismatch"
    for cpu in range(1, cpus):
        assert f"[ OK ] Per-CPU AP {cpu}: GS/TSS/IST1 fault/IST2 probe passed" in output
    if cpus > 1:
        assert f"All {cpus - 1} application processor(s) online" in output
    expected = {"256M": [], "2G": [1], "8G": [1, 2, 4], "32G": [1, 2, 4, 16, 30]}[ram]
    results = re.findall(r"HHDM full-page readback min=(0x[0-9a-fA-F]+) phys=(0x[0-9a-fA-F]+)", output)
    results = {int(minimum, 16): int(phys, 16) for minimum, phys in results}
    for gib in expected:
        minimum = gib * 1024**3
        assert minimum in results, f"Missing {gib} GiB probe: {log}"
        assert results[minimum] >= minimum and results[minimum] % 4096 == 0
    print(f"PASS 6A {firmware} CPUs={cpus} RAM={ram} elapsed={time.monotonic() - started:.1f}s log={log}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ram", choices=("256M", "2G", "8G", "32G"), default="2G")
    parser.add_argument("--cpus", nargs="+", type=int, choices=(1, 4, 8), default=[1, 4, 8])
    parser.add_argument("--firmware", nargs="+", choices=("bios", "uefi"), default=["bios", "uefi"])
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()
    if args.timeout < 1:
        parser.error("--timeout must be positive")
    with tempfile.TemporaryDirectory(prefix="fortress-memory-boot-") as directory:
        temp = Path(directory)
        iso = make_iso(temp)
        for firmware in args.firmware:
            for cpus in args.cpus:
                run_case(iso, temp, firmware, cpus, args.ram, args.timeout)


if __name__ == "__main__":
    main()
