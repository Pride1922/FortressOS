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

            grep_exe = Path(tmp) / "grep_host"
            subprocess.run([
                "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-no-pie",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-DTOOL_HOST_TEST",
                "-Isrc/include", "-Isrc/fs", "-Iuser/tools",
                "tests/grep_host.c", "user/tools/common.c", "user/tools/grep.c",
                "-o", str(grep_exe)
            ], cwd=REPO, check=True)
            subprocess.run([str(grep_exe)], check=True, timeout=30)

            uniq_exe = Path(tmp) / "uniq_host"
            subprocess.run([
                "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-no-pie",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-DTOOL_HOST_TEST",
                "-Isrc/include", "-Isrc/fs", "-Iuser/tools",
                "tests/uniq_host.c", "user/tools/common.c", "user/tools/uniq.c",
                "-o", str(uniq_exe)
            ], cwd=REPO, check=True)
            subprocess.run([str(uniq_exe)], check=True, timeout=30)

            xxd_exe = Path(tmp) / "xxd_host"
            subprocess.run([
                "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-no-pie",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-DTOOL_HOST_TEST",
                "-Isrc/include", "-Isrc/fs", "-Iuser/tools",
                "tests/xxd_host.c", "user/tools/common.c", "user/tools/xxd.c",
                "-o", str(xxd_exe)
            ], cwd=REPO, check=True)
            subprocess.run([str(xxd_exe)], check=True, timeout=30)

            sort_exe = Path(tmp) / "sort_host"
            subprocess.run([
                "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-no-pie",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-DTOOL_HOST_TEST",
                "-Isrc/include", "-Isrc/fs", "-Iuser/tools",
                "tests/sort_host.c", "user/tools/common.c", "user/tools/sort.c",
                "-o", str(sort_exe)
            ], cwd=REPO, check=True)
            subprocess.run([str(sort_exe)], check=True, timeout=30)

            diff_exe = Path(tmp) / "diff_host"
            subprocess.run([
                "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-no-pie",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-DTOOL_HOST_TEST",
                "-Isrc/include", "-Isrc/fs", "-Iuser/tools",
                "tests/diff_host.c", "user/tools/common.c", "user/tools/diff.c",
                "-o", str(diff_exe)
            ], cwd=REPO, check=True)
            subprocess.run([str(diff_exe)], check=True, timeout=30)

            patch_exe = Path(tmp) / "patch_host"
            subprocess.run([
                "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-no-pie",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-DTOOL_HOST_TEST",
                "-Isrc/include", "-Isrc/fs", "-Iuser/tools",
                "tests/patch_host.c", "user/tools/common.c", "user/tools/patch.c",
                "-o", str(patch_exe)
            ], cwd=REPO, check=True)
            subprocess.run([str(patch_exe)], check=True, timeout=30)

            diskbench_exe = Path(tmp) / "diskbench_host"
            subprocess.run([
                "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-no-pie",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-DTOOL_HOST_TEST",
                "-Isrc/include", "-Isrc/fs", "-Iuser/tools",
                "tests/diskbench_host.c", "user/tools/common.c", "user/tools/diskbench.c",
                "-o", str(diskbench_exe)
            ], cwd=REPO, check=True)
            subprocess.run([str(diskbench_exe)], check=True, timeout=30)

            disk_exe = Path(tmp) / "disk_host"
            subprocess.run([
                "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-no-pie",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-DTOOL_HOST_TEST",
                "-Isrc/include", "-Isrc/fs", "-Iuser/tools",
                "tests/disk_host.c", "user/tools/common.c", "user/tools/disk.c", "user/tools/diskbench.c",
                "-o", str(disk_exe)
            ], cwd=REPO, check=True)
            subprocess.run([str(disk_exe)], check=True, timeout=30)

            cp_touch_exe = Path(tmp) / "cp_touch_host"
            subprocess.run([
                "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-no-pie",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-DTOOL_HOST_TEST",
                "-Isrc/include", "-Isrc/fs", "-Iuser/tools",
                "tests/cp_touch_host.c", "user/tools/common.c", "user/tools/cp.c", "user/tools/touch.c",
                "-o", str(cp_touch_exe)
            ], cwd=REPO, check=True)
            subprocess.run([str(cp_touch_exe)], check=True, timeout=30)





