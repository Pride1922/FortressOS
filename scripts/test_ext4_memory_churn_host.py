"""Bounded journaled EXT4 cache churn on a copy of the normal image partition."""
import datetime
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]

def main():
    assert sys.argv[1:] in ([], ['--api'])
    out = ROOT / 'build' / ('memory-churn-' + datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ'))
    out.mkdir(parents=True)
    source = ROOT / 'bin/fortress.img'
    protected = ROOT / 'docs/plans/PERMISSIONS_PLAN.md'
    digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    before = {str(p): digest(p) for p in (source, protected)}
    record = {'status': 'FAIL', 'before': before, 'commands': []}
    def run(argv, name):
        record['commands'].append(argv)
        with (out / name).open('w') as log:
            subprocess.run(argv, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=900)
    try:
        # Production GPT partition 2; no formatting or fallback filesystem.
        image = out / 'source.ext4'
        with source.open('rb') as stream:
            stream.seek(133120 * 512)
            data = stream.read(131072 * 512)
        assert len(data) == 131072 * 512
        image.write_bytes(data)
        run(['dumpe2fs', '-h', str(image)], 'features.log')
        features = (out / 'features.log').read_text()
        assert 'has_journal' in features and 'extent' in features
        binary = out / 'churn-host'
        run(['gcc', '-std=c11', '-O1', '-g', '-fsanitize=address,undefined', '-Wall', '-Wextra', '-Werror',
             '-no-pie', '-pthread', '-Itests/ext4_host', '-Itests/host', '-Isrc/include', '-Isrc/fs',
             '-Isrc/drivers', '-Isrc/mm', 'tests/ext4_memory_churn_host.c', 'tests/ext4_fault_disk.c',
             '-o', str(binary)], 'compile.log')
        run([str(binary), str(image), str(out / 'churn')] + sys.argv[1:], 'churn.log')
        for suffix in ('exhausted-clean', 'remount-clean'):
            saved = out / ('churn-' + suffix + '.img')
            run(['e2fsck', '-fn', str(saved)], suffix + '-fsck.log')
            run(['debugfs', '-R', 'stat /memory-churn.bin', str(saved)], suffix + '-absent.log')
            assert 'File not found' in (out / (suffix + '-absent.log')).read_text()
        record['status'] = 'PASS'
    finally:
        record['after'] = {str(p): digest(p) for p in (source, protected)}
        if record['after'] != before:
            record['status'] = 'FAIL'
        (out / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
        print(f"EXT4 memory churn {record['status']}: {out}", flush=True)
        assert record['after'] == before

if __name__ == '__main__':
    main()
