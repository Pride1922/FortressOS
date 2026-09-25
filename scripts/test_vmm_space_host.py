#!/usr/bin/env python3
"""SMP Piece 6D Step 1: Host ASan/UBSan test for VMM space lifecycle and registry."""
from pathlib import Path
import subprocess
import tempfile

REPO = Path(__file__).resolve().parent.parent


def main():
    with tempfile.TemporaryDirectory(prefix="fortress-vmm-space-") as temp:
        exe = str(Path(temp) / "vmm-space-test")
        subprocess.run([
            "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
            "-DTEST_VMM_HOST",
            "-iquote", "tests/host", "-iquote", "src/include",
            "-iquote", "src/drivers", "-iquote", "src/mm",
            "-iquote", "src/kernel", "-iquote", "src/arch/x86_64",
            "tests/vmm_space_host.c", "src/mm/vmm.c",
            "-o", exe,
        ], cwd=REPO, check=True, timeout=60)
        subprocess.run([exe], cwd=REPO, check=True, timeout=60)


if __name__ == "__main__":
    main()
