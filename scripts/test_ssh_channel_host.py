#!/usr/bin/env python3
"""Host ASan/UBSan test runner for SSH connection protocol channel manager (RFC 4254)."""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / 'build/ssh-channel-host'

def run(args, **kw):
    return subprocess.run(list(map(str, args)), cwd=ROOT, check=True, **kw)

def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    common = [
        'gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-Isrc/include', '-Isrc/lib/crypto/upstream/src', '-Iuser/tools', '-Iuser/sshd', '-I.',
        '-DCRYPTO_HOST_TEST'
    ]
    sshd_units = [
        'user/sshd/sshbuf.c',
        'user/sshd/channel.c'
    ]
    flags = ['-O1', '-g', '-no-pie', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']

    exe = BUILD / 'channel-test'
    run(common + flags + [
        'tests/ssh_channel_host.c',
        *sshd_units,
        '-o', exe
    ])

    print(f"+ {exe}")
    res = subprocess.run([str(exe)], cwd=ROOT)
    if res.returncode != 0:
        print(f"FAILED: channel-test exited with {res.returncode}", file=sys.stderr)
        sys.exit(res.returncode)

    print("PASS: RFC 4254 SSH channel protocol engine verified under ASan/UBSan")

if __name__ == '__main__':
    main()
