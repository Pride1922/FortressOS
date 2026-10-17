#!/usr/bin/env python3
"""Host ASan/UBSan test runner for SSH transport layer, sshbuf, ChaCha20-Poly1305, and KEX."""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / 'build/ssh-transport-host'

def run(args, **kw):
    return subprocess.run(list(map(str, args)), cwd=ROOT, check=True, **kw)

def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    common = [
        'gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-Isrc/include', '-Isrc/lib/crypto/upstream/src', '-I.',
        '-DCRYPTO_HOST_TEST'
    ]
    crypto_wrappers = [f'src/lib/crypto/{x}.c' for x in ('hash', 'curve', 'aead', 'entropy')]
    vendor = [
        'src/lib/crypto/upstream/src/monocypher.c',
        'src/lib/crypto/upstream/src/optional/monocypher-ed25519.c'
    ]
    sshd_units = ['user/sshd/sshbuf.c', 'user/sshd/cipher.c', 'user/sshd/kex.c']

    flags = ['-O1', '-g', '-no-pie', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']

    objs = []
    for i, p in enumerate(vendor):
        obj = BUILD / f'vendor-{i}.o'
        objs.append(obj)
        run(common + flags + ['-include', 'src/lib/crypto/namespace.h', '-c', p, '-o', obj])

    exe = BUILD / 'transport-test'
    run(common + flags + [
        'tests/ssh_transport_host.c',
        *sshd_units,
        *crypto_wrappers,
        'src/lib/root_digest.c',
        *objs,
        '-o', exe
    ])

    print(f"+ {exe}")
    res = subprocess.run([str(exe)], cwd=ROOT)
    if res.returncode != 0:
        print(f"FAILED: transport-test exited with {res.returncode}", file=sys.stderr)
        sys.exit(res.returncode)

    print("PASS: SSH transport engine, sshbuf, chacha20-poly1305, and KEX gates verified under ASan/UBSan")

if __name__ == '__main__':
    main()
