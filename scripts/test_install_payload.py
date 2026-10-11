#!/usr/bin/env python3
"""Actual native deployment engine, sanitizer file adapter and independent GPT oracle.
Synthetic bytes are deliberately not FAT/EXT4/boot artifacts. No installer acceptance
or physical claim. All paths are fresh regular-file fixtures on native WSL storage.
"""
import binascii
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import tarfile
import uuid

ROOT = Path(__file__).resolve().parents[1]


def main():
    parent = ROOT / 'build/root-phase4'
    parent.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix='payload-', dir=parent))
    paths = ['src/include/install_payload.h', 'src/lib/install_payload.c',
             'tests/install_payload_host.c', 'scripts/test_install_payload.py',
             'src/lib/crc32.c', 'user/tools/digest.c', 'Makefile',
             'src/include/install_archive.h', 'src/lib/install_archive.c',
             'tests/install_archive_host.c', 'src/kernel/root_boot.c',
             'src/fs/gpt.c', 'user/tools/userdb.c']
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    manifest = {'scope': 'synthetic deployment workbench; no native installer acceptance',
                'sources': {p: sha(ROOT / p) for p in paths}, 'commands': [], 'result': 'FAIL'}
    normal = {str(p): sha(p) for p in (ROOT / 'bin/fortress.iso', ROOT / 'bin/fortress.img') if p.is_file()}
    manifest['normal_artifacts'] = normal
    try:
        with tempfile.TemporaryDirectory(prefix='fortress-install-payload-') as temporary:
            temp = Path(temporary)
            archive_binary, archive = temp / 'archive-host', out / 'bootstrap.tar'
            command = ['gcc', '-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
                       '-fsanitize=address,undefined', '-no-pie', '-ffunction-sections',
                       '-fdata-sections', '-Wl,--gc-sections', '-Isrc/include', '-Isrc/fs',
                       '-Isrc/kernel', '-Isrc/lib', '-Isrc/drivers', '-Isrc/mm',
                       'src/lib/install_archive.c', 'tests/install_archive_host.c',
                       'src/kernel/root_boot.c', 'src/fs/gpt.c', 'user/tools/userdb.c',
                       'user/tools/digest.c', '-o', str(archive_binary)]
            manifest['commands'].append(command)
            with (out / 'archive.log').open('w') as log:
                subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=90)
                command = [str(archive_binary), str(archive)]
                manifest['commands'].append(command)
                subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=90)
            with tarfile.open(archive) as stream:
                entries = stream.getmembers()
                assert [(x.name, x.mode, x.uid, x.gid) for x in entries] == [
                    ('bin', 0o755, 0, 0), ('bootstrap', 0o700, 0, 0),
                    ('bootstrap/recovery.v1', 0o600, 0, 0)]
                record = stream.extractfile('bootstrap/recovery.v1').read()
                expected_record = (b'FORTRESS-RECOVERY-1\ninstall_id=00112233445566778899aabbccddeeff\n'
                                   b'root_partuuid=11223344-5566-7788-99aa-bbccddeeff00\n'
                                   b'filesystem_uuid=11223344-5566-7788-99aa-bbccddeeff00\n'
                                   b'password_hash=$5$rounds=100000$1234567890abcdef$'
                                   b'...........................................\n')
                assert record == expected_record
            manifest['archive_sha256'] = sha(archive)
            binary, disk = temp / 'host', temp / 'target.img'
            command = ['gcc', '-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
                       '-fsanitize=address,undefined', '-no-pie', '-Isrc/include', '-Isrc/lib',
                       'tests/install_payload_host.c', 'src/lib/install_payload.c',
                       'src/lib/crc32.c', 'user/tools/digest.c', '-o', str(binary)]
            manifest['commands'].append(command)
            with (out / 'host.log').open('w') as log:
                subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=90)
                command = [str(binary), str(disk)]
                manifest['commands'].append(command)
                try:
                    subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=1200)
                finally:
                    if disk.is_file() and not disk.is_symlink():
                        shutil.copyfile(disk, out / 'target.img')
            assert disk.is_file() and not disk.is_symlink()
            with disk.open('rb') as stream:
                sectors = disk.stat().st_size // 512
                stream.seek(0)
                mbr = stream.read(512)
                assert mbr[510:] == b'\x55\xaa' and mbr[450] == 0xee
                assert struct.unpack_from('<II', mbr, 454) == (1, sectors - 1)
                tables = []
                for current, backup, array in ((1, sectors - 1, 2), (sectors - 1, 1, sectors - 33)):
                    stream.seek(current * 512)
                    h = bytearray(stream.read(512))
                    assert h[:8] == b'EFI PART'
                    size, crc = struct.unpack_from('<II', h, 12)
                    assert size == 92
                    struct.pack_into('<I', h, 16, 0)
                    assert binascii.crc32(h[:size]) == crc
                    assert struct.unpack_from('<QQQQ', h, 24) == (current, backup, 34, sectors - 34)
                    assert struct.unpack_from('<QII', h, 72) == (array, 128, 128)
                    stream.seek(array * 512)
                    entries = stream.read(16384)
                    assert binascii.crc32(entries) == struct.unpack_from('<I', h, 88)[0]
                    tables.append(entries)
                assert tables[0] == tables[1]
                roles = [(4096, 131072, 'c12a7328-f81f-11d2-ba4b-00a0c93ec93b'),
                         (135168, 524288, '0fc63daf-8483-4772-8e79-3d69d8477de4'),
                         (2048, 2048, '21686148-6449-6e6f-744e-656564454649')]
                ids = set()
                for index, (start, count, kind) in enumerate(roles):
                    entry = tables[0][index * 128:(index + 1) * 128]
                    assert str(uuid.UUID(bytes_le=entry[:16])) == kind
                    guid = uuid.UUID(bytes_le=entry[16:32])
                    assert guid.version == 4 and guid not in ids
                    ids.add(guid)
                    assert struct.unpack_from('<QQ', entry, 32) == (start, start + count - 1)
                    stream.seek(start * 512)
                    for offset in range(0, count * 512, 1024 * 1024):
                        data = stream.read(min(1024 * 1024, count * 512 - offset))
                        expected = bytearray(bytes((index + i) % 256 for i in range(256)) * (len(data) // 256))
                        if index == 0 and offset == 0:
                            expected[4096:4608] = b'synthetic-readiness-workbench'.ljust(512, b'\0')
                        assert data == expected, (index, offset)
                assert not any(tables[0][384:])
            manifest['disk_sha256'] = sha(disk)
        assert all(sha(ROOT / p) == h for p, h in manifest['sources'].items())
        assert all(sha(Path(p)) == h for p, h in normal.items())
        manifest['result'] = 'PASS'
        print((out / 'archive.log').read_text().strip())
        print((out / 'host.log').read_text().strip())
        print('PASS independent GPT/MBR/payload oracle:', out)
    finally:
        (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    main()
