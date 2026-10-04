"""Negative controls on independent Linux observation, using disposable copies."""
import gzip
import json
from pathlib import Path
import struct
import re
import subprocess
import sys
import tempfile
from test_ext4_crash_inventory import linux_snapshot, command


def main():
    assert len(sys.argv) == 2
    root = Path(sys.argv[1]).resolve()
    manifest = json.loads((root / 'manifest.json').read_text())
    assert not manifest['errors'] and len(manifest['cases']) == 156
    case = next(c for c in manifest['cases'] if c['operation'] == 'write' and c['block'] == 1024 and c['sector'] == 512)
    entry = next(i for i in case['images'] if i['file'].endswith('-clean.img.gz'))
    initial = gzip.decompress((root / entry['file']).read_bytes())
    block = case['snapshot']['namespace']['/target.bin']['blocks'][0]
    bs = case['block']
    out = Path(tempfile.mkdtemp(prefix='oracle-negative-', dir=root))
    new_inodes = []
    for created in (c for c in manifest['cases'] if c['operation'] in ('create', 'mkdir')):
        entry = next(i for i in created['images'] if i['file'].endswith('-clean.img.gz'))
        temporary = out / 'new-inode-audit.img'
        temporary.write_bytes(gzip.decompress((root / entry['file']).read_bytes()))
        name = '/new.bin' if created['operation'] == 'create' else '/newdir'
        text = command(['debugfs', '-R', f'stat {name}', str(temporary)], out / 'new-inode-audit.log')
        size = int(re.search(r'Size:\s+(\d+)', text)[1])
        assert size == (0 if created['operation'] == 'create' else created['block'])
        assert int(re.search(r'Inode:\s+(\d+)', text)[1]) == created['snapshot']['namespace'][name]['inode']
        if created['operation'] == 'create':
            dump = out / 'empty-file.bytes'
            command(['debugfs', '-R', f'dump {name} {dump}', str(temporary)], out / 'new-inode-audit.log')
            assert dump.read_bytes() == b''
            dump.unlink()
        new_inodes.append({'image': entry['file'], 'name': name, 'size': size})
        temporary.unlink()
    altered = bytearray(initial); altered[block * bs] ^= 0x40
    image = out / 'stale-data.img'; image.write_bytes(altered)
    try:
        linux_snapshot(image, bs, 'write', out / 'stale-data.log')
    except AssertionError as error:
        assert str(error) == 'snapshot outside declared transaction boundaries'
    else:
        raise AssertionError('independent byte oracle accepted altered bytes')
    # Bitmap corruption: fsck must catch allocation disagreement without repair.
    altered = bytearray(initial)
    first = struct.unpack_from('<I', altered, 1044)[0]
    per_group = struct.unpack_from('<I', altered, 1056)[0]
    group, within = divmod(block - first, per_group)
    bitmap = struct.unpack_from('<I', altered, (first + 1) * bs + group * 32)[0]
    byte, bit = divmod(within, 8)
    assert altered[bitmap * bs + byte] & (1 << bit)
    altered[bitmap * bs + byte] &= ~(1 << bit)
    image = out / 'allocation-disagreement.img'; image.write_bytes(altered)
    result = subprocess.run(['e2fsck', '-fn', str(image)], capture_output=True, timeout=180)
    (out / 'allocation-disagreement.log').write_bytes(result.stdout + result.stderr)
    assert result.returncode == 4, ('expected uncorrected filesystem errors', result.returncode)
    assert image.read_bytes() == altered, 'read-only checker changed corrupt image'
    (out / 'manifest.json').write_text(json.dumps({'stale_data': 'byte oracle rejected after fsck exit 0',
        'allocation_disagreement': 'unmodified image, e2fsck -fn exit 4', 'new_inode_audits': new_inodes}, indent=2) + '\n')
    print('Independent Linux oracle negative controls PASS: stale bytes and allocation disagreement')
    print(f'New-inode audits PASS {len(new_inodes)}/24; evidence: {out}')


if __name__ == '__main__': main()
