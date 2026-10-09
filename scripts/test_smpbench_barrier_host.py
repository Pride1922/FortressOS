#!/usr/bin/env python3
"""Actual benchmark with syscall adapters under ASan/UBSan; no SMP claim."""
import subprocess
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="fortress-barrier-") as temporary:
    executable = Path(temporary) / "barrier"
    subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
                    "-no-pie", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-Isrc/include", "-Isrc/fs", "-Iuser/tools",
                    "tests/smpbench_barrier_host.c", "-o", str(executable)], cwd=REPO, check=True)
    subprocess.run([str(executable)], check=True, timeout=30)
