#!/usr/bin/env python3
"""Actual BLAKE2s code: offline official KAT + hashlib oracle, all under sanitizers."""
from pathlib import Path
import hashlib
import os
import random
import re
import subprocess

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / 'build/crypto-blake2s-host'
KAT_SHA256 = 'a915b96fc07f6ece3b3355a5cbb813dbf9cd0ad37c22a9b89bf11f9c47922418'


def run(args, **kw):
    subprocess.run(args, cwd=ROOT, check=True, **kw)


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    kat = (ROOT / 'tests/vectors/blake2s-kat.txt').read_bytes()
    assert hashlib.sha256(kat).hexdigest() == KAT_SHA256
    cases = re.findall(r'in:\s*([0-9a-f]*)\nkey:\s*([0-9a-f]+)\nhash:\s*([0-9a-f]+)', kat.decode())
    assert len(cases) == 256
    for n, (msg, key, digest) in enumerate(cases):
        assert bytes.fromhex(msg) == bytes(range(n))
        assert bytes.fromhex(key) == bytes(range(32))
        assert len(digest) == 64
    # RFC 7693 Appendix B, independent literal rather than an oracle result.
    cases.append(('616263', '', '508c5e8c327c14e2e1a72ba34eeb452f37458b209ed63a294d999b4c86675982'))
    rng = random.Random(7693)
    lengths = [0, 1, 2, 3, 31, 32, 63, 64, 65, 127, 128, 129, 255, 256, 257, 1024, 4097]
    for out_len in range(1, 33):
        for key_len in range(33):
            n = lengths[(out_len + key_len) % len(lengths)]
            msg, key = rng.randbytes(n), rng.randbytes(key_len)
            digest = hashlib.blake2s(msg, key=key, digest_size=out_len).hexdigest()
            cases.append((msg.hex(), key.hex(), digest))
    header = BUILD / 'vectors.h'
    header.write_text('static const struct { const char *in, *key, *hash; } vectors[] = {\n' +
                      ''.join('{"%s","%s","%s"},\n' % c for c in cases) + '};\n')
    flags = ['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Isrc/include',
             '-Isrc/lib/crypto/upstream/src', '-I' + str(BUILD), '-O1', '-g', '-no-pie', '-DCRYPTO_HOST_TEST',
             '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
             '-fno-omit-frame-pointer', '-ffunction-sections', '-fdata-sections']
    mono = str(BUILD / 'mono.o')
    run(flags + ['-include', 'src/lib/crypto/namespace.h', '-c',
                 'src/lib/crypto/upstream/src/monocypher.c', '-o', mono])
    exe = str(BUILD / 'blake2s_test')
    run(flags + ['tests/crypto_blake2s_host.c', 'src/lib/crypto/blake2s.c',
                 'src/lib/crypto/hash.c', mono, '-Wl,--gc-sections', '-o', exe])
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',
               UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    run([exe], env=env)
    print('PASS test-crypto-blake2s-host: 256 official keyed KATs, RFC abc, '
          '1056 hashlib cases; one-shot/streaming, bounds, lifecycle and wipe; ASan/UBSan clean.')


if __name__ == '__main__':
    main()

