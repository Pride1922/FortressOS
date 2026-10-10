#!/usr/bin/env python3
"""Actual heap under ASan/UBSan; synthetic PMM/VMM, single-threaded locks."""
import argparse
from pathlib import Path
import subprocess
import tempfile

REPO = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", choices=("all", "shrink", "overflow"), default="all")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="fortress-heap-memory-") as directory:
        exe = str(Path(directory) / "heap-memory")
        subprocess.run([
            "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-O1",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
            "-iquote", "tests/heap_host", "-iquote", "src/include",
            "-iquote", "src/mm", "-iquote", "src/drivers",
            "tests/heap_memory_host.c", "-o", exe,
        ], cwd=REPO, check=True, timeout=60)
        subprocess.run([exe, args.case], cwd=REPO, check=True, timeout=60)


if __name__ == "__main__":
    main()
