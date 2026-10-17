#!/usr/bin/env python3
"""Host ASan/UBSan test runner for SSH authentication, privilege separation, and monitor IPC."""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / 'build/ssh-auth-host'

def run(args, **kw):
    return subprocess.run(list(map(str, args)), cwd=ROOT, check=True, **kw)

def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    common = [
        'gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-Isrc/include', '-Isrc/lib/crypto/upstream/src', '-Iuser/tools', '-Iuser/sshd', '-I.',
        '-DCRYPTO_HOST_TEST'
    ]
    crypto_wrappers = [f'src/lib/crypto/{x}.c' for x in ('hash', 'curve', 'aead', 'entropy', 'ssh_hostkey')]
    vendor = [
        'src/lib/crypto/upstream/src/monocypher.c',
        'src/lib/crypto/upstream/src/optional/monocypher-ed25519.c'
    ]
    sshd_units = [
        'user/sshd/sshbuf.c',
        'user/sshd/cipher.c',
        'user/sshd/kex.c',
        'user/sshd/monitor.c',
        'user/sshd/auth.c'
    ]
    tool_units = [
        'user/tools/userdb.c',
        'src/lib/root_digest.c',
        'src/kernel/creds.c'
    ]

    flags = ['-O1', '-g', '-no-pie', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']

    objs = []
    # If build/ssh-transport-host already built vendor objects, we can reuse or link them
    transport_build = ROOT / 'build/ssh-transport-host'
    for i, p in enumerate(vendor):
        obj = BUILD / f'vendor-{i}.o'
        objs.append(obj)
        trans_obj = transport_build / f'vendor-{i}.o'
        if not obj.exists():
            if trans_obj.exists():
                import shutil
                shutil.copy2(trans_obj, obj)
            else:
                run(common + flags + ['-include', 'src/lib/crypto/namespace.h', '-c', p, '-o', obj])

    exe = BUILD / 'auth-test'
    run(common + flags + [
        'tests/ssh_auth_host.c',
        *sshd_units,
        *crypto_wrappers,
        *tool_units,
        *objs,
        '-o', exe
    ])

    print(f"+ {exe}")
    res = subprocess.run([str(exe)], cwd=ROOT)
    if res.returncode != 0:
        print(f"FAILED: auth-test exited with {res.returncode}", file=sys.stderr)
        sys.exit(res.returncode)

    print("PASS: SSH authentication, privilege separation, and monitor IPC verified under ASan/UBSan")

if __name__ == '__main__':
    main()
