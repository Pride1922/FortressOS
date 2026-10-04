#!/usr/bin/env python3
"""Run diff host unit tests under ASan and UBSan."""
from pathlib import Path
import subprocess
import tempfile

REPO = Path(__file__).resolve().parent.parent


def test_diff():
    with tempfile.TemporaryDirectory(prefix="fortress-diff-host-") as tmp:
        exe = Path(tmp) / "diff_host"
        subprocess.run([
            "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-no-pie",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-DTOOL_HOST_TEST",
            "-Isrc/include", "-Isrc/fs", "-Iuser/tools",
            "tests/diff_host.c", "user/tools/common.c", "user/tools/diff.c",
            "-o", str(exe)
        ], cwd=REPO, check=True)
        subprocess.run([str(exe)], check=True, timeout=30)


if __name__ == "__main__":
    test_diff()
