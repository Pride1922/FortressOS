"""Actual opt-in EXT4 namespace scanner/fence on private regular-file copies."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import shutil
import struct
import subprocess
import time

from test_jbd2_replay_host import crc

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('campaign', type=Path)
    previous = parser.parse_args().campaign.resolve()
    assert previous.is_relative_to((ROOT/'.codex-remote-attachments').resolve())
    assert json.loads((previous/'manifest.json').read_text())['result'] == 'PASS'
    out = ROOT/'.codex-remote-attachments'/('pkg-namespace-kernel-'+time.strftime('%Y%m%d-%H%M%S'))
    work = out/'workspace'
    work.mkdir(parents=True)
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    paths = [p for folder in ('src', 'user', 'tests', 'scripts') for p in (ROOT/folder).rglob('*')
             if p.is_file() and p.suffix in ('.c', '.h', '.inc', '.py') and '__pycache__' not in p.parts]
    sources = {str(p.relative_to(ROOT)): sha(p) for p in paths}
    for name, digest in sources.items():
        target = work/name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT/name, target)
        assert sha(target) == digest
    manifest = {'result': 'INCOMPLETE', 'derived_from': str(previous), 'sources': sources, 'cases': [],
                'scope': 'host actual EXT4/JBD2/redo + opt-in scanner/fence; no production caller',
                'boot_acceptance': 'NOT RUN', 'durable_package_grant': 'NOT IMPLEMENTED'}

    def run(cmd, label):
        result = subprocess.run(cmd, cwd=work, capture_output=True, text=True, timeout=240)
        with (out/(label+'.log')).open('a') as stream:
            stream.write(result.stdout+result.stderr)
        result.check_returncode()
        return result.stdout

    def stat(image, path, label):
        return run(['debugfs', '-R', 'stat '+path, str(image)], label)

    def seal_directory(image, bs, path, edits=None):
        text = stat(image, path, 'seed-metadata')
        ino = int(re.search(r'Inode:\s+(\d+)', text)[1])
        generation = int(re.search(r'Generation:\s+(\d+)', text)[1])
        physical = int(run(['debugfs', '-R', f'bmap {path} 0', str(image)], 'seed-metadata').strip())
        disk = bytearray(image.read_bytes())
        seed = crc(crc(crc(0xffffffff, disk[1128:1144]), struct.pack('<I', ino)), struct.pack('<I', generation))
        block = bytearray(disk[physical*bs:(physical+1)*bs])
        edits(block)
        struct.pack_into('<I', block, bs-4, crc(seed, block[:-12]))
        disk[physical*bs:(physical+1)*bs] = block
        image.write_bytes(disk)

    def split_slot(block):
        at = 0
        while at < len(block)-12:
            ino, rec = struct.unpack_from('<IH', block, at)
            if not ino and rec == 24:
                block[at:at+24] = bytes(24)
                struct.pack_into('<H', block, at+4, 12)
                struct.pack_into('<H', block, at+16, 12)
                return
            at += rec
        raise AssertionError('no free exact slot')

    def audit(image, label, complete):
        run(['e2fsck', '-fn', str(image)], label+'-linux')
        expected = [('/sentinel', b'unrelated fixture bytes\n', 0o644)]
        for i, name in enumerate(('passwd', 'group', 'shadow')):
            data = bytes([65+i])*8192 if complete or i < 2 else b'root:!:::::::\n'
            expected.append(('/etc/'+name, data, 0o600 if i == 2 else 0o644))
            if not complete:
                expected.append(('/var/lib/fortress/accounts/'+name+'.new', bytes([65+i])*8192, 0o600))
        for path, data, mode in expected:
            dump = out/(label+'-'+path.replace('/', '_')+'.dump')
            run(['debugfs', '-R', f'dump {path} {dump}', str(image)], label+'-linux')
            assert dump.read_bytes() == data
            text = stat(image, path, label+'-linux')
            assert int(re.search(r'Mode:\s+([0-7]+)', text)[1], 8) == mode
            assert re.search(r'User:\s+0\s+Group:\s+0', text)
        if complete:
            assert not stat(image, '/var/lib/fortress/accounts/pending', label+'-linux').strip()
            for name in ('passwd', 'group', 'shadow'):
                assert not stat(image, '/etc/'+name+'.publish', label+'-linux').strip()

    try:
        run(['gcc', '-std=c11', '-O1', '-g', '-fsanitize=address,undefined', '-fstack-usage',
             '-Wall', '-Wextra', '-Werror', '-no-pie', '-pthread', '-DTEST_PKG_NAMESPACE_ADMISSION',
             '-Itests/ext4_host', '-Itests/host', '-Isrc/include', '-Isrc/fs', '-Isrc/drivers',
             '-Isrc/mm', '-Isrc/kernel', '-Isrc/arch/x86_64', 'tests/pkg_account_namespace_host.c',
             'tests/ext4_fault_disk.c', 'src/lib/account_redo.c', 'user/tools/digest.c',
             '-o', str(work/'probe')], 'build')
        frames = []
        for file in work.glob('*.su'):
            for line in file.read_text().splitlines():
                if 'e4_ns_' in line or 'ext4_root_account_namespace_admit' in line:
                    parts = line.split('\t')
                    assert int(parts[1]) <= 1024
                    frames.append(line)
        assert frames
        manifest['scanner_stack_frames'] = frames
        for bs in (1024, 2048, 4096):
            source = previous/f'admitted-{bs}.img'
            source_sha = sha(source)
            base = out/f'admitted-{bs}.img'
            shutil.copyfile(source, base)
            run([str(work/'probe'), str(base), '512', 'setup'], f'{bs}-setup')
            text = stat(base, '/var/lib/fortress/accounts', 'seed-metadata')
            self_ino = int(re.search(r'Inode:\s+(\d+)', text)[1])
            parent_ino = int(re.search(r'Inode:\s+(\d+)', stat(base, '/var/lib/fortress', 'seed-metadata'))[1])
            assert int(re.search(r'Size:\s+(\d+)', text)[1]) == bs

            def initialize(block):
                block[:] = bytes(bs)
                struct.pack_into('<IHBB', block, 0, self_ino, 12, 1, 2)
                block[8:9] = b'.'
                struct.pack_into('<IHBB', block, 12, parent_ino, 12, 2, 2)
                block[20:22] = b'..'
                at = 24
                for _ in range(5):
                    struct.pack_into('<H', block, at+4, 24)
                    at += 24
                while at < bs-12:
                    size = 16 if (bs-12-at) % 12 else 12
                    struct.pack_into('<H', block, at+4, size)
                    at += size
                assert at == bs-12
                struct.pack_into('<IHBB', block, at, 0, 12, 0, 0xde)
            seal_directory(base, bs, '/var/lib/fortress/accounts', initialize)
            run(['e2fsck', '-fn', str(base)], f'{bs}-seed-linux')
            base_sha = sha(base)
            for sector in (512, 4096):
                prefix = f'{bs}-{sector}'
                for kind in ('legacy', 'oom', 'read-error', 'missing-private', 'etc-five', 'private-four', 'private-mode', 'role-mode'):
                    label = prefix+'-'+kind
                    image = out/(label+'.img')
                    shutil.copyfile(source if kind == 'missing-private' else base, image)
                    if kind in ('etc-five', 'private-four'):
                        seal_directory(image, bs, '/etc' if kind == 'etc-five' else '/var/lib/fortress/accounts', split_slot)
                    if kind in ('private-mode', 'role-mode'):
                        path = '/var/lib/fortress/accounts' if kind == 'private-mode' else '/etc/passwd'
                        mode = '040755' if kind == 'private-mode' else '0100600'
                        run(['debugfs', '-w', '-R', f'set_inode_field {path} mode {mode}', str(image)], label+'-fixture')
                    mode = kind if kind in ('legacy', 'oom', 'read-error') else 'reject'
                    run([str(work/'probe'), str(image), str(sector), mode], label)
                    run(['e2fsck', '-fn', str(image)], label+'-linux')
                    manifest['cases'].append({'name': label, 'result': 'PASS', 'image_sha256': sha(image)})
                current = base
                for mode in ('shadow', 'group', 'recover', 'recover'):
                    suffix = mode if mode != 'recover' or 'recover' not in current.name else 'recover-again'
                    label = prefix+'-'+suffix
                    image = out/(label+'.img')
                    shutil.copyfile(current, image)
                    run([str(work/'probe'), str(image), str(sector), mode], label)
                    audit(image, label, mode == 'recover')
                    manifest['cases'].append({'name': label, 'result': 'PASS', 'image_sha256': sha(image)})
                    current = image
                print('PASS', prefix, 'scanner/fence/redo + negative controls', flush=True)
                assert sha(base) == base_sha
            assert sha(source) == source_sha
        manifest['result'] = 'PASS'
    except Exception as error:
        manifest.update(result='FAIL', error=repr(error))
        raise
    finally:
        manifest['source_changes'] = [name for name, digest in sources.items() if sha(ROOT/name) != digest]
        (out/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    print('PASS', len(manifest['cases']), 'states; evidence:', out)


if __name__ == '__main__':
    main()
