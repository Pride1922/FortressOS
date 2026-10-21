"""Reproduce the replay model's namespace rejection on actual EXT4, privately.
No kernel/media build, mounts, physical devices or production behavior edits.
"""
from pathlib import Path
import hashlib
import json
import re
import shutil
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parent.parent


def main():
    out = ROOT/'.codex-remote-attachments'/('pkg-account-namespace-'+time.strftime('%Y%m%d-%H%M%S'))
    work = out/'workspace'
    work.mkdir(parents=True)
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    paths = [p for folder in ('src', 'user', 'tests', 'scripts') for p in (ROOT/folder).rglob('*')
             if p.is_file() and p.suffix in ('.c', '.h', '.inc', '.py') and '__pycache__' not in p.parts]
    sources = {str(p.relative_to(ROOT)): sha(p) for p in paths}
    manifest = {'result': 'INCOMPLETE', 'sources': sources, 'cases': [],
                'scope': 'actual EXT4/JBD2/redo, synthetic account validation, durable callback interruptions',
                'boot_acceptance': 'NOT RUN', 'physical_power_loss': 'NOT TESTED'}
    for name, digest in sources.items():
        dest = work/name
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT/name, dest)
        assert sha(dest) == digest

    def run(cmd, log):
        result = subprocess.run(cmd, cwd=work, capture_output=True, text=True, timeout=240)
        with log.open('a') as stream:
            stream.write(result.stdout+result.stderr)
        result.check_returncode()
        return result.stdout

    from test_ext4_time_host import fixture
    from test_jbd2_replay_host import crc
    old = (b'root:x:0:0:root:/root:/bin/shell\n', b'root:x:0:\n', b'root:!:::::::\n')
    names = ('passwd', 'group', 'shadow')

    def audit(image, label, expected):
        log = out/(label+'-linux.log')
        run(['e2fsck', '-fn', str(image)], log)
        for path, data, mode in expected:
            dump = out/(label+'-'+path.replace('/', '_')+'.dump')
            run(['debugfs', '-R', f'dump {path} {dump}', str(image)], log)
            assert dump.read_bytes() == data, (label, path)
            stat = run(['debugfs', '-R', f'stat {path}', str(image)], log)
            assert int(re.search(r'Mode:\s+([0-7]+)', stat)[1], 8) == mode
            assert re.search(r'User:\s+0\s+Group:\s+0', stat)
        return {'name': label, 'result': 'PASS', 'image_sha256': sha(image)}

    try:
        run(['gcc', '-std=c11', '-O1', '-g', '-fsanitize=address,undefined', '-Wall', '-Wextra',
             '-Werror', '-no-pie', '-pthread', '-Itests/ext4_host', '-Itests/host', '-Isrc/include',
             '-Isrc/fs', '-Isrc/drivers', '-Isrc/mm', '-Isrc/kernel', '-Isrc/arch/x86_64',
             'tests/pkg_account_namespace_host.c', 'tests/ext4_fault_disk.c', 'src/lib/account_redo.c',
             'user/tools/digest.c', '-o', str(work/'probe')], out/'build.log')
        for bs in (1024, 2048, 4096):
            folder = out/str(bs)
            folder.mkdir()
            base = fixture(folder, bs, inodes=2048)
            commands = ['mkdir /etc', 'mkdir /var', 'mkdir /root', 'set_inode_field /root mode 040700']
            for i, name in enumerate(names):
                seed = folder/name
                seed.write_bytes(old[i])
                commands += [f'write {seed} /etc/{name}',
                             f'set_inode_field /etc/{name} mode 0100{600 if i == 2 else 644}']
            seed = folder/'sentinel'
            seed.write_bytes(b'unrelated fixture bytes\n')
            commands += [f'write {seed} /sentinel', 'set_inode_field /sentinel mode 0100644']
            empty = folder/'empty'
            empty.write_bytes(b'')
            fillers_per_block = ((bs-12) % 12)//4
            filler_count = (1048576//bs)*fillers_per_block
            # Real distinct inodes, temporarily unlinked by debugfs, then each
            # linked exactly once by the offline directory layout below.
            for i in range(filler_count):
                commands += [f'write {empty} /f{i:07d}', f'unlink /f{i:07d}']
            commands += ['expand_dir /etc']*(1048576//bs-1)
            script = folder/'initialize.commands'
            script.write_text('\n'.join(commands)+'\n')
            text = run(['debugfs', '-w', '-f', str(script), str(base)], folder/'fixture.log')
            filler_inodes = [int(n) for n in re.findall(r'Allocated inode:\s+(\d+)', text)][-filler_count:]
            assert len(filler_inodes) == filler_count and len(set(filler_inodes)) == filler_count
            stat = run(['debugfs', '-R', 'stat /etc', str(base)], folder/'fixture.log')
            assert int(re.search(r'Size:\s+(\d+)', stat)[1]) == 1048576
            ino = int(re.search(r'Inode:\s+(\d+)', stat)[1])
            generation = int(re.search(r'Generation:\s+(\d+)', stat)[1])
            inodes = []
            for name in names:
                text = run(['debugfs', '-R', f'stat /etc/{name}', str(base)], folder/'fixture.log')
                inodes.append(int(re.search(r'Inode:\s+(\d+)', text)[1]))
            script = folder/'mapping.commands'
            script.write_text(''.join(f'bmap /etc {i}\n' for i in range(1048576//bs)))
            text = run(['debugfs', '-f', str(script), str(base)], folder/'mapping.log')
            mapping = [int(n) for n in re.findall(r'^(\d+)\s*$', text, re.M)]
            assert len(mapping) == 1048576//bs and len(set(mapping)) == len(mapping) and all(mapping)
            disk = bytearray(base.read_bytes())
            checksum_seed = crc(crc(crc(0xffffffff, disk[1128:1144]), struct.pack('<I', ino)),
                                struct.pack('<I', generation))
            for index, physical in enumerate(mapping):
                block = bytearray(bs)
                at = 0
                def entry(inode, length, name=b'', kind=1):
                    nonlocal at
                    struct.pack_into('<IHBB', block, at, inode, length, len(name), kind if inode else 0)
                    block[at+8:at+8+len(name)] = name
                    at += length
                if index == 0:
                    entry(ino, 12, b'.', 2)
                    entry(2, 12, b'..', 2)
                    for inode, name in zip(inodes, names):
                        entry(0, 24)
                        entry(inode, 16, name.encode())
                # Linux requires >=12-byte unused records. Exact-size occupied
                # filler records absorb the modulus with no allocatable slack.
                for j in range(fillers_per_block):
                    k = index*fillers_per_block+j
                    entry(filler_inodes[k], 16, f'f{k:07d}'.encode())
                while at < bs-12:
                    entry(0, 12)
                assert at == bs-12
                struct.pack_into('<IHBBI', block, at, 0, 12, 0, 0xde, crc(checksum_seed, block[:at]))
                disk[physical*bs:(physical+1)*bs] = block
            base.write_bytes(disk)
            run(['e2fsck', '-fn', str(base)], folder/'fixture.log')
            base_sha = sha(base)
            for sector in (512, 4096):
                prefix = f'{bs}-{sector}'
                common = [('/sentinel', b'unrelated fixture bytes\n', 0o644)]
                canon = lambda changed: [('/etc/'+name, bytes([65+i])*8192 if i in changed else old[i],
                                          0o600 if i == 2 else 0o644) for i, name in enumerate(names)]
                control = out/(prefix+'-control.img')
                shutil.copyfile(base, control)
                run([str(work/'probe'), str(control), str(sector), 'update'], out/(prefix+'-control.log'))
                manifest['cases'].append(audit(control, prefix+'-control', common+canon(range(3))))
                for name in names:
                    assert not run(['debugfs', '-R', f'stat /etc/{name}.publish', str(control)],
                                   out/(prefix+'-control-linux.log')).strip()
                current = base
                for mode in ('shadow', 'group', 'blocked', 'blocked'):
                    suffix = mode if mode != 'blocked' or 'blocked' not in str(current.name) else 'blocked-again'
                    image = out/(prefix+'-'+suffix+'.img')
                    shutil.copyfile(current, image)
                    run([str(work/'probe'), str(image), str(sector), mode], out/(prefix+'-'+suffix+'.log'))
                    stages = [('/var/lib/fortress/accounts/'+name+'.new', bytes([65+i])*8192, 0o600)
                              for i, name in enumerate(names)]
                    temps = [('/etc/shadow.publish', b'', 0o600)]
                    if mode != 'shadow': temps += [('/etc/group.publish', b'', 0o644)]
                    pending = out/(prefix+'-pending.dump')
                    run(['debugfs', '-R', f'dump /var/lib/fortress/accounts/pending {pending}', str(image)],
                        out/(prefix+'-pending.log'))
                    assert len(pending.read_bytes()) == 160
                    if mode == 'shadow': pending_bytes = pending.read_bytes()
                    assert pending.read_bytes() == pending_bytes
                    expected = common+canon((0, 1))+stages+temps+[
                        ('/var/lib/fortress/accounts/pending', pending_bytes, 0o600)]
                    assert not run(['debugfs', '-R', 'stat /etc/passwd.publish', str(image)],
                                   out/(prefix+'-'+suffix+'-linux.log')).strip()
                    manifest['cases'].append(audit(image, prefix+'-'+suffix, expected))
                    current = image
                assert sha(base) == base_sha
                print('PASS', prefix, 'control + two durable interruptions + two blocked remounts', flush=True)
        manifest['result'] = 'PASS'
    except Exception as error:
        manifest['result'] = 'FAIL'
        manifest['error'] = repr(error)
        raise
    finally:
        manifest['source_preservation'] = {name: sha(ROOT/name) == digest for name, digest in sources.items()}
        (out/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    print('PASS', len(manifest['cases']), 'states; evidence:', out)


if __name__ == '__main__':
    main()
