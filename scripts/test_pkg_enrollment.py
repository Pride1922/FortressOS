"""Private codecs/signature gate. Public TEST keys; no mount or media build.

Independent Python struct/hashlib + cryptography/OpenSSL wire oracle. Retains
copied sources, vectors, compiler logs, failures and preservation in evidence.
"""
import datetime
import hashlib
import json
import os
import pathlib
import shutil
import struct
import subprocess
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

ROOT = pathlib.Path(__file__).resolve().parents[1]
KEYS = [Ed25519PrivateKey.from_private_bytes(bytes([n])*32) for n in range(41, 50)]
ALLPUB = [k.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw) for k in KEYS]
PUB = ALLPUB[:3]
UUIDS = [bytes([i])*16 for i in range(1, 6)]
BODY = 2464


def sha(p):
    return hashlib.sha256(p).digest()


def put(b, off, fmt, *values):
    struct.pack_into('<'+fmt, b, off, *values)


def body(bs=1024):
    b = bytearray(BODY)
    b[:16] = b'FORT-PKG-ENR-v2\0'
    put(b, 16, 'IHHQII', BODY+192, 2, 2, 5, 2, bs)
    b[40:120] = b''.join(UUIDS)
    for i in range(7):
        b[120+32*i:152+32*i] = sha(bytes([i+1]))
    put(b, 344, 'QIHHIHHII', 32768, 8192, 16, 11, 8192, 16, 32, 64, 1)
    for i in range(16):
        off = 416+128*i
        directory = i == 3 or i >= 8
        put(b, off, 'HHIIIQQH', i+1, 2 if directory else 1, 100+i, 10+i,
            0o700 if directory else 0o600, bs, 1, 1)
        put(b, off+40, 'IIQ', 0, 1, 1000+8*i)
    return b


def signed(b=None, signers=(0, 1)):
    b = bytearray(body() if b is None else b)
    put(b, 16, 'I', BODY+96*len(signers))
    put(b, 22, 'H', len(signers))
    records = sorted((sha(ALLPUB[i]), KEYS[i].sign(bytes(b))) for i in signers)
    for i in signers:
        KEYS[i].public_key().verify(KEYS[i].sign(bytes(b)), bytes(b))
    return bytes(b)+b''.join(k+s for k, s in records)


def policy(data, keys=PUB, threshold=2, epoch=5, uuids=UUIDS, pin=None):
    return (b''.join(keys).ljust(256, b'\0')+b''.join(uuids)+
            (sha(data) if pin is None else pin)+struct.pack('<IIQ', len(keys), threshold, epoch))


def origin(active=True, role=0, size=8192, bs=1024):
    b = bytearray(1024)
    b[:16] = b'FORT-ACC-ORG-v1\0'
    put(b, 16, 'IHHQQQ', 1024, 1, int(active), 7, 5, 3)
    b[48:64], b[64:80], b[80:112] = UUIDS[4], UUIDS[0], sha(bytes([7]))
    if active:
        private = 3 <= role <= 5 or role >= 9
        parent = 13 if private else 9
        mode = 0o644 if role in (0, 1, 6, 7) else 0o600
        count = (size+bs-1)//bs
        put(b, 112, 'HHIIIIIIIQHH', role, 1, 500, 2, 100+parent, 10+parent,
            mode, 0, 0, size, int(bool(size)), int(bool(size)))
        if size:
            put(b, 160, 'IIQ', 0, count, 4000)
            put(b, 416, 'QII', 4000, count, 0)
    return seal(b)


def seal(b):
    b = bytearray(b)
    b[992:1024] = sha(b[:992])
    return bytes(b)


def vectors():
    cases = []
    good = signed()

    def desc(label, data=None, rules=None, ok=False):
        data = good if data is None else data
        cases.append((label, 0, ok, data, policy(data) if rules is None else rules))

    desc('valid-2-of-3', ok=True)
    desc('valid-3-of-3', signed(signers=(0, 1, 2)), ok=True)
    eight = signed(signers=tuple(range(8)))
    desc('valid-eight-of-eight', eight, policy(eight, keys=ALLPUB[:8], threshold=8), True)
    desc('eight-plus-unknown-signer', signed(signers=(0, 1, 8)))
    b = bytearray(policy(good)); put(b, 368, 'I', 9)
    desc('key-count-plus-one', rules=bytes(b))
    desc('maximum-descriptor-plus-one-byte', eight+b'x', policy(eight+b'x', keys=ALLPUB[:8]))
    for bs in (2048, 4096):
        desc(f'valid-block-{bs}', signed(body(bs)), ok=True)
    b = body(); put(b, 432, 'QQH', 4096, 4, 4)
    for i in range(4):
        put(b, 456+16*i, 'IIQ', i, 1, 1000+2*i)
    desc('valid-four-inline-extents', signed(b), ok=True)
    desc('wrong-pin', rules=policy(good, pin=bytes(32)))
    desc('wrong-epoch', rules=policy(good, epoch=6))
    for i in range(5):
        ids = UUIDS.copy(); ids[i] = bytes([91])*16
        desc(f'wrong-uuid-{i}', rules=policy(good, uuids=ids))
    for label, keys, threshold in [('no-keys', [], 1), ('zero-key', [bytes(32)], 1),
                                    ('duplicate-key', [PUB[0], PUB[0]], 2),
                                    ('zero-threshold', PUB, 0), ('over-threshold', PUB, 4),
                                    ('unknown-signer', [PUB[0], PUB[2]], 2)]:
        desc(label, rules=policy(good, keys=keys, threshold=threshold))
    desc('threshold-not-met', signed(signers=(0,)))
    desc('duplicate-signer', signed(signers=(0, 0)))
    b = bytearray(good); b[-1] ^= 1
    desc('signature-only-corrupt-with-matching-pin', bytes(b))
    b = signed(signers=(0, 1, 2)); b = b[:-1]+bytes([b[-1]^1])
    desc('invalid-extra-signature-despite-threshold', b)
    desc('unsorted-signers', good[:BODY]+good[BODY+96:]+good[BODY:BODY+96])
    desc('suffix', good+b'x')
    # Re-sign and independently re-pin each malformed structure. Crypto passes;
    # parser must reject the isolated field violation.
    changes = [(0, 'B', 1), (20, 'H', 3), (24, 'Q', 0), (24, 'Q', 2**64-1),
               (32, 'I', 1), (36, 'I', 512), (344, 'Q', 2**64-1),
               (352, 'I', 1048577), (356, 'H', 17), (358, 'H', 12),
               (360, 'I', 8193), (364, 'H', 17), (366, 'H', 33),
               (368, 'I', 65), (372, 'I', 2), (376, 'B', 1),
               (416, 'H', 2), (418, 'H', 2), (420, 'I', 0), (424, 'I', 0),
               (428, 'I', 0o644), (432, 'Q', 0), (440, 'Q', 2),
               (448, 'H', 5), (450, 'B', 1), (456, 'I', 1), (460, 'I', 0),
               (464, 'Q', 32768), (480, 'B', 1), (544+4, 'I', 100),
               (544+48, 'Q', 1000)]
    for off, fmt, value in changes:
        b = body(); put(b, off, fmt, value)
        desc(f'resigned-field-{off}-{value}', signed(b))
    for off, n in [(40, 16), (120, 32), (312, 32)]:
        b = body(); b[off:off+n] = bytes(n)
        desc(f'zero-identity-{off}', signed(b))

    def slot(label, data, ok=False):
        cases.append((label, 1, ok, data, b''))

    slot('valid-active', origin(), True)
    slot('valid-empty', origin(False), True)
    b = bytearray(origin()); put(b, 152, 'HH', 8, 9)
    for i in range(8):
        put(b, 160+16*i, 'IIQ', i, 1, 4000+2*i)
        put(b, 416+16*i, 'QII', 4000+2*i, 1, 0)
    put(b, 416+16*8, 'QII', 4200, 1, 0)
    slot('valid-fragmented-plus-metadata-budget', seal(b), True)
    b = bytearray(b); put(b, 160+16, 'IIQ', 1, 1, 4000)
    slot('overlapping-original-data-extents', seal(b))
    for role in range(11):
        slot(f'valid-role-{role}', origin(role=role, size=160 if role >= 9 else 8192), True)
        slot(f'valid-zero-allocation-role-{role}', origin(role=role, size=0), True)
    slot('slot-suffix', origin()+b'x')
    b = bytearray(origin()); b[-1] ^= 1
    slot('slot-checksum', b)
    changes = [(0, 'B', 1), (16, 'I', 1023), (20, 'H', 2), (22, 'H', 2),
               (24, 'Q', 8), (32, 'Q', 6), (40, 'Q', 4), (48, 'B', 1),
               (64, 'B', 2), (80, 'B', 1), (112, 'H', 11), (114, 'H', 3),
               (116, 'I', 100), (120, 'I', 0), (124, 'I', 1), (128, 'I', 1),
               (132, 'I', 0o600), (136, 'I', 1), (140, 'I', 1), (144, 'Q', 8193),
               (152, 'H', 17), (154, 'H', 33), (156, 'B', 1), (160, 'I', 1),
               (164, 'I', 9), (168, 'Q', 2**64-1), (176, 'B', 1),
               (416, 'Q', 1000), (424, 'I', 10), (428, 'I', 1), (432, 'B', 1),
               (928, 'B', 1)]
    for off, fmt, value in changes:
        b = bytearray(origin()); put(b, off, fmt, value)
        assert bytes(b) != origin(), (off, value)
        slot(f'resealed-slot-{off}-{value}', seal(b))
    b = bytearray(origin(False)); b[112] = 1
    slot('empty-with-payload', seal(b))
    for bs in (2048, 4096):
        data = signed(body(bs))
        cases.append((f'select-geometry-{bs}', 2, True, data, policy(data)))
        for role in range(11):
            slot(f'geometry-{bs}-role-{role}', origin(role=role, size=160 if role >= 9 else 8192, bs=bs), True)
    return cases


def main():
    os.chdir(ROOT)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%d-%H%M%S-%f')
    out = ROOT/'.codex-remote-attachments'/('pkg-enrollment-'+stamp)
    out.mkdir(parents=True)
    source = out/'source'; source.mkdir()
    paths = [p for folder in ('src/include', 'src/lib/crypto', 'user/tools')
             for p in pathlib.Path(folder).rglob('*') if p.is_file() and p.suffix in ('.h', '.c')]
    paths += [pathlib.Path(p) for p in ('tests/pkg_enrollment_candidate.h',
              'tests/pkg_enrollment_candidate.c', 'tests/pkg_enrollment_host.c',
              'scripts/test_pkg_enrollment.py')]
    guards = paths+[pathlib.Path(p) for p in ('AGENTS.md', 'README.md', 'assets/ssl/ca-certificates.sha256')]
    guards += [p for p in pathlib.Path('bin').iterdir() if p.is_file() and
               p.name in ('fortress.elf', 'initramfs.tar', 'fortress.iso', 'fortress.img')]
    def hashes():
        return {str(p): hashlib.file_digest(p.open('rb'), 'sha256').hexdigest() for p in guards}
    before = hashes()
    for p in paths:
        target = source/p; target.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(p, target)
    cases = vectors()
    with (out/'vectors.bin').open('wb') as f:
        for _, kind, ok, data, rules in cases:
            f.write(struct.pack('<BBI', kind, ok, len(data))+rules+data)
    manifest = {'status': 'INCOMPLETE', 'cases': [{'label': x[0], 'kind': x[1], 'expected': x[2]} for x in cases],
                'before': before, 'commands': []}
    def run(args, cwd=source):
        index = len(manifest['commands'])
        manifest['commands'].append(args)
        with (out/f'command-{index}.log').open('w') as log:
            subprocess.run(args, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=120)
    try:
        flags = ['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                 '-ffunction-sections', '-fdata-sections', '-fsanitize=address,undefined',
                 '-fno-omit-frame-pointer', '-I.', '-Isrc/include', '-Iuser/tools',
                 '-Isrc/lib/crypto/upstream/src']
        objects = []
        for name, path in [('mono', 'src/lib/crypto/upstream/src/monocypher.c'),
                           ('ed', 'src/lib/crypto/upstream/src/optional/monocypher-ed25519.c')]:
            obj = str(out/(name+'.o'))
            run(flags+['-include', 'src/lib/crypto/namespace.h', '-c', path, '-o', obj]); objects.append(obj)
        run(flags+['-no-pie', '-Wl,--gc-sections', 'tests/pkg_enrollment_host.c',
                   'tests/pkg_enrollment_candidate.c', 'src/lib/crypto/hash.c', 'src/lib/crypto/curve.c',
                   'user/tools/digest.c', *objects, '-o', str(out/'probe')])
        run([str(out/'probe'), str(out/'vectors.bin')])
        run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2', '-ffreestanding',
             '-fno-builtin', '-mno-red-zone', '-mno-sse', '-mno-mmx', '-msoft-float',
             '-fstack-usage', '-Wframe-larger-than=1024', '-Isrc/include',
             '-c', 'tests/pkg_enrollment_candidate.c', '-o', str(out/'candidate.o')])
        manifest['status'] = 'PASS'
    except Exception as exc:
        manifest['status'] = 'FAIL'; manifest['error'] = repr(exc)
        raise
    finally:
        manifest['after'] = hashes()
        manifest['preserved'] = manifest['before'] == manifest['after']
        manifest['copied_sources_preserved'] = all(
            hashlib.file_digest((source/p).open('rb'), 'sha256').hexdigest() == before[str(p)] for p in paths)
        manifest['vectors_sha256'] = hashlib.file_digest((out/'vectors.bin').open('rb'), 'sha256').hexdigest()
        if not manifest['preserved'] or not manifest['copied_sources_preserved']:
            manifest['status'] = 'FAIL'
        manifest['artifacts'] = {p.name: hashlib.file_digest(p.open('rb'), 'sha256').hexdigest()
                                 for p in out.iterdir() if p.is_file() and p.name != 'manifest.json'}
        (out/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
        print(out, manifest['status'], flush=True)
    assert manifest['status'] == 'PASS'


if __name__ == '__main__':
    main()
