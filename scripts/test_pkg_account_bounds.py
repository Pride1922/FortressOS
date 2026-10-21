"""Check source-derived one-pass bounds against privately instrumented EXT4.
Not production admission or a restart-invariant reservation proof.
"""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parent.parent


def tree_blocks(count, bs):
    if count <= 4:
        return 0
    capacity = (bs-12)//12
    leaves = (count+capacity-1)//capacity
    indexes = (leaves+capacity-1)//capacity if leaves > 4 else 0
    assert indexes <= 4 and leaves+indexes <= 64
    return leaves+indexes


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('campaign', type=Path)
    args = parser.parse_args()
    previous = args.campaign.resolve()
    assert previous.is_relative_to((ROOT/'.codex-remote-attachments').resolve())
    out = ROOT/'.codex-remote-attachments'/('pkg-account-bounds-'+time.strftime('%Y%m%d-%H%M%S'))
    work = out/'workspace'
    shutil.copytree(previous/'workspace', work)
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    manifest = {'result': 'INCOMPLETE', 'derived_from': str(previous), 'cases': [],
                'scope': 'one-pass arithmetic + actual-code private gross-allocation hook',
                'restart_invariant': 'NOT PROVED', 'production_admission': 'NOT IMPLEMENTED'}
    mutate = work/'src/fs/ext4_mutate.inc'
    text = mutate.read_text()
    needle = '    e4_p16(d+offset,(uint16_t)n); e4_p32(sb+total,(uint32_t)sum); return true;'
    assert text.count(needle) == 1
    mutate.write_text('extern void pkg_bound_alloc(unsigned,unsigned);\n'+text.replace(
        needle, '    if(delta<0)pkg_bound_alloc(inode,(unsigned)-delta);\n'+needle))
    probe = work/'tests/pkg_account_ext4_host.c'
    text = probe.read_text()
    text = '''static unsigned bound_active,bound_blocks,bound_inodes;
void pkg_bound_alloc(unsigned inode,unsigned n) {
    if(bound_active){if(inode)bound_inodes+=n;else bound_blocks+=n;}
}
'''+text
    needle = '        int updated=account_redo_update(&io,&workspace,generation);'
    assert text.count(needle) == 1
    text = text.replace(needle, '''        bound_blocks=bound_inodes=0;bound_active=1;
'''+needle+'''
        bound_active=0;
        unsigned bs=mounted->bs,d=(8192+bs-1)/bs,cap=(bs-12)/12;
        unsigned count=1048576/bs,leaves=(count+cap-1)/cap;
        unsigned tdir=leaves+(leaves>4?(leaves+cap-1)/cap:0);
        unsigned bound=6*d+1+6*(d>4?1:0)+11*(1+tdir);
        assert(bound_blocks<=bound && bound_inodes<=7);
        printf("GROSS generation=%u blocks=%u inodes=%u bound_blocks=%u\\n",
               generation,bound_blocks,bound_inodes,bound);''')
    probe.write_text(text)
    manifest['derived_sources'] = {str(p.relative_to(work)): sha(p) for p in (mutate, probe)}
    def run(cmd, log):
        result = subprocess.run(cmd, cwd=work, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, timeout=180)
        with log.open('a') as stream: stream.write(result.stdout)
        result.check_returncode()
        return result.stdout
    try:
        formulas = []
        for bs in (1024, 2048, 4096):
            # Exhaust every dense extent-count possibility, not just endpoints.
            maximum = max(tree_blocks(n, bs) for n in range(1, 1048576//bs+1))
            d = (8192+bs-1)//bs
            bound = 6*d+1+6*tree_blocks(d, bs)+11*(1+maximum)
            formulas.append({'block_size': bs, 'max_directory_tree_blocks': maximum,
                             'update_gross_blocks': bound, 'update_bytes': bound*bs,
                             'fresh_inodes': 7, 'within_existing_floor': bound*bs <= 262144})
            assert bound*bs <= 262144
        manifest['formulas'] = formulas
        run(['gcc', '-std=c11', '-O1', '-g', '-fsanitize=address,undefined', '-Wall', '-Wextra',
             '-Werror', '-no-pie', '-pthread', '-Itests/ext4_host', '-Itests/host', '-Isrc/include',
             '-Isrc/fs', '-Isrc/drivers', '-Isrc/mm', '-Isrc/kernel', '-Isrc/arch/x86_64',
             'tests/pkg_account_ext4_host.c', 'tests/ext4_fault_disk.c', 'src/lib/account_redo.c',
             'user/tools/userdb.c', 'user/tools/digest.c', '-o', str(work/'bound-probe')], out/'build.log')
        for bs in (1024, 2048, 4096):
            base = previous/f'{bs}/base-{bs}.img'
            digest = sha(base)
            for sector in (512, 4096):
                for kind in ('small', 'bytes', 'pins', 'directory') + (('fragment',) if bs == 1024 else ()):
                    name = f'{bs}-{sector}-{kind}'
                    image = out/(name+'.img')
                    shutil.copyfile(base, image)
                    output = run([str(work/'bound-probe'), str(image), str(sector), kind], out/(name+'.log'))
                    assert 'GROSS ' in output
                    run(['e2fsck', '-fn', str(image)], out/(name+'-linux.log'))
                    manifest['cases'].append({'name': name, 'result': 'PASS', 'image_sha256': sha(image)})
            assert sha(base) == digest
        manifest['result'] = 'PASS'
    finally:
        (out/'manifest.json').write_text(json.dumps(manifest, indent=2))
    print(json.dumps(manifest['formulas'], indent=2))
    print('PASS', len(manifest['cases']), 'gross-allocation profiles; evidence:', out)


if __name__ == '__main__':
    main()
