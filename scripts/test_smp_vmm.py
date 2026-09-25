#!/usr/bin/env python3
"""SMP Piece 6D: Address space lifetime and deferred reaping runner.

Runs address-space lifetime test across BIOS/UEFI and 1/4/8 CPUs with -m 2G.
Verifies:
1. Explicit VMM_ERR_BUSY deferral, queueing, and deferred drainage.
2. 100 user process spawn/exit cycles across cores.
3. Zero deferred destructions remaining at the end.
4. Exact table-frame counter equality (matches baseline).
5. Exact physical frame equality (zero frame leaks).
6. Clean stack slot reclamation.
7. Clean boot to interactive shell prompt.
"""
import argparse
from pathlib import Path
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
        "timeout: 0\n/FortressOS SMP VMM lifecycle test\n"
        "    protocol: limine\n"
        "    kernel_path: boot():/boot/fortress.elf\n"
        "    module_path: boot():/boot/initramfs.tar\n"
        "    kernel_cmdline: smp_memory_test=vmm_lifecycle\n"
    )
    (boot / "limine.conf").write_text(config)
    (limine / "limine.conf").write_text(config)
    iso = temp / "vmm-lifecycle.iso"
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
    name = f"smp-vmm-lifecycle-{firmware}-{cpus}-{ram}"
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
    assert [cmd[i + 1] for i, arg in enumerate(cmd) if arg == "-drive"] == drives
    assert not any(arg in cmd for arg in ("-blockdev", "-hda", "-hdb", "-device"))

    started = time.monotonic()
    with error.open("wb") as err:
        child = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=err)
        try:
            while time.monotonic() - started < timeout:
                output = log.read_text(errors="replace")
                if "[FAIL] SMP memory" in output:
                    raise AssertionError(f"VMM lifecycle assertion failed: {log}")
                if "fortress> " in output:
                    break
                if child.poll() is not None:
                    raise RuntimeError(f"QEMU exited early; see {log} and {error}")
                time.sleep(0.2)
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
    required_milestones = [
        "SMP Piece 6D: Address-Space Lifetime & Deferred Reaping",
        "[PASS] SMP memory 6D: VMM_ERR_BUSY deferral, queueing, and drainage verified",
        f"[TEST 2] SMP memory 6D: Running 100 spawn/exit cycles across {cpus} CPU(s)...",
        "[PASS] SMP memory 6D: zero deferred destructions remaining (all drained)",
        "[PASS] SMP memory 6D: exact table-frame counter equality (matches baseline)",
        "[PASS] SMP memory 6D: exact physical frame equality (zero frame leaks)",
        "[PASS] SMP memory 6D: all worker stacks reaped cleanly",
        "[ OK ] SMP Piece 6D (Address-Space Lifetime & Deferred Reaping) complete.",
    ]
    for m in required_milestones:
        assert m in output, f"Missing milestone '{m}' in {log}"

    elapsed = time.monotonic() - started
    print(f"PASS 6D {firmware} CPUs={cpus} RAM={ram} elapsed={elapsed:.1f}s log={log}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ram", default="2G")
    parser.add_argument("--cpus", nargs="+", type=int, choices=(1, 4, 8), default=[1, 4, 8])
    parser.add_argument("--firmware", nargs="+", choices=("bios", "uefi"), default=["bios", "uefi"])
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args()
    if args.timeout < 1:
        parser.error("--timeout must be positive")
    with tempfile.TemporaryDirectory(prefix="fortress-vmm-lifecycle-") as directory:
        temp = Path(directory)
        iso = make_iso(temp)
        for firmware in args.firmware:
            for cpus in args.cpus:
                run_case(iso, temp, firmware, cpus, args.ram, args.timeout)


if __name__ == "__main__":
    main()
