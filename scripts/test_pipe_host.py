#!/usr/bin/env python3
"""Real pipe/VFS/sys_pipe with host adapters; no SMP/IRQ acceptance claim."""
import os
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="fortress-pipe-") as tmp:
    exe = Path(tmp) / "pipe_host"
    subprocess.run([
        "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-Itests/pipe_host", "-Itests/host", "-Isrc/include", "-Isrc/drivers", "-Isrc/mm",
        "-Isrc/fs", "-Isrc/kernel", "-Isrc/arch/x86_64",
        "tests/pipe_host.c", "-o", str(exe),
    ], cwd=repo, check=True, timeout=60)
    subprocess.run([str(exe)], check=True, timeout=60,
                   env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1",
                        "UBSAN_OPTIONS": "halt_on_error=1"})
