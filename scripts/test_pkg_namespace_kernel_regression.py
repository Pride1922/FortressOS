"""Supplement namespace admission with actual legacy account-service regression.
Private source snapshot, host-only images; no kernel/media linking or boot.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import shutil
import subprocess
import time

from test_ext4_time_host import fixture

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('campaign', type=Path)
    previous = parser.parse_args().campaign.resolve()
    assert previous.is_relative_to((ROOT/'.codex-remote-attachments').resolve())
    parent = json.loads((previous/'manifest.json').read_text())
    assert parent['result'] == 'PASS'
    out = ROOT/'.codex-remote-attachments'/('pkg-namespace-regression-'+time.strftime('%Y%m%d-%H%M%S'))
    work = out/'workspace'
    shutil.copytree(previous/'workspace', work)
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    for name, digest in parent['sources'].items():
        assert sha(work/name) == digest
    shutil.copyfile(Path(__file__), out/'runner.py')
    result = {'result': 'INCOMPLETE', 'derived_from': str(previous), 'cases': [], 'runner_sha256': sha(Path(__file__))}

    def run(cmd, label):
        response = subprocess.run(cmd, cwd=work, capture_output=True, text=True, timeout=240)
        with (out/(label+'.log')).open('a') as stream:
            stream.write(response.stdout+response.stderr)
        response.check_returncode()
        return response.stdout

    includes = ['-Itests/ext4_host', '-Itests/host', '-Isrc/include', '-Isrc/fs', '-Isrc/drivers',
                '-Isrc/mm', '-Isrc/kernel', '-Isrc/arch/x86_64']
    flags = ['gcc', '-std=c11', '-O1', '-g', '-fsanitize=address,undefined', '-Wall', '-Wextra',
             '-Werror', '-no-pie', '-pthread']
    try:
        header = work/'tests/pkg_namespace_kernel_host.inc'
        old_header = header.read_text()
        # Derived host adapter strengthens retained-reference coverage to a
        # foreign inode, and never publishes the deliberately corrupt intent.
        derived = old_header.replace('ns_lookup("/etc/passwd")', 'ns_lookup("/etc/f0000000")')
        needle = '    assert(!ext4_publish_root(mounted));before=writes_seen;'
        assert derived.count(needle) == 1
        derived = derived.replace(needle, '    if (!strcmp(mode,"bad-intent")) return false;\n'+needle)
        race = '''
static _Thread_local bool ns_admitting;
static _Atomic unsigned ns_scan_entered,ns_writer_started;
static vfs_node_t *ns_race_parent;
static void ns_race_read_gate(void) {
    if (!ns_admitting || atomic_exchange(&ns_scan_entered,1)) return;
    unsigned spins=0;while(!atomic_load(&ns_writer_started) && spins++<100000000u) {}
    assert(atomic_load(&ns_writer_started));
}
static void *ns_race_writer(void *unused) {
    (void)unused;unsigned spins=0;
    while(!atomic_load(&ns_scan_entered) && spins++<100000000u) {}
    assert(atomic_load(&ns_scan_entered));
    assert(pthread_mutex_trylock(&e4_lock.mutex)!=0);atomic_store(&ns_writer_started,1);
    int error;creds_t actor;creds_init_root(&actor);
    vfs_node_t *node=e4_jcreate_common(ns_race_parent,"racingforeign",VFS_FILE,&error,true,NULL,&actor);
    assert(!node && error==-VFS_EROFS);return NULL;
}
static void ns_race_admit(void) {
    ns_race_parent=ns_lookup("/etc");unsigned before=writes_seen;pthread_t worker;
    assert(!pthread_create(&worker,NULL,ns_race_writer,NULL));
    ext4_account_namespace_report_t report;ns_admitting=true;
    assert(!ext4_root_account_namespace_admit(mounted,&report));ns_admitting=false;
    assert(!pthread_join(worker,NULL));assert(writes_seen==before);vfs_node_put(ns_race_parent);
    puts("RACE PASS queued root creator denied across atomic scan/fence; writes=0");
}
'''
        boundary = 'static bool ns_kernel_test(const char *mode) {'
        assert derived.count(boundary) == 1
        derived = derived.replace(boundary, race+'\n'+boundary)
        needle = '    assert(!ext4_root_account_namespace_admit(mounted,&report));'
        assert derived.count(needle) == 2  # one in the new race helper, one in test
        location = derived.index(needle, derived.index(boundary))
        derived = derived[:location]+'    ns_race_admit();\n'+derived[location:]
        header.write_text(derived)
        result['derived_test_header'] = {'parent_sha256': hashlib.sha256(old_header.encode()).hexdigest(),
                                         'derived_sha256': sha(header)}
        probe = work/'tests/pkg_account_namespace_host.c'
        body = probe.read_text()
        needle = 'static bool rd(block_dev_t *d,uint64_t l,void *p) {'
        assert body.count(needle) == 1
        probe.write_text('static void ns_race_read_gate(void);\n'+body.replace(needle, needle+'\n    ns_race_read_gate();'))
        result['derived_test_probe_sha256'] = sha(probe)
        run(flags+['-DTEST_PKG_NAMESPACE_ADMISSION', '-DTEST_EXT4_ATOMIC_COUNTERS']+includes+
            ['tests/pkg_account_namespace_host.c', 'tests/ext4_fault_disk.c', 'src/lib/account_redo.c',
             'user/tools/digest.c', '-o', str(work/'foreign-probe')], 'foreign-build')
        run(flags+includes+['tests/account_ext4_host.c', 'tests/ext4_fault_disk.c', 'src/lib/account_redo.c',
                           'user/tools/userdb.c', 'user/tools/digest.c', '-o', str(work/'account-service')], 'account-build')
        run(flags+['-Isrc/include', 'tests/account_redo_host.c', 'src/lib/account_redo.c',
                   'user/tools/digest.c', '-o', str(work/'redo-unit')], 'redo-build')
        run([str(work/'redo-unit')], 'redo-unit')
        base = fixture(out, 4096, inodes=2048, size_mib=256)
        commands = ['mkdir /etc', 'mkdir /var', 'mkdir /home', 'mkdir /root',
                    'set_inode_field /root mode 040700', 'mkdir /home/operator',
                    'set_inode_field /home/operator mode 040700',
                    'set_inode_field /home/operator uid 1000', 'set_inode_field /home/operator gid 1000']
        values = [('passwd', b'root:x:0:0:root:/root:/bin/shell\noperator:x:1000:1000::/home/operator:/bin/shell\n', '0100644'),
                  ('group', b'root:x:0:\noperator:x:1000:\n', '0100644'),
                  ('shadow', b'root:!:::::::\noperator:!:::::::\n', '0100600')]
        for name, data, mode in values:
            seed = out/name
            seed.write_bytes(data)
            commands += [f'write {seed} /etc/{name}', f'set_inode_field /etc/{name} mode {mode}']
        script = out/'initialize.commands'
        script.write_text('\n'.join(commands)+'\n')
        run(['debugfs', '-w', '-f', str(script), str(base)], 'initialize')
        run(['e2fsck', '-fn', str(base)], 'initialize')
        for sector in (512, 4096):
            image = out/f'legacy-{sector}.img'
            shutil.copyfile(base, image)
            run([str(work/'account-service'), str(image), str(sector)], f'legacy-{sector}')
            run(['e2fsck', '-fn', str(image)], f'legacy-{sector}-linux')
            result['cases'].append({'name': f'legacy-{sector}', 'result': 'PASS', 'image_sha256': sha(image)})
        # Corrupt data only, preserving the admitted physical namespace/metadata.
        for sector in (512, 4096):
            source = previous/f'1024-{sector}-shadow.img'
            before = sha(source)
            image = out/f'bad-intent-{sector}.img'
            shutil.copyfile(source, image)
            block = int(run(['debugfs', '-R', 'bmap /var/lib/fortress/accounts/pending 0', str(image)], 'bad-intent-fixture').strip())
            with image.open('r+b') as stream:
                stream.seek(block*1024+128)
                value = stream.read(1)
                stream.seek(block*1024+128)
                stream.write(bytes([value[0] ^ 1]))
            run([str(work/'foreign-probe'), str(image), str(sector), 'bad-intent'], f'bad-intent-{sector}')
            run(['e2fsck', '-fn', str(image)], f'bad-intent-{sector}-linux')
            assert sha(source) == before
            result['cases'].append({'name': f'bad-intent-{sector}', 'result': 'PASS', 'image_sha256': sha(image)})
        for path in ('/etc/passwd', '/var/lib/fortress/accounts'):
            image = out/('wrong-owner-'+path.replace('/', '_')+'.img')
            shutil.copyfile(previous/'admitted-4096.img', image)
            run(['debugfs', '-w', '-R', f'set_inode_field {path} uid 1000', str(image)], 'wrong-owner-fixture')
            run([str(work/'foreign-probe'), str(image), '512', 'reject'], 'wrong-owner')
            run(['e2fsck', '-fn', str(image)], 'wrong-owner-linux')
            result['cases'].append({'name': 'wrong-owner-'+path, 'result': 'PASS', 'image_sha256': sha(image)})
        run(['gcc', '-std=c11', '-DFORTRESS_DAC_ENFORCED', '-ffreestanding', '-fno-stack-protector',
             '-fno-stack-check', '-fno-lto', '-fPIE', '-m64', '-march=x86-64', '-mno-80387',
             '-mno-mmx', '-mno-sse', '-mno-sse2', '-mno-red-zone', '-Wall', '-Wextra', '-Werror',
             '-g', '-fstack-usage']+[p for p in includes if not p.startswith('-Itests')]+
            ['-c', 'src/fs/ext4.c', '-o', str(out/'ext4.o')], 'freestanding-object')
        frames = [line for line in (out/'ext4.su').read_text().splitlines()
                  if 'e4_ns_' in line or 'ext4_root_account_namespace_admit' in line]
        assert frames and all(int(line.split('\t')[1]) <= 1024 for line in frames)
        result.update(result='PASS', freestanding_frames=frames, redo_unit='PASS')
    except Exception as error:
        result.update(result='FAIL', error=repr(error))
        raise
    finally:
        (out/'manifest.json').write_text(json.dumps(result, indent=2)+'\n')
    print('PASS legacy service x2, corrupt intent/foreign handles x2, owner denials x2, redo unit and freestanding object; evidence:', out)


if __name__ == '__main__':
    main()
