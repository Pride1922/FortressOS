#!/usr/bin/env python3
"""Run ps host unit tests under ASan/UBSan."""
from pathlib import Path
import subprocess
import tempfile

REPO = Path(__file__).resolve().parent.parent

def test_ps():
    with tempfile.TemporaryDirectory(prefix="fortress-ps-host-") as tmp:
        exe = Path(tmp) / "ps_host"
        cmd = [
            "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-no-pie",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-DPS_HOST_TEST",
            "-Isrc/include", "-Iuser",
            "tests/ps_host.c", "user/ps.c",
            "-o", str(exe)
        ]
        subprocess.run(cmd, cwd=REPO, check=True)
        subprocess.run([str(exe)], check=True)

if __name__ == "__main__":
    test_ps()
