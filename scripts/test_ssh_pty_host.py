#!/usr/bin/env python3
"""Host ASan/UBSan test runner for SSH PTY bridge and terminal control (Phase 6)."""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / 'build/ssh-pty-host'

def run(args, **kw):
    return subprocess.run(list(map(str, args)), cwd=ROOT, check=True, **kw)

def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    common = [
        'gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-Isrc/include', '-Isrc/lib/crypto/upstream/src', '-Iuser/tools', '-Iuser/sshd', '-Isrc/fs', '-I.',
        '-DCRYPTO_HOST_TEST'
    ]
    sshd_units = [
        'user/sshd/sshbuf.c',
        'user/sshd/channel.c',
        'user/sshd/pty_bridge.c'
    ]
    flags = ['-O1', '-g', '-no-pie', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']

    exe = BUILD / 'pty-test'
    run(common + flags + [
        'tests/ssh_pty_host.c',
        *sshd_units,
        '-o', exe
    ])

    print(f"+ {exe}")
    res = subprocess.run([str(exe)], cwd=ROOT)
    if res.returncode != 0:
        print(f"FAILED: pty-test exited with {res.returncode}", file=sys.stderr)
        sys.exit(res.returncode)

    print("PASS: SSH PTY bridge and terminal control verified under ASan/UBSan")

    kernel_exe = BUILD / 'pty-output-test'
    run(common + flags + ['-pthread', '-DTEST_SMP_MEMORY',
        '-Isrc/kernel', '-Isrc/drivers', '-Isrc/arch/x86_64', '-Isrc/mm',
        '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
        'tests/pty_output_host.c', '-o', kernel_exe])
    run([kernel_exe])

if __name__ == '__main__':
    main()
