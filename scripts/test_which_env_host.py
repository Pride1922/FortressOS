#!/usr/bin/env python3
"""Run which and env host unit tests under ASan/UBSan."""
from pathlib import Path
import subprocess
import tempfile

REPO = Path(__file__).resolve().parent.parent

def test_which_env():
    with tempfile.TemporaryDirectory(prefix="fortress-which-env-host-") as tmp:
        exe = Path(tmp) / "which_env_host"
        cmd = [
            "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-no-pie",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-DTOOL_HOST_TEST",
            "-Isrc/include", "-Isrc/fs", "-Iuser/tools",
            "tests/which_env_host.c", "user/tools/common.c", "user/tools/which.c", "user/tools/env.c",
            "-o", str(exe)
        ]
        subprocess.run(cmd, cwd=REPO, check=True)
        subprocess.run([str(exe)], check=True)

if __name__ == "__main__":
    test_which_env()
