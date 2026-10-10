#!/usr/bin/env python3
"""Actual PMM accounting corruption and fragmented OOM, ASan/UBSan."""
from pathlib import Path
import subprocess
import tempfile

REPO = Path(__file__).resolve().parent.parent


def main():
    with tempfile.TemporaryDirectory(prefix="fortress-pmm-audit-") as directory:
        exe = str(Path(directory) / "pmm-audit")
        subprocess.run([
            "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-O1",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
            "-iquote", "tests/heap_host", "-iquote", "src/include",
            "-iquote", "src/mm", "-iquote", "src/drivers",
            "tests/pmm_audit_host.c", "-o", exe,
        ], cwd=REPO, check=True, timeout=60)
        subprocess.run([exe], cwd=REPO, check=True, timeout=60)


if __name__ == "__main__":
    main()
