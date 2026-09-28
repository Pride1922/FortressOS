"""Actual terminal/group logic, host IRQ/scheduler/device adapters only."""
from pathlib import Path
import os
import subprocess
import tempfile

repo = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="fortress-terminal-") as tmp:
    exe = Path(tmp) / "test"
    subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-pthread",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
                    "-DTEST_SMP_MEMORY", "-Isrc/include", "-Isrc/kernel", "-Isrc/drivers",
                    "-Isrc/arch/x86_64", "-Isrc/fs", "tests/s8_terminal_host.c", "-o", str(exe)],
                   cwd=repo, check=True, timeout=60)
    subprocess.run([str(exe)], check=True, timeout=60,
                   env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1", "UBSAN_OPTIONS": "halt_on_error=1"})
