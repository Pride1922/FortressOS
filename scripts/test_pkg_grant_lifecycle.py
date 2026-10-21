"""Copied-workspace inode/orphan accounting overlay; no production edits."""
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
    parser=argparse.ArgumentParser();parser.add_argument('--block',type=int,choices=(1024,2048,4096));args=parser.parse_args()
    out=ROOT/'.codex-remote-attachments'/('pkg-grant-lifecycle-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'))
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
         'static int pg_life_core_commit(ext4_engine_t *e) {'),
        ('src/fs/ext4_mount_journal.inc','    r=ext4_engine_orphan_recover(engine);if (r) goto fail;',
         '    r=pg_life_reconstruct(engine);if(r)goto fail;\n    r=ext4_engine_orphan_recover(engine);if (r) goto fail;')):
        p=work/name;source=p.read_text();assert source.count(before)==1
        p.write_text(source.replace(before,after));overlay[name]={'original_sha256':sources[name],'overlay_sha256':sha(p),'before':before,'after':after}
    source=(work/'tests/pkg_grant_fs_host.c').read_text()
    assert source.count('int main(int argc,char **argv){')==1
    generated=work/'tests/lifecycle.c'
    generated.write_text('struct ext4_engine;\nstatic int pg_life_reconstruct(struct ext4_engine *);\nstatic int e4_semantic_commit(struct ext4_engine *);\n'+
        source.replace('int main(int argc,char **argv){','int pg_previous_main(int argc,char **argv){')+
        '\n#include "pkg_grant_lifecycle.inc"\n')
    manifest={'result':'INCOMPLETE','sources':sources,'private_overlay':overlay,'cases':[],'commands':[],
        'scope':'private copied-source semantic-commit and pre-cleanup reconstruction hooks; actual EXT4/VFS/JBD2; no production hooks, trusted boot, Ring3, SMP or physical acceptance'}
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
        units=['tests/lifecycle.c','tests/pkg_grant_candidate.c','tests/ext4_fault_disk.c','src/fs/permission_values.c','src/kernel/creds.c','user/tools/digest.c']
        dep=run(flags+['-MM','-MT','probe',*units],'dependencies')
        dependencies={str((work/p).resolve().relative_to(work)) for p in dep.replace('\\\n',' ').split() if not p.endswith(':')}
        manifest['compiled_inputs']={p:sha(work/p) for p in sorted(dependencies)}
        run(flags+units+['-o',str(out/'probe')],'compile')
        for bs in ((args.block,) if args.block else (1024,2048,4096)):
            image=fixture(out,bs,inodes=128,size_mib=32 if bs==4096 else 16);original=sha(image)
            for ss in (512,4096):
                prefix=out/f'{bs}-{ss}'
                text=run([str(out/'probe'),str(image),str(ss),str(prefix)],f'{bs}-{ss}',timeout=1200)
                cuts=int(re.search(r'PASS lifecycle cuts=(\d+)',text)[1])
                for suffix in ('refunded','recovered'):
                    image_path=pathlib.Path(str(prefix)+'-'+suffix+'.img')
                    run(['e2fsck','-fn',str(image_path)],f'{bs}-{ss}-{suffix}-linux')
                    dumped={}
                    for name in ('grant.v1','ownership.v1'):
                        dst=out/f'{bs}-{ss}-{suffix}-{name}.dump'
                        run(['debugfs','-R',f'dump /pkg/{name} {dst}',str(image_path)],f'{bs}-{ss}-{suffix}-{name}')
                        dumped[name]=dst.read_bytes()
                    import struct
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
    finally:
        manifest['changed_compiled_copies']=[p for p,h in manifest.get('compiled_inputs',{}).items() if sha(work/p)!=h]
        manifest['changed_repository_sources']=[p for p,h in sources.items() if sha(ROOT/p)!=h]
        compiled_repo={p for p in manifest.get('compiled_inputs',{}) if p in sources}
        manifest['changed_compiled_repository_sources']=[p for p in compiled_repo if sha(ROOT/p)!=sources[p]]
        (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    assert not manifest['changed_compiled_copies'] and not manifest['changed_compiled_repository_sources']
    print('Evidence:',out)


if __name__=='__main__':main()
