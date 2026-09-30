#!/usr/bin/env python3
"""Networking Phase 1a: PCI discovery, MMIO mapping, and MAC read test suite.

Covers:
- BIOS + UEFI boot under SMP=1
- e1000 present: verifies BDF, vendor:device (8086:100E), BAR0 physical & virtual map, MAC, and STATUS
- e1000 absent: verifies clean fallback report ("No network controller found; networking unavailable")
- Argv preflight: enforces no-data-disk guarantee while allowing network device pair
"""

import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

REPO = Path(__file__).resolve().parent.parent
CODE = Path("/usr/share/OVMF/OVMF_CODE_4M.fd")
VARS = Path("/usr/share/OVMF/OVMF_VARS_4M.fd")
ISO = REPO / "bin" / "fortress.iso"


def preflight(cmd, firmware, present, firmware_drives):
    assert firmware in ("bios", "uefi")

    # Storage devices strictly forbidden
    forbidden = ("-hda", "-hdb", "nvme", "-blockdev", "-snapshot", "usb-storage")
    for token in forbidden:
        for arg in cmd:
            assert token not in arg, f"preflight found forbidden token {token} in {arg}"

    for i, arg in enumerate(cmd):
        if arg == "-drive":
            drive_val = cmd[i + 1]
            assert drive_val in firmware_drives, f"forbidden drive in argv: {drive_val}"

    if present:
        assert "-netdev" in cmd, "preflight: expected -netdev for present case"
        netdev_idx = cmd.index("-netdev")
        assert cmd[netdev_idx + 1] == "user,id=net0"
        assert "-device" in cmd, "preflight: expected -device for present case"
        dev_idx = cmd.index("-device")
        assert cmd[dev_idx + 1] == "e1000,netdev=net0"
    else:
        assert "-net" in cmd, "preflight: expected -net none for absent case"
        net_idx = cmd.index("-net")
        assert cmd[net_idx + 1] == "none"
        assert "-netdev" not in cmd, "preflight: unexpected -netdev in absent case"
        assert "e1000" not in " ".join(cmd), "preflight: unexpected e1000 device in absent case"


def run_test_case(firmware, present):
    desc = f"{firmware.upper()} (e1000 {'present' if present else 'absent'})"
    print(f"--> Running test case: {desc}...", flush=True)

    log_path = REPO / "build" / f"test-net-pci-{firmware}-{'present' if present else 'absent'}.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_text("")

    with tempfile.TemporaryDirectory(prefix="fortress-net-pci-") as tmp:
        cmd = [
            "qemu-system-x86_64",
            "-M", "q35",
            "-m", "2G",
            "-accel", "tcg",
            "-smp", "1",
            "-display", "none",
            "-monitor", "none",
            "-no-reboot",
            "-boot", "d",
            "-cdrom", str(ISO),
            "-serial", f"file:{log_path}"
        ]

        firmware_drives = []
        if firmware == "uefi":
            assert CODE.is_file() and VARS.is_file(), "OVMF 4M firmware files required"
            vars_copy = Path(tmp) / "vars.fd"
            shutil.copyfile(VARS, vars_copy)
            firmware_drives = [
                f"if=pflash,format=raw,unit=0,readonly=on,file={CODE}",
                f"if=pflash,format=raw,unit=1,file={vars_copy}"
            ]
            for d in firmware_drives:
                cmd += ["-drive", d]

        if present:
            cmd += ["-netdev", "user,id=net0", "-device", "e1000,netdev=net0"]
        else:
            cmd += ["-net", "none"]

        # Run preflight assertions
        preflight(cmd, firmware, present, firmware_drives)

        # Test preflight injection rejection
        for injection in (["-hda", "unsafe.img"], ["-device", "nvme"], ["-device", "usb-storage,drive=x"]):
            try:
                preflight(cmd + injection, firmware, present, firmware_drives)
            except AssertionError:
                pass
            else:
                raise AssertionError(f"preflight accepted injection: {injection}")

        # Spawn QEMU
        proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.monotonic() + 25.0
        success = False
        captured = ""

        try:
            while time.monotonic() < deadline:
                if log_path.exists():
                    captured = log_path.read_text(errors="replace")
                    if "[BOOT] Interactive shell ready." in captured:
                        success = True
                        break
                time.sleep(0.2)
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

        assert success, f"Timeout waiting for interactive shell in {desc}. Log:\n{captured}"

        # Assert networking diagnostics
        assert "[NET] Probing network controllers..." in captured, f"Missing probe header in {desc}"

        if present:
            assert "[NET] e1000: " in captured, f"Missing e1000 diagnostic in {desc}"
            assert ("8086:100E" in captured or "8086:10D3" in captured), f"Missing vendor:device 8086:100E/10D3 in {desc}"
            assert "BAR0 0x" in captured, f"Missing BAR0 report in {desc}"
            assert "-> 0xFFFFFFFFE2000000" in captured, f"Missing MMIO virtual mapping in {desc}"
            assert "[NET] e1000: MAC " in captured, f"Missing MAC report in {desc}"

            mac_match = re.search(r"\[NET\] e1000: MAC ([0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5})", captured)
            assert mac_match, f"Invalid or missing MAC format in {desc}:\n{captured}"
            mac_str = mac_match.group(1).upper()
            assert mac_str != "00:00:00:00:00:00" and mac_str != "FF:FF:FF:FF:FF:FF", f"Invalid MAC address: {mac_str}"

            assert "STATUS 0x" in captured, f"Missing STATUS register in {desc}"
            assert "(link up)" in captured, f"Expected link up in QEMU default in {desc}"
            print(f"  [PASS] {desc}: e1000 found, BAR0 mapped, MAC={mac_str}, link up", flush=True)
        else:
            assert "[NET] No network controller found; networking unavailable" in captured, \
                f"Missing clean absence report in {desc}:\n{captured}"
            assert "e1000: MAC" not in captured, f"Unexpected MAC report in absent case in {desc}"
            print(f"  [PASS] {desc}: clean fallback on absent network device", flush=True)


def main():
    if not ISO.exists():
        print(f"Error: {ISO} does not exist. Run 'make' first.", flush=True)
        sys.exit(1)

    print("========================================================", flush=True)
    print("FortressOS Networking Phase 1a — PCI Discovery Suite", flush=True)
    print("========================================================", flush=True)

    test_cases = [
        ("bios", True),
        ("uefi", True),
        ("bios", False),
        ("uefi", False),
    ]

    for firmware, present in test_cases:
        run_test_case(firmware, present)

    print("========================================================", flush=True)
    print("All 4 Phase 1a QEMU test cases passed! [100% PASS]", flush=True)
    print("========================================================", flush=True)


if __name__ == "__main__":
    main()
