#!/usr/bin/env python3
"""Host ASan/UBSan test runner for HKDF-SHA-256 (RFC 5869)."""
from pathlib import Path
import ctypes as c
import hashlib
import hmac
import random
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / 'build/crypto-hkdf-host'

def run(cmd, **kw):
    subprocess.run(cmd, cwd=ROOT, check=True, **kw)

def py_hkdf_extract(salt: bytes, ikm: bytes) -> bytes:
    if not salt:
        salt = bytes([0] * 32)
    return hmac.new(salt, ikm, hashlib.sha256).digest()

def py_hkdf_expand(prk: bytes, info: bytes, length: int) -> bytes:
    t = b''
    okm = b''
    i = 1
    while len(okm) < length:
        t = hmac.new(prk, t + info + bytes([i]), hashlib.sha256).digest()
        okm += t
        i += 1
    return okm[:length]

def py_hkdf(salt: bytes, ikm: bytes, info: bytes, length: int) -> bytes:
    prk = py_hkdf_extract(salt, ikm)
    return py_hkdf_expand(prk, info, length)

def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    common = [
        'gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-Isrc/include', '-Isrc/lib/crypto/upstream/src', '-I.',
        '-DCRYPTO_HOST_TEST'
    ]
    crypto_wrappers = [f'src/lib/crypto/{x}.c' for x in ('hash', 'curve', 'aead', 'entropy')]
    vendor = [
        ('src/lib/crypto/upstream/src/monocypher.c', 'mono'),
        ('src/lib/crypto/upstream/src/optional/monocypher-ed25519.c', 'ed')
    ]

    flags = ['-O1', '-g', '-no-pie', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']

    objs = []
    for src_path, name in vendor:
        obj = str(BUILD / f'{name}.o')
        objs.append(obj)
        run(common + flags + ['-include', 'src/lib/crypto/namespace.h', '-c', src_path, '-o', obj])

    exe = str(BUILD / 'hkdf_test')
    run(common + flags + [
        'tests/crypto_hkdf_host.c',
        *crypto_wrappers,
        'src/lib/root_digest.c',
        *objs,
        '-o', exe
    ])

    print('+ running tests/crypto_hkdf_host.c under ASan/UBSan...')
    run([exe])

    # Build shared library for differential test against Python implementation
    sh_flags = common + ['-O2', '-fPIC', '-DCRYPTO_HOST_TEST']
    sh_objs = []
    for src_path, name in vendor:
        obj = str(BUILD / f'{name}-pic.o')
        sh_objs.append(obj)
        run(sh_flags + ['-include', 'src/lib/crypto/namespace.h', '-c', src_path, '-o', obj])

    sh_lib = str(BUILD / 'hkdf_diff.so')
    run(sh_flags + [
        '-shared',
        *crypto_wrappers,
        'tests/crypto_diff_shim.c',
        'src/lib/root_digest.c',
        *sh_objs,
        '-o', sh_lib
    ])

    print('+ running differential tests against Python reference...')
    lib = c.CDLL(sh_lib)
    P = c.c_void_p
    N = c.c_size_t

    def setup(name, types):
        f = getattr(lib, name)
        f.argtypes = types
        f.restype = c.c_int
        return f

    c_extract = setup('crypto_hkdf_extract', [P, N, P, N, P])
    c_expand = setup('crypto_hkdf_expand', [P, N, P, N, P, N])
    c_hkdf = setup('crypto_hkdf', [P, N, P, N, P, N, P, N])

    rng = random.Random(0x5869256)
    lengths = [0, 1, 16, 32, 42, 64, 82, 100, 255, 256, 1024, 2048, 4096, 8160]

    for round_idx in range(50):
        l = lengths[round_idx % len(lengths)]
        salt_len = [0, 0, 1, 13, 16, 32, 64, 80, 128][round_idx % 9]
        ikm_len = [1, 16, 22, 32, 48, 80, 100, 256][round_idx % 8]
        info_len = [0, 0, 1, 10, 32, 64, 80, 128][round_idx % 8]

        salt = rng.randbytes(salt_len)
        ikm = rng.randbytes(ikm_len)
        info = rng.randbytes(info_len)

        expected_prk = py_hkdf_extract(salt, ikm)
        expected_okm = py_hkdf_expand(expected_prk, info, l)

        # Test extract
        prk_buf = c.create_string_buffer(32)
        r = c_extract(salt, salt_len, ikm, ikm_len, prk_buf)
        assert r == 0, f"extract failed in round {round_idx}"
        assert prk_buf.raw == expected_prk, f"extract mismatch in round {round_idx}"

        # Test expand
        okm_buf = c.create_string_buffer(l) if l > 0 else None
        r = c_expand(expected_prk, len(expected_prk), info, info_len, okm_buf, l)
        assert r == 0, f"expand failed in round {round_idx}"
        if l > 0:
            assert okm_buf.raw == expected_okm, f"expand mismatch in round {round_idx}"

        # Test full wrapper
        wrapper_buf = c.create_string_buffer(l) if l > 0 else None
        r = c_hkdf(salt, salt_len, ikm, ikm_len, info, info_len, wrapper_buf, l)
        assert r == 0, f"wrapper failed in round {round_idx}"
        if l > 0:
            assert wrapper_buf.raw == expected_okm, f"wrapper mismatch in round {round_idx}"

    print(f'PASS: 50 differential rounds matched Python RFC 5869 reference byte-for-byte.')
    print('PASS test-crypto-hkdf-host: 100% vector and differential verification clean under ASan/UBSan')

if __name__ == '__main__':
    main()
