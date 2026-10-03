#!/usr/bin/env python3
"""Host tests for FortressOS tar tool under ASan and UBSan."""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build" / "tar-host"


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    flags = [
        "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-O2",
        "-Isrc/include", "-Isrc/fs", "-Iuser/tools"
    ]
    sources = [
        "user/tools/digest.c",
        "user/tools/tar.c",
        "user/tools/common.c",
        "tests/tar_host.c"
    ]
    exe = BUILD / "tar_host_tests"
    compile_cmd = flags + [
        "-DTOOL_HOST_TEST", "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer", "-no-pie"
    ] + sources + ["-o", str(exe)]

    print("Compiling host tests with ASan/UBSan...", flush=True)
    subprocess.run(compile_cmd, cwd=ROOT, check=True)

    print("Running host tests...", flush=True)
    subprocess.run([str(exe)], cwd=ROOT, check=True)
    print("PASS: Host tar verification complete.", flush=True)


if __name__ == "__main__":
    main()
