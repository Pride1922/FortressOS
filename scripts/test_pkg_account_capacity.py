"""Actual account role-capacity bound in a private fenced EXT4 profile."""
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
    out=ROOT/'.codex-remote-attachments'/('pkg-account-capacity-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'))
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
         'static int pg_capacity_core_commit(ext4_engine_t *e) {'),
):
        p=work/name;source=p.read_text();assert source.count(before)==1
        p.write_text(source.replace(before,after));overlay[name]={'original_sha256':sources[name],'overlay_sha256':sha(p),'before':before,'after':after}
    source=(work/'tests/pkg_account_namespace_host.c').read_text()
    assert source.count('int main(int argc,char **argv) {')==1
    generated=work/'tests/capacity.c'
    generated.write_text('struct ext4_engine;\nstatic int e4_semantic_commit(struct ext4_engine *);\n'+
        source.replace('int main(int argc,char **argv) {','int pg_previous_main(int argc,char **argv) {')+
        '\n#include "pkg_account_capacity.inc"\n')
    manifest={'result':'INCOMPLETE','sources':sources,'private_overlay':overlay,'cases':[],'commands':[],
        'scope':'actual EXT4/redo; fixed role capacity, fenced unpinned namespace, synthetic account validation and volatile test escrow; no production admission or persistent grant integration'}
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
        units=['tests/capacity.c','tests/ext4_fault_disk.c','src/lib/account_redo.c','user/tools/digest.c']
        dep=run(flags+['-MM','-MT','probe',*units],'dependencies')
        dependencies={str((work/p).resolve().relative_to(work)) for p in dep.replace('\\\n',' ').split() if not p.endswith(':')}
        manifest['compiled_inputs']={p:sha(work/p) for p in sorted(dependencies)}
        run(flags+units+['-o',str(out/'probe')],'compile')
        previous=ROOT/'.codex-remote-attachments/pkg-namespace-kernel-20261021-140820'
        old=json.loads((previous/'manifest.json').read_text());assert old['result']=='PASS'
        for bs in ((args.block,) if args.block else (1024,2048,4096)):
            for ss in (512,4096):
                for mode in ('recover','update','interrupt'):
                    src=previous/f'{bs}-{ss}-{"shadow" if mode=="recover" else "recover"}.img'
                    expected=next(c['image_sha256'] for c in old['cases'] if c['name']==src.stem)
                    assert sha(src)==expected
                    dst=out/f'{bs}-{ss}-{mode}.img';shutil.copyfile(src,dst)
                    run([str(out/'probe'),str(dst),str(ss),mode],f'{bs}-{ss}-{mode}',timeout=1200)
                    if mode=='interrupt':
                        for passno in range(2):
                            run([str(out/'probe'),str(dst),str(ss),'recover'],f'{bs}-{ss}-{mode}-restart-{passno}',timeout=1200)
                    run(['e2fsck','-fn',str(dst)],f'{bs}-{ss}-{mode}-linux')
                    for i,name in enumerate(('passwd','group','shadow')):
                        dump=out/f'{bs}-{ss}-{mode}-{name}.dump'
                        run(['debugfs','-R',f'dump /etc/{name} {dump}',str(dst)],f'{bs}-{ss}-{mode}-{name}')
                        assert dump.read_bytes()==bytes([65+i])*8192
                    assert sha(src)==expected
                    case={'block':bs,'sector':ss,'mode':mode,'result':'PASS'}
                    manifest['cases'].append(case);print(case,flush=True)
        manifest['result']='PASS'
    finally:
        manifest['changed_compiled_copies']=[p for p,h in manifest.get('compiled_inputs',{}).items() if sha(work/p)!=h]
        manifest['changed_repository_sources']=[p for p,h in sources.items() if sha(ROOT/p)!=h]
        compiled_repo={p for p in manifest.get('compiled_inputs',{}) if p in sources}
        manifest['changed_compiled_repository_sources']=[p for p in compiled_repo if sha(ROOT/p)!=sources[p]]
        (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    assert not manifest['changed_compiled_copies'] and not manifest['changed_compiled_repository_sources']
    from pkg_account_capacity_oracle import audit
    audit(out)
    print('Evidence:',out)


if __name__=='__main__':main()
