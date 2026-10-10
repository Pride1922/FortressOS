"""Existing mounted EXT4 lifecycle/recovery tests on a journaled production copy."""
import datetime
import json
from pathlib import Path
import subprocess
from test_memory_ext4 import ROOT, digest
from test_ext4_mount_host import audit_bytes
from test_jbd2_replay_host import oracle


def main():
    out = ROOT / 'build' / ('memory-node-mount-' + datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ'))
    out.mkdir(parents=True)
    source = ROOT / 'bin/fortress.img'
    original = digest(source)
    record = {'status': 'FAIL', 'source_sha256': original, 'commands': []}
    def run(argv, name):
        record['commands'].append(argv)
        with (out / name).open('w') as log:
            subprocess.run(argv, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, timeout=180, check=True)
    try:
        image = out / 'source.ext4'
        with source.open('rb') as stream:
            stream.seek(133120 * 512)
            data = stream.read(131072 * 512)
        assert len(data) == 131072 * 512
        bs = 1024 << int.from_bytes(data[1048:1052], 'little')
        image.write_bytes(data)
        run(['dumpe2fs', '-h', str(image)], 'features.log')
        assert 'has_journal' in (out / 'features.log').read_text()
        seed = out / 'seed.bin'
        seed.write_bytes(b'O' * (3 * bs))
        run(['debugfs', '-w', '-R', f'write {seed} /target.bin', str(image)], 'seed.log')
        binary = out / 'mount-host'
        run(['gcc', '-std=c11', '-O1', '-g', '-fsanitize=address,undefined', '-Wall', '-Wextra', '-Werror',
             '-no-pie', '-pthread', '-Itests/ext4_host', '-Itests/host', '-Isrc/include', '-Isrc/fs',
             '-Isrc/drivers', '-Isrc/mm', 'tests/ext4_mount_host.c', 'tests/ext4_fault_disk.c',
             '-o', str(binary)], 'compile.log')
        prefix = out / 'mount'
        run([str(binary), str(image), '512', str(prefix), '--smoke'], 'mount.log')
        for suffix in ('lifecycle-clean', 'pending-seed', 'directory-cache-clean'):
            saved = Path(str(prefix) + '-' + suffix + '.img')
            oracle(saved, Path(str(saved) + '.linux.img'), Path(str(saved) + '.linux.log'))
            audit_bytes(saved, suffix, bs)
        record['status'] = 'PASS'
    finally:
        record['source_sha256_after'] = digest(source)
        if record['source_sha256_after'] != original:
            record['status'] = 'FAIL'
        (out / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
        print(f"Mounted node regression {record['status']}: {out}", flush=True)
        assert record['source_sha256_after'] == original


if __name__ == '__main__':
    main()
