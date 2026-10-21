"""Deterministic unsigned PKG-1 fixtures, independent of the C verifier.
These synthetic ELF files are validation fixtures, never published packages.
"""
import gzip
import hashlib
import json
import pathlib
import struct
import zlib
from pkg_manifest_vectors import text


def elf(mem=4096, flags=5, entry=0x400078, addr=0x400000, kind=1, phoff=64):
    ident = b'\x7fELF\x02\x01\x01' + bytes(9)
    h = struct.pack('<16sHHIQQQIHHHHHH', ident, 2, 62, 1, entry, phoff, 0, 0,
                    64, 56, 1, 0, 0, 0)
    return h + struct.pack('<IIQQQQQQ', kind, flags, 0, addr, 0, 121, mem, 4096) + b'\xc3'


def octal(value, width):
    return f'{value:0{width-1}o}'.encode() + b'\0'


def tar_header(path, data, mode):
    h = bytearray(512)
    h[:len(path)] = path.encode()
    for offset, value, width in [(100, mode, 8), (108, 0, 8), (116, 0, 8),
                                  (124, len(data), 12), (136, 0, 12)]:
        h[offset:offset+width] = octal(value, width)
    h[148:156] = b' '*8
    h[156] = ord('0')
    h[257:265] = b'ustar\x0000'
    h[148:156] = octal(sum(h), 7) + b' '
    return bytes(h)


def parts(executable=None, documentation=b'PKG fixture\n'):
    if executable is None:
        executable = elf()
    entries = [('bin/demo', executable, 0o755, 1), ('doc/readme', documentation, 0o644, 2)]
    m = struct.pack('<4sHHQQQ', b'FMN1', 1, 2, 1, 1, 3) + bytes(range(32))
    m += text('demo') + text('demo') + text('1') + text('')
    tar = b''
    for path, data, mode, role in entries:
        m += struct.pack('<BBHQ', role, 0, mode, len(data)) + hashlib.sha256(data).digest() + text(path)
        tar += tar_header(path, data, mode) + data + bytes((-len(data)) % 512)
    return m, tar + bytes(1024)


def pack(m, tar, payload=None, expanded=None):
    if payload is None:
        payload = gzip.compress(tar, mtime=0)
    if expanded is None:
        expanded = len(tar)
    h = struct.pack('<8sIIIIIIQQ', b'FORTFPK\0', 1, 128, 0, len(m), 0, 0, len(payload), expanded)
    return h + hashlib.sha256(m).digest() + hashlib.sha256(payload).digest() + bytes(16) + m + payload


def fix_checksum(t):
    t[148:156] = b' '*8
    t[148:156] = octal(sum(t[:512]), 7) + b' '
    return t


def main():
    m, tar = parts()
    valid = pack(m, tar)
    assert valid == pack(*parts())
    # Independent standard-library decode and archive content inspection.
    import io
    import tarfile
    with tarfile.open(fileobj=io.BytesIO(tar), mode='r:') as archive:
        assert archive.getnames() == ['bin/demo', 'doc/readme']
        assert archive.extractfile('bin/demo').read() == elf()
    assert gzip.decompress(valid[128+len(m):]) == tar
    cases = [('valid-default', True, valid)]
    dm, dt = parts(documentation=b'abcd'*8192)
    dynamic = gzip.compress(dt, mtime=0)
    assert (dynamic[10] >> 1) & 3 == 2
    cases.append(('dynamic-huffman', True, pack(dm, dt, dynamic)))
    for strategy, name in [(zlib.Z_FIXED, 'fixed'), (zlib.Z_DEFAULT_STRATEGY, 'stored')]:
        co = zlib.compressobj(0 if name == 'stored' else 6, zlib.DEFLATED, 31, 8, strategy)
        cases.append((name, True, pack(m, tar, co.compress(tar)+co.flush())))
    for kwargs in [dict(mem=4*1024*1024+1), dict(flags=7), dict(entry=0),
                   dict(addr=0), dict(kind=3), dict(kind=2), dict(kind=7),
                   dict(addr=0x7fffefffb000), dict(phoff=2**64-1), dict(mem=10)]:
        cases.append(('elf-'+str(kwargs), False, pack(*parts(elf(**kwargs)))))
    cases.append(('elf-mapped-limit', True, pack(*parts(elf(mem=4*1024*1024)))))
    for offset, value, width, name in [(16, 3, 2, 'pie'), (56, 17, 2, 'phnum'),
                                       (72, 4096, 8, 'file-bounds'),
                                       (80, 0x400001, 8, 'congruence'),
                                       (80, 0x800000000000, 8, 'upper-half'),
                                       (68, 4, 4, 'entry-not-exec')]:
        e = bytearray(elf()); e[offset:offset+width] = value.to_bytes(width, 'little')
        cases.append(('elf-'+name, False, pack(*parts(e))))
    for count in [1, 63]:
        cases.append(('short-elf-'+str(count), False, pack(*parts(elf()[:count]))))
    e = bytearray(elf()[:120] + elf()[64:120] + b'X')
    e[56:58] = (2).to_bytes(2, 'little')
    cases.append(('elf-overlap', False, pack(*parts(e))))
    for offset, value, name in [(156, ord('2'), 'link'), (157, 120, 'link-name'),
                                (345, 120, 'prefix'), (265, 120, 'uname'),
                                (0, ord('/'), 'path'), (100, 0xff, 'octal'),
                                (512+121, 1, 'padding'), (len(tar)-1, 1, 'end-block')]:
        t = bytearray(tar); t[offset] = value
        if offset < 512:
            fix_checksum(t)
        cases.append((name, False, pack(m, t)))
    t = bytearray(tar); t[148] ^= 1
    cases.append(('checksum', False, pack(m, t)))
    t = bytearray(tar); t[512] ^= 1
    cases.append(('file-hash', False, pack(m, t)))
    cases += [('extra-member', False, pack(m, tar + tar[:1024])),
              ('extra-zero-block', False, pack(m, tar + bytes(512))),
              ('missing-end-block', False, pack(m, tar[:-512])),
              ('archive-length', False, pack(m, tar, expanded=len(tar)+1))]
    gz = gzip.compress(tar, mtime=0)
    # Mutate DEFLATE bytes, resealing outer hashes so the decoder is exercised.
    for i in range(10, min(len(gz)-8, 42)):
        g = bytearray(gz); g[i] ^= 1 << (i % 8)
        try:
            expected = gzip.decompress(g) == tar
        except (OSError, EOFError, zlib.error):
            expected = False
        cases.append((f'deflate-mutation-{i}', expected, pack(m, tar, g)))
    for offset, name in [(-8, 'crc'), (-4, 'isize'), (3, 'flags')]:
        g = bytearray(gz); g[offset] ^= 1
        cases.append((name, False, pack(m, tar, g)))
    cases += [('gzip-concat', False, pack(m, tar, gz+gz)),
              ('gzip-suffix', False, pack(m, tar, gz+b'X')),
              ('gzip-truncated', False, pack(m, tar, gz[:-1])),
              ('gzip-reserved-block', False, pack(m, tar, gz[:10]+b'\x07'+gz[11:])),
              ('expansion-overrun', False, pack(m, tar, gzip.compress(bytes(1024*1024), mtime=0))),
              ('suffix', False, valid+b'X')]
    for offset, name in [(48, 'manifest-hash'), (80, 'payload-hash'), (128, 'manifest-magic')]:
        b = bytearray(valid); b[offset] ^= 1
        cases.append((name, False, b))
    out = pathlib.Path('build/pkg-format')
    with (out/'artifact-vectors.bin').open('wb') as f:
        for name, expected, data in cases:
            f.write(struct.pack('<BI', expected, len(data)))
            f.write(hashlib.sha256(data).digest())
            f.write(data)
    (out/'fixture.fpk').write_bytes(valid)
    (out/'artifact-vectors.json').write_text(json.dumps({
        'fixture_sha256': hashlib.sha256(valid).hexdigest(),
        'cases': [{'name': name, 'expected': expected, 'bytes': len(data)} for name, expected, data in cases],
    }, indent=2)+'\n')
    print(f'Generated {len(cases)} artifact vectors; deterministic fixture SHA-256 {hashlib.sha256(valid).hexdigest()}')


if __name__ == '__main__':
    main()
