"""Actual copied-source account service/redo and JBD2 paired-state gate.

Public test enrollment keys, disposable regular-file fixtures and RAM sector
fault adapter only. Never uses a native .origin witness or release build target.
"""
import argparse
import datetime
import hashlib
import json
import pathlib
import re
import shutil
import struct
import subprocess
from concurrent.futures import ThreadPoolExecutor
from test_ext4_time_host import fixture
from test_pkg_enrollment import signed, policy
from test_pkg_service_integration import function
from pkg_account_service_overlay import apply

ROOT = pathlib.Path(__file__).resolve().parents[1]


def digest(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--block', type=int, choices=(1024, 2048, 4096))
    parser.add_argument('--smoke', action='store_true')
    parser.add_argument('--jobs', type=int, choices=range(1, 9), default=8)
    args = parser.parse_args()
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%d-%H%M%S-%f')
    out = ROOT/'.codex-remote-attachments'/('pkg-account-service-durable-'+stamp)
    work = out/'workspace'; work.mkdir(parents=True)
    sources = {}
    for folder in ('src', 'tests', 'user', 'scripts'):
        for p in (ROOT/folder).rglob('*'):
            if p.is_file() and p.suffix in ('.c', '.h', '.inc', '.py', '.mk') and '__pycache__' not in p.parts:
                name = str(p.relative_to(ROOT)); sources[name] = digest(p)
                dest = work/name; dest.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(p, dest)
    guards = {str(p.relative_to(ROOT)): digest(p) for p in
              [ROOT/'AGENTS.md', ROOT/'README.md', ROOT/'assets/ssl/ca-certificates.sha256']+
              [p for p in (ROOT/'bin').iterdir() if p.is_file() and p.name in
               ('fortress.elf', 'initramfs.tar', 'fortress.iso', 'fortress.img')]}
    manifest = {'status': 'INCOMPLETE', 'sources': sources, 'guards_before': guards,
                'commands': [], 'overlay': [], 'cases': [],
                'scope': 'Private copied-source actual account service/redo EXT4/JBD2 integration; semantic commit cancellation, no production or boot acceptance'}

    def replace(name, before, after):
        p = work/name; text = p.read_text()
        if text.count(before) != 1:
            manifest.update(status='FAIL', error=repr((name,before,text.count(before))))
            (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
            raise AssertionError((name,before))
        p.write_text(text.replace(before, after))
        manifest['overlay'].append({'path': name, 'before': before, 'after': after})

    replace('src/fs/ext4_transaction.inc', 'static int e4_semantic_commit(ext4_engine_t *e) {',
            'static int ad_core_commit(ext4_engine_t *e) {')
    p = 'src/fs/ext4_orphan.inc'
    text = (work/p).read_text()
    old = function(text, 'int ext4_engine_orphan_unlink(ext4_engine_t *e,uint32_t parent,const char *name,bool directory) {')
    before = '    r=e4_orphan_add(e,ino,raw);if (r) return e4_namespace_end(e,r);'
    assert old.count(before) == 1
    replace(p, old, old.replace(before, '    r=ad_activate(e,ino,parent);if(r)return e4_namespace_end(e,r);\n'+before))
    before = '        e4_inode_sum(e->fs,ino,raw);r=e4_namespace_end(e,0);if (r) return r;'
    replace(p, before, '        if(deleted){r=ad_retire(e,ino);if(r)return e4_namespace_end(e,r);}\n'+before)
    replace('src/fs/ext4_mount_journal.inc', '    e4_engine_dispose(engine);engine=NULL;',
            '    e4_mount_stage(stage,"account-provenance-preview");\n    r=ad_preview(engine);if(r)goto fail;\n    e4_engine_dispose(engine);engine=NULL;')
    apply(work, replace)
    generated = work/'tests/account-service-durable.c'
    generated.write_text('#define _POSIX_C_SOURCE 200809L\n#include "account_redo.h"\nstruct ext4_engine;struct ext4_mount;struct vfs_node;\nstatic struct ext4_mount *sv_fs;\nstatic unsigned sv_role=ACCOUNT_ROLE_COUNT;\nstatic void teardown(void);\nstatic int e4_semantic_commit(struct ext4_engine *);\nstatic int e4_namespace_end(struct ext4_engine *,int);\nstatic int ad_activate(struct ext4_engine *,unsigned,unsigned);\nstatic int ad_retire(struct ext4_engine *,unsigned);\nstatic int ad_preview(struct ext4_engine *);\nstatic int ad_validate(struct ext4_engine *);\nstatic int sv_state_extra(void);\nstatic const char *sv_path(unsigned);\nstatic void sv_seed(struct ext4_mount *);\nstatic void sv_seed_state(struct ext4_mount *);\nstatic bool sv_pin_deny(struct vfs_node *);\nstatic int sv_boot_bind(void *,account_io_t *);\nstatic int sv_boot_end(void *);\nstatic void sv_relax(void);\nstatic int sv_begin(void *);\nstatic int sv_end(void *);\nstatic int sv_issue_pre(void *,uint64_t);\nstatic int sv_issue(void *,account_workspace_t *,uint64_t);\nstatic int sv_complete(void *);\n#define TEST_PERMISSIONS_ENFORCEMENT\n#define main account_previous_main\n#include "ext4_mount_host.c"\n#undef main\n#include "pkg_account_durable.inc"\n#include "src/kernel/accounts.c"\nuint64_t *vmm_get_active_pml4_virt(void){return (void *)1;}\nbool vmm_validate_user_range(uint64_t *p,uintptr_t a,size_t n,bool output){\n (void)p;(void)output;return a>=4096&&a<0x800000000000ull&&n<=0x800000000000ull-a;\n}\n' + function((work/'src/kernel/syscall.c').read_text(),'int64_t syscall_from_vfs_error(') + '\n#include "pkg_account_service_durable.inc"\n')

    def run(command, label, timeout=180):
        manifest['commands'].append(command)
        log_path=out/(label+'.log')
        with log_path.open('w') as log:
            result = subprocess.run(command, cwd=work, stdout=log, stderr=subprocess.STDOUT, text=True, timeout=timeout)
        text=log_path.read_text()
        assert result.returncode == 0, (label, result.returncode, text[-2000:])
        return text

    manifest['copied_inputs'] = {p: digest(work/p) for p in sources}

    def audit(image, label, family):
        before=digest(image)
        run(['e2fsck','-fn',str(image)],label+'-fsck')
        old=[b'root:x:0:0:root:/root:/bin/shell\n',b'root:x:0:\n',b'root:!:::::::\n']
        new=[b'root:x:0:0:root2:/root:/bin/shell\n',b'root:x:0:root\n',b'root:*:::::::\n']
        for i in range(3):
            while len(new[i])<8192:
                n=min(513,8192-len(new[i]));new[i]+=(b'#'+b'p'*(n-2)+b'\n') if n>1 else b'\n'
        for i,name in enumerate(('passwd','group','shadow')):
            target=out/(label+'-'+name+'.dump')
            run(['debugfs','-R',f'dump /etc/{name} {target}',str(image)],label+'-'+name)
            assert target.read_bytes()==(new if family else old)[i]
        records={}
        for name in ('origin.v1','account-generation.v1'):
            target=out/(label+'-'+name+'.dump')
            run(['debugfs','-R',f'dump /pkg/{name} {target}',str(image)],label+'-'+name)
            b=target.read_bytes();records[name]=b;assert len(b)==1024 and b[992:]==hashlib.sha256(b[:992]).digest()
            assert struct.unpack_from('<H',b,22)[0]==0
            if name=='origin.v1': assert b[112:992]==bytes(880)
            else: assert b[152:328]==bytes(176)
        state,slot=records['account-generation.v1'],records['origin.v1']
        assert state[112:144]==hashlib.sha256(slot).digest() and state[24:32]==slot[24:32]
        assert struct.unpack_from('<H',state,20)[0]==2
        assert struct.unpack_from('<Q',state,32)[0]==struct.unpack_from('<Q',slot,40)[0]==2
        assert struct.unpack_from('<Q',state,144)[0]==2
        for i,b in enumerate(new if family else old):
            assert state[328+32*i:360+32*i]==hashlib.sha256(b).digest()
            assert struct.unpack_from('<I',state,424+4*i)[0]==len(b)
        assert digest(image)==before

    try:
        flags = ['gcc', '-std=c11', '-O1', '-g', '-fsanitize=address,undefined', '-fstack-usage',
                 '-Wall', '-Wextra', '-Werror', '-no-pie', '-pthread', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections']
        flags += ['-I'+p for p in ('tests', 'tests/ext4_host', 'tests/host', 'src/include', 'src/fs',
                                   'src/drivers', 'src/kernel', 'src/arch/x86_64', 'src/mm', 'user/tools', 'src/lib/crypto/upstream/src', '.')]
        objects = []
        for tag, path in [('mono', 'src/lib/crypto/upstream/src/monocypher.c'),
                          ('ed', 'src/lib/crypto/upstream/src/optional/monocypher-ed25519.c')]:
            target = out/(tag+'.o'); objects.append(str(target))
            run(flags+['-include', 'src/lib/crypto/namespace.h', '-c', path, '-o', str(target)], 'compile-'+tag)
        units = ['tests/account-service-durable.c', 'tests/pkg_enrollment_candidate.c', 'tests/ext4_fault_disk.c',
                 'src/fs/permission_values.c', 'src/kernel/creds.c', 'user/tools/digest.c',
                 'src/lib/crypto/hash.c', 'src/lib/crypto/curve.c', 'src/lib/account_redo.c', 'user/tools/userdb.c']
        dep = run(flags+['-MM', '-MT', 'probe', *units], 'dependencies')
        names = {p for p in dep.replace('\\\n', ' ').split() if not p.endswith(':')}
        names.update(('src/lib/crypto/upstream/src/monocypher.c','src/lib/crypto/upstream/src/optional/monocypher-ed25519.c'))
        manifest['compiled_inputs'] = {p: digest(work/p) for p in sorted(names)}
        run(flags+units+objects+['-o', str(out/'probe')], 'compile')
        for bs in ((args.block,) if args.block else (1024, 2048, 4096)):
            image = fixture(out, bs, inodes=128, size_mib=32 if bs == 4096 else 16); image_before = digest(image)
            for ss in (512, 4096):
                tag = f'{bs}-{ss}'; prefix = out/tag
                run([str(out/'probe'), 'setup', str(image), str(ss), str(prefix)], tag+'-setup')
                setup = pathlib.Path(str(prefix)+'-setup.img'); body = pathlib.Path(str(prefix)+'-body.img').read_bytes()
                assert len(body) == 2464
                descriptor = signed(body); ids = [body[40+16*i:56+16*i] for i in range(5)]
                enrollment = out/(tag+'-enrollment.bin'); enrollment.write_bytes(descriptor)
                selector = out/(tag+'-policy.bin'); selector.write_bytes(policy(descriptor, uuids=ids))
                setup_before = digest(setup)
                normal=out/(tag+'-normal')
                output=run([str(out/'probe'),'normal',str(setup),str(ss),str(enrollment),str(selector),str(normal)],tag+'-normal')
                commits=int(re.search(r'PASS update commits=(\d+)',output)[1])
                final=pathlib.Path(str(normal)+'-final.img')
                audit(final,tag+'-normal',1)
                fresh=out/(tag+'-fresh')
                text=run([str(out/'probe'),'recover',str(final),str(ss),str(enrollment),str(selector),str(fresh)],tag+'-fresh')
                assert 'PASS family=1 auth=2' in text and 'commits=0' in text
                case={'block':bs,'sector':ss,'update_commits':commits,'cuts':[]}
                if not args.smoke:
                    def cancel(cut):
                        pre=out/(tag+f'-cut-{cut}')
                        text=run([str(out/'probe'),f'update={cut}',str(setup),str(ss),str(enrollment),str(selector),str(pre)],pre.name)
                        interrupted=pathlib.Path(str(pre)+'-interrupted.img')
                        recovered=out/(pre.name+'-recovered')
                        recovery=run([str(out/'probe'),'recover',str(interrupted),str(ss),str(enrollment),str(selector),str(recovered)],recovered.name)
                        result=pathlib.Path(str(recovered)+'-final.img')
                        family=int(re.search(r'PASS family=(\d+)',recovery)[1])
                        audit(result,recovered.name,family)
                        again=out/(pre.name+'-again')
                        repeated=run([str(out/'probe'),'recover',str(result),str(ss),str(enrollment),str(selector),str(again)],again.name)
                        assert 'commits=0' in repeated
                        recovery_commits=int(re.search(r'PASS recovery commits=(\d+)',recovery)[1])
                        if recovery_commits:
                            nested=out/(pre.name+'-nested')
                            nested_text=run([str(out/'probe'),'recover=1',str(interrupted),str(ss),str(enrollment),str(selector),str(nested)],nested.name)
                            resumed=out/(pre.name+'-resumed')
                            resumed_text=run([str(out/'probe'),'recover',str(nested)+'-interrupted.img',str(ss),str(enrollment),str(selector),str(resumed)],resumed.name)
                            audit(pathlib.Path(str(resumed)+'-final.img'),resumed.name,int(re.search(r'PASS family=(\d+)',resumed_text)[1]))
                            pathlib.Path(str(nested)+'-interrupted.img').unlink();pathlib.Path(str(resumed)+'-final.img').unlink()
                        else: nested_text=''
                        record={'nested':nested_text,'cut':cut,'interrupted':text,'recovery':recovery,'repeat':repeated}
                        for im in (interrupted,result,pathlib.Path(str(again)+'-final.img')):
                            record[im.name]=digest(im);im.unlink()
                        return record
                    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
                        case['cuts']=list(pool.map(cancel,range(1,commits+1)))
                assert digest(setup)==setup_before and digest(image)==image_before
                case['status']='PASS';manifest['cases'].append(case);print(case['block'],case['sector'],commits,len(case['cuts']),flush=True)
        manifest['status'] = 'PASS'
    except Exception as exc:
        manifest['status'] = 'FAIL'; manifest['error'] = repr(exc); raise
    finally:
        manifest['changed_sources'] = [p for p, h in sources.items() if digest(ROOT/p) != h]
        manifest['changed_compiled_copies'] = [p for p, h in manifest.get('compiled_inputs', {}).items() if digest(work/p) != h]
        manifest['changed_compiled_repository_sources'] = [p for p in manifest.get('compiled_inputs',{}) if p in sources and digest(ROOT/p)!=sources[p]]
        manifest['changed_all_copies'] = [p for p, h in manifest['copied_inputs'].items() if digest(work/p) != h]
        manifest['guards_after'] = {p: digest(ROOT/p) for p in guards}
        manifest['preserved'] = not manifest['changed_compiled_repository_sources'] and not manifest['changed_compiled_copies'] and not manifest['changed_all_copies'] and guards == manifest['guards_after']
        if not manifest['preserved']: manifest['status'] = 'FAIL'
        manifest['artifacts'] = {p.name: digest(p) for p in out.iterdir() if p.is_file() and p.name != 'manifest.json'}
        (out/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
        print('Evidence:', out, manifest['status'], flush=True)
    assert manifest['status'] == 'PASS'


if __name__ == '__main__':
    main()
