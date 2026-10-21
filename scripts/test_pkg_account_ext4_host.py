"""Private actual EXT4 account resource observations, Linux oracle, no kernel/media build."""
from pathlib import Path
import hashlib
import json
import re
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent.parent


def main():
    out = ROOT/'.codex-remote-attachments'/('pkg-account-ext4-'+time.strftime('%Y%m%d-%H%M%S'))
    work = out/'workspace'
    work.mkdir(parents=True)
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    paths = [p for folder in ('src', 'user', 'tests', 'scripts') for p in (ROOT/folder).rglob('*')
             if p.is_file() and p.suffix in ('.c', '.h', '.inc', '.py') and '__pycache__' not in p.parts]
    before = {str(p.relative_to(ROOT)): sha(p) for p in paths}
    manifest = {'sources': before, 'cases': [], 'result': 'INCOMPLETE',
                'scope': 'host mounted actual EXT4/JBD2/redo, host device and validation adapters',
                'boot_acceptance': 'NOT RUN', 'grant_acceptance': 'NOT RUN'}
    for name, digest in before.items():
        dest = work/name
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT/name, dest)
        assert sha(dest) == digest
    def run(cmd, log, cwd=work):
        with log.open('a') as stream:
            result = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, timeout=180)
            stream.write(result.stdout)
            result.check_returncode()
            return result.stdout
    try:
        run(['gcc', '-std=c11', '-O1', '-g', '-fsanitize=address,undefined', '-Wall', '-Wextra',
             '-Werror', '-no-pie', '-pthread', '-Itests/ext4_host', '-Itests/host', '-Isrc/include',
             '-Isrc/fs', '-Isrc/drivers', '-Isrc/mm', '-Isrc/kernel', '-Isrc/arch/x86_64',
             'tests/pkg_account_ext4_host.c', 'tests/ext4_fault_disk.c', 'src/lib/account_redo.c',
             'user/tools/userdb.c', 'user/tools/digest.c', '-o', str(work/'probe')], out/'build.log')
        sys.path.insert(0, str(work/'scripts'))
        from test_ext4_time_host import fixture
        for bs in (1024, 2048, 4096):
            folder = out/str(bs)
            folder.mkdir()
            base = fixture(folder, bs, inodes=512)
            seed = folder/'seed.txt'
            commands = ['mkdir /etc', 'mkdir /var', 'mkdir /root', 'set_inode_field /root mode 040700']
            for name, data, mode in [('passwd', b'root:x:0:0:root:/root:/bin/shell\n', '0100644'),
                                     ('group', b'root:x:0:\n', '0100644'), ('shadow', b'root:!:::::::\n', '0100600')]:
                p = folder/name
                p.write_bytes(data)
                commands += [f'write {p} /etc/{name}', f'set_inode_field /etc/{name} mode {mode}']
            seed.write_bytes(b'unrelated fixture bytes\n')
            commands += [f'write {seed} /sentinel']
            for i in range(80): commands += [f'write {seed} /etc/fill-{i:03d}-'+'x'*32]
            command_file = folder/'initialize.commands'
            command_file.write_text('\n'.join(commands)+'\n')
            run(['debugfs', '-w', '-f', str(command_file), str(base)], folder/'fixture.log')
            run(['e2fsck', '-fn', str(base)], folder/'fixture.log')
            original = sha(base)
            for sector in (512, 4096):
                for kind in ('small', 'bytes', 'pins') + (('floor',) if bs == 4096 else ()):
                    name = f'{bs}-{sector}-{kind}'
                    image = folder/(name+'.img')
                    shutil.copyfile(base, image)
                    run([str(work/'probe'), str(image), str(sector), kind], out/(name+'.log'))
                    run(['e2fsck', '-fn', str(image)], out/(name+'-linux.log'))
                    for role, data in [('passwd', b'root:x:0:0:root:/root:/bin/shell\n'),
                                       ('group', b'root:x:0:\n'), ('shadow', b'root:!:::::::\n'),
                                       ('sentinel', b'unrelated fixture bytes\n')]:
                        target = folder/(name+'-'+role+'.dump')
                        source = '/sentinel' if role == 'sentinel' else '/etc/'+role
                        run(['debugfs', '-R', f'dump {source} {target}', str(image)], out/(name+'-linux.log'))
                        expected = data if kind == 'small' or role == 'sentinel' else bytes([65+('passwd','group','shadow').index(role)])*8192
                        assert target.read_bytes() == expected, (name, role)
                        if role != 'sentinel':
                            metadata = run(['debugfs', '-R', f'stat {source}', str(image)], out/(name+'-linux.log'))
                            mode = re.search(r'Mode:\s+([0-7]+)', metadata)
                            assert mode and int(mode[1], 8) == (0o600 if role == 'shadow' else 0o644), metadata
                            assert re.search(r'User:\s+0\s+Group:\s+0', metadata), metadata
                    manifest['cases'].append({'name': name, 'result': 'PASS', 'image_sha256': sha(image)})
                    assert sha(base) == original
                    print('PASS', name, flush=True)
        manifest['result'] = 'PASS'
    finally:
        manifest['changed_sources'] = [p for p, h in before.items() if sha(ROOT/p) != h]
        manifest['source_preservation'] = 'FAIL' if manifest['changed_sources'] else 'PASS'
        (out/'manifest.json').write_text(json.dumps(manifest, indent=2))
    assert manifest['source_preservation'] == 'PASS'
    print(f'PASS {len(manifest["cases"])}/20 actual EXT4 profiles; evidence:', out)


if __name__ == '__main__':
    main()
