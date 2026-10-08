#!/usr/bin/env python3
import subprocess
import time
import re
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

def test_step1a():
    log = REPO / "build" / "test_step1a.log"
    if log.exists():
        log.unlink()
    cmd = [
        "qemu-system-x86_64", "-accel", "tcg", "-M", "q35", "-m", "2G",
        "-smp", "4", "-display", "none", "-no-reboot", "-monitor", "none",
        "-serial", f"file:{log}", "-boot", "d", "-cdrom", "bin/fortress.iso"
    ]
    proc = subprocess.Popen(cmd, cwd=REPO)
    deadline = time.monotonic() + 30
    found = False
    while time.monotonic() < deadline:
        if log.exists():
            text = log.read_text(errors="replace")
            if "[ OK ] SMP Step 1A (cpus_allowed mask) complete." in text:
                found = True
                break
        time.sleep(0.2)
    proc.terminate()
    proc.wait()

    assert found, "Step 1A completion not found in log!"
    text = log.read_text(errors="replace")
    for line in text.splitlines():
        if "SMP Step 1A" in line or "Sub-step A" in line:
            print(line)

if __name__ == "__main__":
    test_step1a()
