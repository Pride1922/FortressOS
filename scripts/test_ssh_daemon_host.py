#!/usr/bin/env python3
"""Host ASan/UBSan test runner for SSH daemon lifecycle and deployment (Phase 7)."""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / 'build/ssh-daemon-host'

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
        'user/sshd/sshd_config.c',
        'user/sshd/sshd_readiness.c',
        'user/sshd/sshd_admission.c'
    ]
    flags = ['-O1', '-g', '-no-pie', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']

    exe = BUILD / 'daemon-test'
    run(common + flags + [
        'tests/ssh_daemon_host.c',
        *sshd_units,
        '-o', exe
    ])

    print(f"+ {exe}")
    res = subprocess.run([str(exe)], cwd=ROOT)
    if res.returncode != 0:
        print(f"FAILED: daemon-test exited with {res.returncode}", file=sys.stderr)
        sys.exit(res.returncode)

    print("PASS: SSH daemon lifecycle and configuration verified under ASan/UBSan")

if __name__ == '__main__':
    main()
