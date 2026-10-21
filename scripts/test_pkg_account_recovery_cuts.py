"""Interrupt actual account recovery in a private source-derived host fixture.
Requires a workspace/base image made by test_pkg_account_ext4_host.py.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('campaign', type=Path)
    args = parser.parse_args()
    previous = args.campaign.resolve()
    assert previous.is_relative_to((ROOT/'.codex-remote-attachments').resolve())
    base = previous/'4096/base-4096.img'
    assert base.is_file()
    out = ROOT/'.codex-remote-attachments'/('pkg-account-recovery-'+time.strftime('%Y%m%d-%H%M%S'))
    work = out/'workspace'
    shutil.copytree(previous/'workspace', work)
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    source = work/'tests/pkg_account_ext4_host.c'
    original = source.read_text()
    needle = '    assert(!account_redo_recover(&io,&workspace));'
    assert original.count(needle) == 2
    injected = '''    flushes_seen=0;
    if(!strncmp(argv[3],"recover-cut=",12)) {
        cut_flush=(unsigned)strtoul(argv[3]+12,NULL,10);
        int r=account_redo_recover(&io,&workspace);
        assert(r<0 && offline);assert(!fclose(image));puts("RECOVERY CUT PASS");return 0;
    }
    assert(!account_redo_recover(&io,&workspace));
    printf("RECOVERY_FLUSHES=%u\\n",flushes_seen);'''
    source.write_text(original.replace(needle, injected, 1))
    manifest = {'derived_from': str(previous), 'input_base_sha256': sha(base),
                'derived_test_sha256': sha(source), 'result': 'INCOMPLETE', 'cases': [],
                'scope': 'actual recovery, private host flush-prefix failures; no physical/boot/grant acceptance'}
    def run(command, log):
        result = subprocess.run(command, cwd=work, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, timeout=180)
        with log.open('a') as stream: stream.write(result.stdout)
        result.check_returncode()
        return result.stdout
    try:
        run(['gcc', '-std=c11', '-O1', '-g', '-fsanitize=address,undefined', '-Wall', '-Wextra',
             '-Werror', '-no-pie', '-pthread', '-Itests/ext4_host', '-Itests/host', '-Isrc/include',
             '-Isrc/fs', '-Isrc/drivers', '-Isrc/mm', '-Isrc/kernel', '-Isrc/arch/x86_64',
             'tests/pkg_account_ext4_host.c', 'tests/ext4_fault_disk.c', 'src/lib/account_redo.c',
             'user/tools/userdb.c', 'user/tools/digest.c', '-o', str(work/'recovery-probe')], out/'build.log')
        for sector in (512, 4096):
            seed = out/f'pending-{sector}.img'
            shutil.copyfile(base, seed)
            run([str(work/'recovery-probe'), str(seed), str(sector), 'cut=60'], out/f'seed-{sector}.log')
            pending = run(['debugfs', '-R', 'stat /var/lib/fortress/accounts/pending', str(seed)], out/f'seed-{sector}.log')
            assert 'Inode:' in pending and 'File not found' not in pending
            measured = out/f'measured-{sector}.img'
            shutil.copyfile(seed, measured)
            text = run([str(work/'recovery-probe'), str(measured), str(sector), 'recover'], out/f'measured-{sector}.log')
            count = int(re.search(r'RECOVERY_FLUSHES=(\d+)', text)[1])
            assert count > 1
            for boundary in range(1, count+1):
                name = f'{sector}-recovery-cut-{boundary}'
                image = out/(name+'.img')
                shutil.copyfile(seed, image)
                run([str(work/'recovery-probe'), str(image), str(sector), f'recover-cut={boundary}'], out/(name+'.log'))
                for attempt in (1, 2):
                    run([str(work/'recovery-probe'), str(image), str(sector), 'recover'], out/(name+'.log'))
                    run(['e2fsck', '-fn', str(image)], out/(name+'-linux.log'))
                    for index, role in enumerate(('passwd', 'group', 'shadow')):
                        target = out/(name+f'-{attempt}-{role}.dump')
                        run(['debugfs', '-R', f'dump /etc/{role} {target}', str(image)], out/(name+'-linux.log'))
                        assert target.read_bytes() == bytes([65+index])*8192
                manifest['cases'].append({'sector': sector, 'recovery_flush': boundary, 'result': 'PASS',
                                           'image_sha256': sha(image), 'clean_recoveries': 2})
                assert image.resolve().is_relative_to(out.resolve()) and image.is_file()
                image.unlink()
            print('PASS all recovery flush cuts:', sector, count, flush=True)
        assert sha(base) == manifest['input_base_sha256']
        manifest['result'] = 'PASS'
    finally:
        (out/'manifest.json').write_text(json.dumps(manifest, indent=2))
    print('PASS recovery cuts:', len(manifest['cases']), 'evidence:', out)


if __name__ == '__main__':
    main()
