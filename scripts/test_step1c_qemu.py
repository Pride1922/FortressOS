#!/usr/bin/env python3
import subprocess
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

def test_step1c():
    log = REPO / "build" / "test_step1c.log"
    if log.exists():
        log.unlink()
    cmd = [
        "qemu-system-x86_64", "-accel", "tcg", "-M", "q35", "-m", "2G",
        "-smp", "4", "-display", "none", "-no-reboot", "-monitor", "none",
        "-serial", f"file:{log}", "-boot", "d", "-cdrom", "bin/fortress.iso"
    ]
    proc = subprocess.Popen(cmd, cwd=REPO)
    deadline = time.monotonic() + 35
    found = False
    while time.monotonic() < deadline:
        if log.exists():
            text = log.read_text(errors="replace")
            if "[ OK ] SMP Step 1C (IPI kick on wake & HLT idle) complete." in text:
                found = True
                break
        time.sleep(0.3)
    proc.terminate()
    proc.wait()

    assert found, "Step 1C completion not found in log!"
    text = log.read_text(errors="replace")
    for line in text.splitlines():
        if "SMP Step 1C" in line or "Sub-step C" in line or "IPI" in line or "Worker" in line:
            print(line)

if __name__ == "__main__":
    test_step1c()
