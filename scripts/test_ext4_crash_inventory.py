"""Phase 9.1 mounted baseline inventory; no exhaustive crash claim."""
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import struct
import sys
import tempfile
import time
from create_ext4_fixtures import ROOT
from ext4_crash_model import replay, validate_snapshot, audit_barriers

OPS = ('create', 'mkdir', 'write', 'append', 'rename', 'truncate', 'unlink',
       'open-unlink', 'last-close', 'rmdir', 'reuse', 'sync', 'freeze')


def command(argv, log):
    result = subprocess.run(argv, capture_output=True, timeout=180)
    with log.open('ab') as stream:
        stream.write((json.dumps(argv) + '\n').encode() + result.stdout + result.stderr)
    assert result.returncode == 0, (argv, result.returncode, str(log))
    return (result.stdout + result.stderr).decode(errors='replace')


def linux_snapshot(image, bs, op, log, committed=True, deep_size=None):
    # Read-only fsck, never repair the output to make an audit pass.
    command(['e2fsck', '-fn', str(image)], log)
    path = '/sub/renamed.bin' if op == 'rename' and committed else '/reuse.bin' if op == 'reuse' else '/target.bin'
    expected = (b'I' * (2 * bs) if committed else b'') if op == 'reuse' else b'O' * (bs + 17) if op == 'truncate' and committed \
        else b'O' * (3 * bs) + b'I' * (2 * bs) if op == 'write' and committed \
        else b'O' * (3 * bs) + b'I' * 17 if op == 'append' and committed else b'O' * (3 * bs)
    if deep_size is not None:
        assert op in ('write','sync') and deep_size>=5*bs
        expected=b'O'*(3*bs)+(b'I'*(2*bs) if op=='write' and committed else bytes(2*bs))+bytes(deep_size-5*bs)
    absent = op == 'last-close' or committed and op in ('unlink', 'open-unlink')
    text = command(['debugfs', '-R', f'stat {path}', str(image)], log)
    observed = {'present': 'File not found' not in text}
    wanted = {'present': not absent}
    if not absent:
        dump = Path(str(image) + '.bytes')
        command(['debugfs', '-R', f'dump {path} {dump}', str(image)], log)
        observed.update(size=int(re.search(r'Size:\s+(\d+)', text)[1]),
                        links=int(re.search(r'Links:\s+(\d+)', text)[1]),
                        sha256=hashlib.sha256(dump.read_bytes()).hexdigest())
        wanted.update(size=len(expected), links=1, sha256=hashlib.sha256(expected).hexdigest())
        dump.unlink()
    validate_snapshot(observed, [wanted])
    for missing in (['/target.bin'] if op == 'reuse' or op == 'rename' and committed else ['/sub'] if op == 'rmdir' and committed else []):
        assert 'File not found' in command(['debugfs', '-R', f'stat {missing}', str(image)], log)
    if op in ('create', 'mkdir') and committed:
        name = '/new.bin' if op == 'create' else '/newdir'
        text = command(['debugfs', '-R', f'stat {name}', str(image)], log)
        assert 'Type: ' + ('regular' if op == 'create' else 'directory') in text
    namespace, owners = {}, {}
    pending = ['/']
    while pending:
        directory = pending.pop()
        listing = command(['debugfs', '-R', f'ls -p {directory}', str(image)], log)
        for line in listing.splitlines():
            fields = line.split('/')
            if len(fields) < 7 or not fields[1].isdigit() or int(fields[1]) == 0 or not fields[5] or fields[5] in ('.', '..'):
                continue
            name = directory.rstrip('/') + '/' + fields[5]
            kind = 'directory' if int(fields[2], 8) & 0o170000 == 0o040000 else 'file'
            assert name not in namespace
            stat = command(['debugfs', '-R', f'stat {name}', str(image)], log)
            block_text = command(['debugfs', '-R', f'blocks {name}', str(image)], log)
            allocated = [int(n) for line in block_text.splitlines() if re.fullmatch(r'\d+(?:\s+\d+)*\s*', line)
                         for n in line.split()]
            for block in allocated:
                assert block not in owners, ('double allocation', block, name)
                owners[block] = name
            namespace[name] = {'kind': kind, 'inode': int(fields[1]), 'blocks': allocated,
                               'links': int(re.search(r'Links:\s+(\d+)', stat)[1])}
            if kind == 'directory': pending.append(name)
    wanted_names = {'/lost+found': 'directory'}
    if not absent: wanted_names[path] = 'file'
    if op == 'rename' or op == 'rmdir' and not committed: wanted_names['/sub'] = 'directory'
    if op == 'create' and committed: wanted_names['/new.bin'] = 'file'
    if op == 'mkdir' and committed: wanted_names['/newdir'] = 'directory'
    validate_snapshot({n: v['kind'] for n, v in namespace.items()}, [wanted_names])
    for name, entry in namespace.items():
        links = 1 if entry['kind'] == 'file' else 2 + sum(
            other != name and Path(other).parent.as_posix() == name and v['kind'] == 'directory'
            for other, v in namespace.items())
        assert entry['links'] == links, (name, 'link count')
    root = command(['debugfs', '-R', 'stat /', str(image)], log)
    assert int(re.search(r'Links:\s+(\d+)', root)[1]) == 2 + sum(
        Path(name).parent.as_posix() == '/' and entry['kind'] == 'directory' for name, entry in namespace.items())
    data = image.read_bytes()
    assert struct.unpack_from('<I', data, 1256)[0] == 0, 'remaining traditional orphan'
    observed['namespace'] = namespace
    observed['allocated_data_owners'] = owners
    return observed


def retain_image(image):
    data = image.read_bytes()
    target = Path(str(image) + '.gz')
    with target.open('xb') as raw:
        with gzip.GzipFile(filename='', mode='wb', fileobj=raw, mtime=0) as stream:
            stream.write(data)
    assert gzip.decompress(target.read_bytes()) == data
    image.unlink()
    return {'file': target.name, 'sha256': hashlib.sha256(data).hexdigest()}


def main():
    assert len(sys.argv) == 2 or len(sys.argv)==3 and sys.argv[2]=='--write-only', 'explicit fixture directory [--write-only] required'
    operations = ('write',) if len(sys.argv)==3 else OPS
    fixture = Path(sys.argv[1]).resolve()
    fixture_manifest=json.loads((fixture/'manifest.json').read_text())
    evidence = Path(os.environ.get('FORTRESS_EXT4_CRASH_EVIDENCE', ROOT / '.codex-remote-attachments/ext4-phase9')).resolve()
    allowed = (ROOT / '.codex-remote-attachments').resolve()
    assert fixture.is_relative_to(allowed) and (fixture / 'manifest.json').is_file()
    assert evidence.is_relative_to(allowed)
    out = Path(tempfile.mkdtemp(prefix='foundation-', dir=evidence))
    manifest = {'argv': sys.argv, 'scope': 'baseline, not crash acceptance', 'operations': operations, 'cases': [], 'errors': []}
    started = time.monotonic()
    try:
        command([sys.executable, str(ROOT / 'scripts/test_ext4_crash_model.py')], out / 'calibration.log')
        for bs in (1024, 2048, 4096):
            for placement in ('normal', 'wrap'):
                source = fixture / f'{bs}-{placement}.img'
                assert source.is_file() and not source.is_symlink()
                for ss in (512, 4096):
                    label = f'{bs}-{placement}-{ss}'
                    prefix = out / label
                    argv = [str(evidence / 'bin/ext4_crash_inventory_host'), str(source), str(ss), str(prefix)]
                    if len(operations)==1: argv.append('--write-only')
                    command(argv, out / f'{label}.log')
                    for op in operations:
                        stem = Path(str(prefix) + '-' + op)
                        events = [json.loads(line) for line in Path(str(stem) + '.events.jsonl').read_text().splitlines()]
                        payload = Path(str(stem) + '.payload.bin').read_bytes()
                        before = Path(str(stem) + '-before.img'); after = Path(str(stem) + '-after.img')
                        reconstructed = replay(before.read_bytes(), ss, events, payload)
                        assert reconstructed.stable == after.read_bytes(), (label, op, 'trace reconstruction')
                        assert reconstructed.volatile == reconstructed.stable, (label, op, 'operation returned before barrier')
                        audit_barriers(before.read_bytes(), ss, bs, events, payload)
                        negative_controls = 0
                        for index, event in enumerate(events):
                            if event['kind'] != 'flush': continue
                            # Removing a commit barrier must be caught before the
                            # first metadata-home write, even if later flushes succeed.
                            prior = events[index - 1] if index else None
                            if prior and prior['state'] == 2 and any(
                                    e['sequence'] == prior['sequence'] and any(p['journal_type'] == 2 for p in e['parts'])
                                    for e in events[:index]):
                                try:
                                    audit_barriers(before.read_bytes(), ss, bs, events, payload, omit_flush=event['index'])
                                except AssertionError:
                                    negative_controls += 1
                                else:
                                    raise AssertionError((label, op, 'missing commit flush undetected'))
                        clean = Path(str(stem) + '-clean.img')
                        config=next((c for c in fixture_manifest.get('cases',[]) if c.get('block')==bs and c.get('placement')==placement),{})
                        deep_size=config.get('deep_size')
                        observed = linux_snapshot(clean, bs, op, Path(str(stem) + '.linux.log'),deep_size=deep_size)
                        record = {'block': bs, 'sector': ss, 'placement': placement, 'operation': op,
                                  'events': len(events), 'writes': sum(e['kind'] == 'write' for e in events),
                                  'flushes': sum(e['kind'] == 'flush' for e in events), 'snapshot': observed,
                                  'missing_commit_flush_controls': negative_controls,
                                  'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
                                  'payload_sha256': hashlib.sha256(payload).hexdigest(),
                                  'images': [retain_image(p) for p in (before, after, clean)]}
                        manifest['cases'].append(record)
                        if deep_size is not None:record['deep_size']=deep_size
                    print(f'PASS {label}: {len(operations)} operation inventories, reconstructed media, Linux audits', flush=True)
    except Exception as error:
        manifest['errors'].append(repr(error))
        raise
    finally:
        manifest['seconds'] = time.monotonic() - started
        manifest['binary_sha256'] = hashlib.sha256((evidence / 'bin/ext4_crash_inventory_host').read_bytes()).hexdigest()
        manifest['sources'] = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                               for p in [ROOT / 'tests/ext4_crash_inventory_host.c', ROOT / 'tests/ext4_integration_host.c',
                                         ROOT / 'scripts/ext4_crash_model.py', Path(__file__)]}
        (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        print(f'Evidence: {out}', flush=True)
    print(f'Phase 9.1 PASS: {len(manifest["cases"])} mounted operation inventories', flush=True)


if __name__ == '__main__':
    main()
