"""Selected actual EXT4 traces for the conservative six-record /etc profile.
Uses the retained namespace campaign's source snapshot and regular-file seeds.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import shutil
import struct
import subprocess
import sys
import time

from test_jbd2_replay_host import crc

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT/'tests'))
from pkg_namespace_admission import admit


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('campaign', type=Path)
    previous = parser.parse_args().campaign.resolve()
    assert previous.is_relative_to((ROOT/'.codex-remote-attachments').resolve())
    parent = json.loads((previous/'manifest.json').read_text())
    assert parent['result'] == 'PASS'
    out = ROOT/'.codex-remote-attachments'/('pkg-namespace-admission-ext4-'+time.strftime('%Y%m%d-%H%M%S'))
    work = out/'workspace'
    shutil.copytree(previous/'workspace', work)
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    manifest = {'result': 'INCOMPLETE', 'derived_from': str(previous), 'cases': [],
                'scope': 'selected actual-code restart traces; candidate /etc classifier only',
                'boot_acceptance': 'NOT RUN', 'runtime_admission': 'NOT IMPLEMENTED'}
    for name, digest in parent['sources'].items():
        assert sha(work/name) == digest
    for name in ('scripts/test_pkg_namespace_admission_ext4.py', 'tests/pkg_namespace_admission.py',
                 'tests/pkg_account_replay_model.py', 'scripts/test_jbd2_replay_host.py'):
        target = out/'gate-sources'/name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT/name, target)
    manifest['gate_sources'] = {str(p.relative_to(out/'gate-sources')): sha(p)
                                for p in (out/'gate-sources').rglob('*.py')}

    def run(cmd, label):
        result = subprocess.run(cmd, cwd=work, capture_output=True, text=True, timeout=240)
        with (out/(label+'.log')).open('a') as stream:
            stream.write(result.stdout+result.stderr)
        result.check_returncode()
        return result.stdout

    def audit(image, label, complete):
        run(['e2fsck', '-fn', str(image)], label+'-linux')
        expected = [('/sentinel', b'unrelated fixture bytes\n', 0o644)]
        for i, name in enumerate(('passwd', 'group', 'shadow')):
            old = b'root:!:::::::\n'
            expected.append(('/etc/'+name, bytes([65+i])*8192 if complete or i < 2 else old,
                             0o600 if i == 2 else 0o644))
            if not complete:
                expected.append(('/var/lib/fortress/accounts/'+name+'.new', bytes([65+i])*8192, 0o600))
        for path, data, mode in expected:
            dump = out/(label+'-'+path.replace('/', '_')+'.dump')
            run(['debugfs', '-R', f'dump {path} {dump}', str(image)], label+'-linux')
            assert dump.read_bytes() == data
            stat = run(['debugfs', '-R', f'stat {path}', str(image)], label+'-linux')
            assert int(re.search(r'Mode:\s+([0-7]+)', stat)[1], 8) == mode
            assert re.search(r'User:\s+0\s+Group:\s+0', stat)
        if complete:
            for name in ('passwd.publish', 'group.publish', 'shadow.publish'):
                assert not run(['debugfs', '-R', f'stat /etc/{name}', str(image)], label+'-linux').strip()
            assert not run(['debugfs', '-R', 'stat /var/lib/fortress/accounts/pending', str(image)], label+'-linux').strip()

    try:
        run(['gcc', '-std=c11', '-O1', '-g', '-fsanitize=address,undefined', '-Wall', '-Wextra',
             '-Werror', '-no-pie', '-pthread', '-Itests/ext4_host', '-Itests/host', '-Isrc/include',
             '-Isrc/fs', '-Isrc/drivers', '-Isrc/mm', '-Isrc/kernel', '-Isrc/arch/x86_64',
             'tests/pkg_account_namespace_host.c', 'tests/ext4_fault_disk.c', 'src/lib/account_redo.c',
             'user/tools/digest.c', '-o', str(work/'admission-probe')], 'build')
        for bs in (1024, 2048, 4096):
            original = previous/str(bs)/f'base-{bs}.img'
            original_sha = sha(original)
            mapping = [int(n) for n in re.findall(r'^(\d+)\s*$', (previous/str(bs)/'mapping.log').read_text(), re.M)]
            fixture_log = (previous/str(bs)/'fixture.log').read_text()
            match = re.search(r'Inode:\s+(\d+)\s+Type:\s+directory.*?Generation:\s+(\d+)', fixture_log, re.S)
            inode, generation = map(int, match.groups())

            def scan(image):
                disk = image.read_bytes()
                seed = crc(crc(crc(0xffffffff, disk[1128:1144]), struct.pack('<I', inode)), struct.pack('<I', generation))
                slots = []
                roles = {'passwd': 0, 'group': 1, 'shadow': 2,
                         'passwd.publish': 6, 'group.publish': 7, 'shadow.publish': 8}
                for physical in mapping:
                    block = disk[physical*bs:(physical+1)*bs]
                    assert crc(seed, block[:-12]) == struct.unpack_from('<I', block, bs-4)[0]
                    at = 0
                    while at < bs-12:
                        ino, rec, length, _ = struct.unpack_from('<IHBB', block, at)
                        assert rec >= 12 and rec % 4 == 0 and at+rec <= bs-12
                        name = block[at+8:at+8+length].decode() if ino else ''
                        used = (8+length+3)&~3 if ino else 0
                        slots.append((rec, used, roles.get(name, -2) if ino else -1))
                        at += rec
                return slots, seed

            slots, seed = scan(original)
            assert not admit(slots) and sha(original) == original_sha
            disk = bytearray(original.read_bytes())
            physical = mapping[0]
            block = bytearray(disk[physical*bs:(physical+1)*bs])
            at, merged = 0, 0
            while at < bs-12 and merged < 3:
                ino, rec = struct.unpack_from('<IH', block, at)
                if not ino and rec == 12 and struct.unpack_from('<IH', block, at+12) == (0, 12):
                    block[at:at+24] = bytes(24)
                    struct.pack_into('<H', block, at+4, 24)
                    rec = 24
                    merged += 1
                at += rec
            assert merged == 3
            struct.pack_into('<I', block, bs-4, crc(seed, block[:-12]))
            disk[physical*bs:(physical+1)*bs] = block
            base = out/f'admitted-{bs}.img'
            base.write_bytes(disk)
            assert admit(scan(base)[0])
            run(['e2fsck', '-fn', str(base)], f'{bs}-seed-linux')
            for sector in (512, 4096):
                current = base
                for mode in ('shadow', 'group', 'recover', 'recover'):
                    suffix = mode if mode != 'recover' or 'recover' not in current.name else 'recover-again'
                    label = f'{bs}-{sector}-{suffix}'
                    image = out/(label+'.img')
                    shutil.copyfile(current, image)
                    assert admit(scan(image)[0])
                    run([str(work/'admission-probe'), str(image), str(sector), mode], label)
                    assert admit(scan(image)[0])
                    audit(image, label, mode == 'recover')
                    manifest['cases'].append({'name': label, 'result': 'PASS', 'image_sha256': sha(image),
                                              'admission_before_after': 'ACCEPT'})
                    current = image
                assert sha(original) == original_sha
                print('PASS', bs, sector, 'admission + two interruptions + completion + repeat', flush=True)
        manifest['result'] = 'PASS'
    except Exception as error:
        manifest.update(result='FAIL', error=repr(error))
        raise
    finally:
        manifest['gate_source_preservation'] = all(sha(ROOT/name) == digest for name, digest in manifest['gate_sources'].items())
        (out/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    print('PASS', len(manifest['cases']), 'states; evidence:', out)


if __name__ == '__main__':
    main()
