"""Actual private five-record retirement JBD2 commits; no mount admission wiring."""
import argparse
import datetime
import hashlib
import json
import pathlib
import re
import shutil
import subprocess
from test_ext4_time_host import fixture

ROOT=pathlib.Path(__file__).resolve().parent.parent


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--block',type=int,choices=(1024,2048,4096));parser.add_argument('--sector',type=int,choices=(512,4096));parser.add_argument('--selected-cuts',action='store_true');args=parser.parse_args()
    out=ROOT/'.codex-remote-attachments'/('pkg-retirement-journal-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'))
    work=out/'workspace';work.mkdir(parents=True,exist_ok=False)
    sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
    sources={}
    for directory in ('src','tests','user'):
        for p in (ROOT/directory).rglob('*'):
            if p.is_file() and p.suffix in ('.c','.h','.inc'):
                name=str(p.relative_to(ROOT));sources[name]=sha(p)
                target=work/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(p,target)
                assert sha(target)==sources[name]
    overlay={}
    for name,before,after in (
        ('src/fs/ext4_transaction.inc','static int e4_semantic_commit(ext4_engine_t *e) {',
         'static int rt_core_commit(ext4_engine_t *e) {'),
        ('src/fs/ext4_mount_journal.inc','    r=ext4_engine_orphan_recover(engine);if (r) goto fail;',
         '    r=pg_life_reconstruct(engine);if(r)goto fail;\n    r=ext4_engine_orphan_recover(engine);if (r) goto fail;')):
        p=work/name;source=p.read_text();assert source.count(before)==1
        p.write_text(source.replace(before,after));overlay[name]={'original_sha256':sources[name],'overlay_sha256':sha(p),'before':before,'after':after}
    guards={str(p.relative_to(ROOT)):sha(p) for p in [ROOT/'AGENTS.md',ROOT/'README.md',ROOT/'scripts/test_pkg_retirement_journal.py',*(p for p in (ROOT/'bin').iterdir() if p.name in ('fortress.elf','initramfs.tar','fortress.iso','fortress.img'))]}
    source=(work/'tests/pkg_grant_fs_host.c').read_text()
    assert source.count('int main(int argc,char **argv){')==1
    generated=work/'tests/lifecycle.c'
    generated.write_text('struct ext4_engine;\nstatic int pg_life_reconstruct(struct ext4_engine *);\nstatic int e4_semantic_commit(struct ext4_engine *);\n'+
        source.replace('int main(int argc,char **argv){','int pg_previous_main(int argc,char **argv){')+
        '\n#include "pkg_grant_lifecycle.inc"\n')
    p=work/'tests/pkg_grant_lifecycle.inc';life=p.read_text()
    life=life.replace('pg_view v;return life_validate(e,&v);','pg_view v;int r=life_validate(e,&v);return r?r:rt_check(e);')
    life=life.replace('assert(!life_validate(fs->engine,&v)&&events==d->events);','assert(!life_validate(fs->engine,&v)&&!rt_check(fs->engine)&&events==d->events);')
    life=life.replace('memcpy(linked,d.stable,size);assert(vfs_file_get(held));','rt_setup(fs);life_check(fs);memcpy(linked,d.stable,size);assert(vfs_file_get(held));')
    life=life.replace('for(unsigned op=0;op<4;op++){','for(unsigned op=1;op<4;op++){')

    assert life.count('rt_setup(fs);life_check(fs);')==1 and life.count('!rt_check(fs->engine)')==1
    life=life.replace('for(unsigned pass=0;pass<2;pass++){', 'uint64_t first_account=0;for(unsigned pass=0;pass<2;pass++){')
    life=life.replace('assert(!ext4_freeze_and_sync(fs));\n            }cuts++;','assert(!ext4_freeze_and_sync(fs));if(!pass)first_account=rt_account_sequence();else assert(first_account==rt_account_sequence());\n            }cuts++;')
    life=life.replace('printf("PASS lifecycle cuts=', 'rt_report();printf("PASS lifecycle cuts=')
    assert life.count('first_account=rt_account_sequence();')==1
    p.write_text(life)
    text=generated.read_text()
    text=text.replace('struct ext4_engine;', 'struct ext4_engine;\nstruct ext4_mount;\nstatic int pg_life_core_commit(struct ext4_engine *);\nstatic int rt_check(struct ext4_engine *);\nstatic void rt_setup(struct ext4_mount *);\nstatic unsigned long rt_account_sequence(void);\nstatic void rt_report(void);')
    generated.write_text(text+'\n#include "pkg_retirement_journal.inc"\n')
    if args.selected_cuts:
        p=work/'tests/pkg_grant_lifecycle.inc';text=p.read_text()
        needle='for(size_t cut=0;cut<total;cut++){'
        assert text.count(needle)==1
        text=text.replace(needle,needle+'\n            if(cut!=0&&cut!=total/2&&cut!=total-1)continue;')
        assert text.count('for(unsigned op=1;op<4;op++){')==1 and text.count('op,total,total*8);')==1
        text=text.replace('for(unsigned op=1;op<4;op++){','for(unsigned op=1;op<4;op++){unsigned start_cuts=cuts;')
        p.write_text(text.replace('op,total,total*8);','op,total,(size_t)(cuts-start_cuts));'))
    manifest={'result':'INCOMPLETE','sources':sources,'private_overlay':overlay,'cases':[],'commands':[], 'cut_selection':'first/middle/last' if args.selected_cuts else 'all',
        'guards':guards,'scope':'private five-record commit and post-replay/pre-cleanup validation; actual EXT4/VFS/JBD2; trusted fixture pins retained in host process; no production hooks, mount admission, authenticated accounts, boot, Ring3, SMP or physical acceptance'}
    def run(cmd,label,timeout=180):
        manifest['commands'].append(cmd)
        r=subprocess.run(cmd,cwd=work,text=True,capture_output=True,timeout=timeout)
        (out/(label+'.log')).write_text(r.stdout+r.stderr)
        assert r.returncode==0,(label,r.returncode,r.stdout[-2000:],r.stderr[-2000:])
        return r.stdout+r.stderr
    try:
        flags=['gcc','-std=c11','-O1','-g','-fsanitize=address,undefined','-Wall','-Wextra','-Werror',
               '-no-pie','-pthread','-ffunction-sections','-fdata-sections','-Wl,--gc-sections']
        flags+=['-I'+p for p in ('tests','tests/ext4_host','tests/host','src/include','src/fs','src/drivers','src/mm','user/tools','.')]
        units=['tests/lifecycle.c','tests/pkg_grant_candidate.c','tests/pkg_retirement_candidate.c','tests/ext4_fault_disk.c','src/fs/permission_values.c','src/kernel/creds.c','user/tools/digest.c']
        dep=run(flags+['-MM','-MT','probe',*units],'dependencies')
        dependencies={str((work/p).resolve().relative_to(work)) for p in dep.replace('\\\n',' ').split() if not p.endswith(':')}
        manifest['compiled_inputs']={p:sha(work/p) for p in sorted(dependencies)}
        run(flags+units+['-o',str(out/'probe')],'compile')
        for bs in ((args.block,) if args.block else (1024,2048,4096)):
            image=fixture(out,bs,inodes=128,size_mib=32 if bs==4096 else 16);original=sha(image)
            for ss in ((args.sector,) if args.sector else (512,4096)):
                prefix=out/f'{bs}-{ss}'
                text=run([str(out/'probe'),str(image),str(ss),str(prefix)],f'{bs}-{ss}',timeout=1200)
                cuts=int(re.search(r'PASS lifecycle cuts=(\d+)',text)[1])
                for suffix in ('refunded','recovered'):
                    image_path=pathlib.Path(str(prefix)+'-'+suffix+'.img')
                    run(['e2fsck','-fn',str(image_path)],f'{bs}-{ss}-{suffix}-linux')
                    dumped={}
                    for name in ('grant.v1','ownership.v1','retirement.v1','account.v4','origin.v1'):
                        dst=out/f'{bs}-{ss}-{suffix}-{name}.dump'
                        run(['debugfs','-R',f'dump /pkg/{name} {dst}',str(image_path)],f'{bs}-{ss}-{suffix}-{name}')
                        dumped[name]=dst.read_bytes()
                    import struct
                    ldg,account,origin=(dumped[n] for n in ('retirement.v1','account.v4','origin.v1'))
                    grant,owners=dumped['grant.v1'],dumped['ownership.v1']
                    for record in (grant,owners,ldg,account,origin):
                        assert hashlib.sha256(record[:-32]).digest()==record[-32:]
                    assert ldg[136:168]==hashlib.sha256(grant).digest()
                    assert ldg[168:200]==hashlib.sha256(owners).digest()
                    assert account[112:144]==hashlib.sha256(origin).digest()
                    assert account[448:480]==hashlib.sha256(grant).digest()
                    assert account[592:624]==hashlib.sha256(ldg).digest()
                    assert ldg[24:32]==grant[24:32]==owners[48:56]==account[440:448]
                    assert ldg[200:208]==account[24:32]==origin[24:32]
                    assert struct.unpack_from('<QQ',ldg,288)==(0,0)
                    l,t=dumped['grant.v1'],dumped['ownership.v1']
                    assert hashlib.sha256(t).digest()==l[264:296]
                    assert struct.unpack_from('<QQ',l,312)==(8,1)
                    assert struct.unpack_from('<QQ',l,328)==(0,0)
                    assert struct.unpack_from('<H',t,258)[0]==3
                    assert 'File not found' in run(['debugfs','-R','stat /pkg/transaction.v1/unit',str(image_path)],f'{bs}-{ss}-{suffix}-absent')
                assert sha(image)==original
                case={'block':bs,'sector':ss,'cuts':cuts,'restarts_per_cut':2,'linux_audits':2,'result':'PASS'}
                manifest['cases'].append(case);print(case,flush=True)
        manifest['result']='PASS'
    except Exception as exc:
        manifest['result']='FAIL';manifest['error']=repr(exc);raise
    finally:
        manifest['changed_compiled_copies']=[p for p,h in manifest.get('compiled_inputs',{}).items() if sha(work/p)!=h]
        manifest['changed_repository_sources']=[p for p,h in sources.items() if sha(ROOT/p)!=h]
        compiled_repo={p for p in manifest.get('compiled_inputs',{}) if p in sources}
        manifest['changed_compiled_repository_sources']=[p for p in compiled_repo if sha(ROOT/p)!=sources[p]]
        manifest['changed_guards']=[p for p,h in guards.items() if sha(ROOT/p)!=h]
        if manifest['changed_guards'] or manifest['changed_compiled_copies'] or manifest['changed_compiled_repository_sources']:manifest['result']='FAIL'
        (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    assert manifest['result']=='PASS'
    print('Evidence:',out)


if __name__=='__main__':main()
