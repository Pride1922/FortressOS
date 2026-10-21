"""Independent Python wire producer for candidate PKG-1 syntax test vectors.
No package signing, ELF validation, extraction or target mutation.
"""
import pathlib
import struct


def text(s):
    b = s.encode('ascii')
    return struct.pack('<H', len(b)) + b


def record(path, role=2, mode=0o644, size=1, reserved=0):
    return struct.pack('<BBHQ', role, reserved, mode, size) + bytes(range(32)) + text(path)


def manifest(records=None, build=1, low=1, high=3, platform=bytes(range(32)),
             name='demo', command='demo', version='1', description=''):
    if records is None:
        records = [record('bin/demo', 1, 0o755, 3), record('doc/readme')]
    return (struct.pack('<4sHHQQQ', b'FMN1', 1, len(records), build, low, high)
            + platform + text(name) + text(command) + text(version) + text(description)
            + b''.join(records))


def main():
    good = manifest()
    cases = [(True, good)]
    cases += [(False, good[:i]) for i in range(len(good))]
    cases += [(False, good + b'\0'), (False, manifest(build=0)),
              (False, manifest(low=4)), (False, manifest(platform=bytes(32))),
              (False, manifest(command='Demo')), (False, manifest(version='\0')),
              (False, manifest(records=[]))]
    for offset in [0, 1, 2, 3, 4, 5, 6, 7]:
        changed = bytearray(good)
        changed[offset] ^= 0x80
        cases.append((False, changed))
    exe = record('bin/demo', 1, 0o755, 3)
    for path in ['doc/../x', 'doc/.', 'doc//x', 'doc/x/', '/doc/x', 'doc/a\\b',
                 'doc/a:b', 'doc/\0x', 'doc/' + 'a'*64, 'etc/passwd']:
        cases.append((False, manifest([exe, record(path)])))
    for role, mode, size, reserved in [(3, 0o644, 1, 0), (2, 0o777, 1, 0),
                                      (2, 0o644, 0, 0), (2, 0o644, 2**64-1, 0),
                                      (2, 0o644, 1, 1)]:
        cases.append((False, manifest([exe, record('doc/a', role, mode, size, reserved)])))
    cases += [(False, manifest([exe, exe])),
              (True, manifest([exe, record('doc/' + 'a'*48 + '/' + 'b'*47)])),
              (False, manifest([exe, record('doc/' + 'a'*48 + '/' + 'b'*48)])),
              (False, manifest([record('doc/a')])),
              (False, manifest([exe, record('doc/z'), record('doc/a')])),
              (False, manifest([exe, record('doc/a'), record('doc/a-'), record('doc/a/b')])),
              (False, manifest([record('bin/other', 1, 0o755)])),
              (False, manifest([record('bin/demo', 1, 0o755, 4*1024*1024+1)])),
              (False, manifest([exe] + [record(f'doc/{i:02}') for i in range(16)])),
              (False, manifest([exe, record('doc/a', size=16*1024*1024)])),
              (True, manifest([exe] + [record(f'doc/{i:02}') for i in range(15)])),
              (True, manifest([record('bin/demo', 1, 0o755, 4*1024*1024)]))]
    out = pathlib.Path('build/pkg-format/manifest-vectors.bin')
    with out.open('wb') as f:
        for expected, data in cases:
            f.write(struct.pack('<BI', expected, len(data)))
            f.write(data)
    print(f'Generated {len(cases)} independent manifest vectors')


if __name__ == '__main__':
    main()
