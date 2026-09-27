#!/usr/bin/env python3
"""Compile real tool modules with ASan/UBSan and syscall mocks; no guest claims."""
from pathlib import Path
import argparse
import subprocess
import tempfile

REPO = Path(__file__).resolve().parent.parent


def build(exe):
    subprocess.run([
        "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-no-pie",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-DTOOL_HOST_TEST",
        "-Isrc/include", "-Isrc/fs", "-Iuser/tools", "tests/stream_tools_host.c",
        *[f"user/tools/{name}.c" for name in ("common", "cat", "head", "tail", "wc")],
        "-o", str(exe)], cwd=REPO, check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-only", type=Path, help="compile to this path without running tests")
    args = parser.parse_args()
    if args.build_only:
        build(args.build_only)
    else:
        with tempfile.TemporaryDirectory(prefix="fortress-stream-tools-") as tmp:
            exe = Path(tmp) / "tools"
            build(exe)
            subprocess.run([str(exe)], check=True, timeout=30)
