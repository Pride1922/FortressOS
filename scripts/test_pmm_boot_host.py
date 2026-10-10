#!/usr/bin/env python3
"""6A boot policy only; single-threaded shims, host ASan/UBSan, no SMP claim."""
from pathlib import Path
import subprocess
import tempfile

REPO = Path(__file__).resolve().parent.parent


def main():
    with tempfile.TemporaryDirectory(prefix="fortress-pmm-boot-") as temp:
        exe = str(Path(temp) / "pmm-boot")
        subprocess.run([
            "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
            "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
            # Project/shim headers use quotes; hosted <string.h> must resolve
            # to libc rather than src/include/string.h (which has no strcpy).
            "-iquote", "tests/host", "-iquote", "src/include",
            "-iquote", "src/drivers", "-iquote", "src/mm",
            "-iquote", "src/arch/x86_64", "-iquote", "src/kernel",
            "-iquote", "src/fs",
            "tests/pmm_boot_host.c", "src/mm/pmm.c", "src/mm/memory_boot_test.c",
            "-o", exe,
        ], cwd=REPO, check=True, timeout=60)
        subprocess.run([exe], cwd=REPO, check=True, timeout=60)


if __name__ == "__main__":
    main()
