#!/usr/bin/env python3
"""Run builtin_exec host unit tests (echo -n/-e, printf %b, dmesg -a) under ASan/UBSan."""
from pathlib import Path
import subprocess
import tempfile

REPO = Path(__file__).resolve().parent.parent

def test_builtin_exec():
    with tempfile.TemporaryDirectory(prefix="fortress-builtin-exec-host-") as tmp:
        exe = Path(tmp) / "builtin_exec_host"
        cmd = [
            "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-no-pie",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-builtin-puts",
            "-Isrc/include", "-Isrc/fs", "-Iuser", "-Iuser/shell",
            "tests/builtin_exec_host.c", "user/shell/builtin_exec.c", "user/shell/builtins.c",
            "-o", str(exe)
        ]
        subprocess.run(cmd, cwd=REPO, check=True)
        res = subprocess.run([str(exe)], check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        print(res.stdout)

if __name__ == "__main__":
    test_builtin_exec()
