#!/usr/bin/env python3
"""Regression for >2 GiB RAM and absent COM1, observing progress with GDB/QMP."""
from pathlib import Path
import shutil
import subprocess
import tempfile
from test_nmi_transitions import REPO, Remote, QMP, symbols

sym = symbols()
with tempfile.TemporaryDirectory(prefix="fortress-no-uart-") as temp:
    temp = Path(temp)
    shutil.copyfile("/usr/share/OVMF/OVMF_VARS_4M.fd", temp / "vars.fd")
    cmd = ["qemu-system-x86_64", "-M", "q35", "-m", "8G", "-accel", "tcg", "-smp", "1",
           "-display", "none", "-serial", "none", "-monitor", "none", "-no-reboot", "-S",
           "-chardev", f"socket,path={temp}/gdb,server=on,wait=off,id=gdb0", "-gdb", "chardev:gdb0",
           "-qmp", f"unix:{temp}/qmp,server=on,wait=off", "-boot", "d", "-cdrom", "bin/fortress.iso",
           "-drive", "if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
           "-drive", f"if=pflash,format=raw,unit=1,file={temp}/vars.fd"]
    child = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    remote = qmp = None
    try:
        qmp = QMP(temp / "qmp")
        remote = Remote(temp / "gdb")
        remote.request("qSupported")
        remote.request("qXfer:features:read:target.xml:0,fff")
        # Reaching PCI discovery proves PMM/VMM, scheduling, Ring 3 and VFS
        # suites completed with no COM1 and RAM above the managed 2 GiB limit.
        remote.resume_to(sym["test_phase9a_pci_discovery"])
        total_pages = int.from_bytes(remote.memory(sym["total_pages"], 8), "little")
        assert total_pages == (2 * 1024 * 1024 * 1024) // 4096
        assert remote.memory(sym["serial_available"], 1) == b"\x00"
        screenshot = REPO / "build" / "boot-8g-no-uart.png"
        qmp.execute("screendump", {"filename": str(screenshot), "format": "png"})
        assert screenshot.exists() and screenshot.stat().st_size > 1024
        print(f"PASS UEFI 8 GiB, COM1 absent: reached PCI discovery; PMM capped safely; screen {screenshot}")
    finally:
        if remote:
            remote.sock.close()
        if qmp:
            qmp.stream.close()
            qmp.sock.close()
        child.terminate()
        try:
            child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait()
