#!/usr/bin/env python3
"""Run grep host unit tests under ASan and UBSan."""
from pathlib import Path
import subprocess
import tempfile

REPO = Path(__file__).resolve().parent.parent


def test_grep():
    with tempfile.TemporaryDirectory(prefix="fortress-grep-host-") as tmp:
        exe = Path(tmp) / "grep_host"
        cmd = [
            "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-no-pie",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-DTOOL_HOST_TEST",
            "-Isrc/include", "-Isrc/fs", "-Iuser/tools",
            "tests/grep_host.c", "user/tools/common.c", "user/tools/grep.c",
            "-o", str(exe)
        ]
        subprocess.run(cmd, cwd=REPO, check=True)
        res = subprocess.run([str(exe)], cwd=REPO, check=True, capture_output=True, text=True)
        print(res.stdout)


if __name__ == "__main__":
    test_grep()
