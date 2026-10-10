#!/usr/bin/env python3
"""ASan/UBSan host test for builtin_exec and builtin_is_child_safe.

Compiles runner_host.c against the actual builtin_exec.c and builtins.c shared
modules under AddressSanitizer and UBSan.  io.c is excluded because runner_host.c
provides its own mocks for write_bytes_fd, puts, puts_err, call, length and equal.
No kernel or QEMU required.
"""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="fortress-runner-host-") as tmp:
    exe = str(Path(tmp) / "runner_host")
    subprocess.run([
        "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
        "-DPERMISSIONS_CLI_HOST_TEST",
        "-Isrc/include", "-Isrc/fs", "-Iuser/shell",
        "tests/runner_host.c",
        "user/shell/builtin_exec.c",
        "user/tools/userdb.c", "user/tools/digest.c",
        "user/shell/builtins.c",
        "-o", exe,
    ], cwd=repo, check=True)
    subprocess.run([exe], check=True, timeout=30)
    print("PASS runner handlers, including ls -l database names and full-width numeric fallback")
