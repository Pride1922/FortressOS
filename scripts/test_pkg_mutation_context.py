"""Private per-operation context with actual filesystem and service contenders."""
import argparse
import hashlib
import json
import pathlib
import re
import shutil
import subprocess
import sys
from test_pkg_service_integration import function

ROOT=pathlib.Path(__file__).resolve().parent.parent


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--block',type=int,choices=(1024,2048,4096))
    parser.add_argument('--tsan',action='store_true');parser.add_argument('--binding',action='store_true');args=parser.parse_args()
    build=subprocess.run([sys.executable,'scripts/test_pkg_service_integration.py','--build-only'],cwd=ROOT,text=True,capture_output=True,timeout=180)
    assert not build.returncode,(build.stdout,build.stderr)
    out=pathlib.Path(re.search(r'Evidence: (.+)',build.stdout)[1]).resolve()
    assert out.parent==(ROOT/'.codex-remote-attachments').resolve()
    work=out/'workspace';baseline=json.loads((out/'manifest.json').read_text())
    assert baseline['result']=='BUILD_ONLY' and not baseline['changed_compiled_copies'] and not baseline['changed_compiled_repository_sources']
    shutil.copyfile(out/'manifest.json',out/'build-only-manifest.json');sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
    overlays=[]
    def replace(name,before,after):
        p=work/name;s=p.read_text();assert s.count(before)==1,(name,before)
        p.write_text(s.replace(before,after));overlays.append(dict(file=name,before=before,after=after))
    name='tests/pkg_service_integration.inc';s=(work/name).read_text()
    old=function(s,'static int e4_semantic_commit(ext4_engine_t *e){')
    replace(name,old,'static int e4_semantic_commit(ext4_engine_t *e){return integrated?mutation_dispatch(e):pg_life_core_commit(e);}')
    old=function((work/name).read_text(),'static int pg_integration_headroom(void *context){')
    replace(name,old,'static int pg_integration_headroom(void *context){return mutation_headroom(context);}')
    old=function((work/name).read_text(),'static int pg_integration_storage(void *context,account_io_t *io){')
    replace(name,old,'static int pg_integration_storage(void *context,account_io_t *io){return mutation_storage(context,io);}')
    replace(name,'int main(int argc,char **argv){','int mutation_previous_main(int argc,char **argv){')
    replace('tests/integration.c','#include "pkg_service_integration.inc"',
        'static int mutation_dispatch(ext4_engine_t *);\nstatic int mutation_headroom(void *);\nstatic int mutation_storage(void *,account_io_t *);\n#include "pkg_service_integration.inc"\n#include "pkg_mutation_integration.inc"')
    if args.binding:
        from pkg_call_overlay import integration_overlay
        integration_overlay(work,replace)
    command=baseline['compile_command'];command.insert(1,'-DTEST_EXT4_ATOMIC_COUNTERS')
    if args.tsan:
        command=[c for c in command if c!='-fsanitize=address,undefined'];command.insert(1,'-fsanitize=thread')
    manifest={'result':'INCOMPLETE','kind':'private-call-binding' if args.binding else 'private-mutation-context','sources':baseline['sources'],'overlays':overlays,'cases':[],
        'sanitizer':'TSan' if args.tsan else 'ASan/UBSan','commands':[],
        'scope':'private copied dispatcher, pinned operation arena, pthread delivery adapter; one ordinary file and one tagged file, trusted quiescent enrollment/acquire; no production/TCB/boot/media acceptance'}
    if args.binding:manifest['scope']='private copied-TCB retained call and cancellation; actual VFS finite sandbox namespace; host current-task delivery, no scheduler/production/boot/media acceptance'
    def run(cmd,label,timeout=300):
        manifest['commands'].append(cmd)
        try:r=subprocess.run(cmd,cwd=work,text=True,capture_output=True,timeout=timeout)
        except subprocess.TimeoutExpired as e:
            (out/(label+'.log')).write_bytes((e.stdout or b'')+(e.stderr or b''));raise
        (out/(label+'.log')).write_text(r.stdout+r.stderr)
        assert not r.returncode,(label,r.returncode,r.stdout[-1800:],r.stderr[-2400:])
        return r.stdout+r.stderr
    try:
        dep=run(command[:command.index('-o')]+['-MM','-MT','probe'],'mutation-dependencies')
        names={str((work/p).resolve().relative_to(work)) for p in dep.replace('\\\n',' ').split() if not p.endswith(':')}
        names.update(('src/kernel/thread.c','src/kernel/syscall.c'))
        manifest['compiled_inputs']={p:sha(work/p) for p in sorted(names)}
        run(command,'mutation-compile')
        previous=ROOT/'.codex-remote-attachments/pkg-namespace-kernel-20261021-140820'
        prior=json.loads((previous/'manifest.json').read_text());assert prior['result']=='PASS'
        for bs in ((args.block,) if args.block else (1024,2048,4096)):
            for sector in (512,4096):
                source=previous/f'{bs}-{sector}-oom.img';original=sha(source)
                assert next(c['image_sha256'] for c in prior['cases'] if c['name']==source.stem)==original
                target=out/f'{bs}-{sector}-mutation.img';shutil.copyfile(source,target)
                invoke=[str(out/'probe'),str(target),str(sector)]
                if args.tsan:invoke=['setarch','x86_64','-R',*invoke]
                run(invoke,f'{bs}-{sector}-mutation',timeout=600)
                run(['e2fsck','-fn',str(target)],f'{bs}-{sector}-linux')
                for path,letters in (('/pkg/transaction.v1/unit',b'PQ'),('/ordinary',b'NO')):
                    dump=out/f'{bs}-{sector}-{path.rsplit("/",1)[-1]}.dump'
                    run(['debugfs','-R',f'dump {path} {dump}',str(target)],dump.stem)
                    data=dump.read_bytes();blocks=[data[i:i+bs] for i in range(0,len(data),bs)]
                    if path.endswith('/unit'):assert blocks.pop()==b'Z'*bs
                    assert sorted(blocks)==sorted(bytes([c])*bs for c in letters)
                dumped={}
                for name in ('grant.v1','ownership.v1'):
                    dump=out/f'{bs}-{sector}-{name}.dump'
                    run(['debugfs','-R',f'dump /pkg/{name} {dump}',str(target)],dump.stem);dumped[name]=dump.read_bytes()
                import struct
                ledger,table=dumped['grant.v1'],dumped['ownership.v1']
                assert struct.unpack_from('<6Q',ledger,296)==(8,1,5,0,3,1)
                assert hashlib.sha256(table).digest()==ledger[264:296] and hashlib.sha256(ledger[:992]).digest()==ledger[992:]
                for path,expected in (('/etc/passwd',b'root:x:0:0:root2:/root:/bin/shell\n'),('/etc/group',b'root:x:0:\n'),('/etc/shadow',b'root:!:::::::\n')):
                    dump=out/f'{bs}-{sector}-{path.rsplit("/",1)[-1]}.dump'
                    run(['debugfs','-R',f'dump {path} {dump}',str(target)],dump.stem);assert dump.read_bytes()==expected
                if args.binding:
                    for path in ('/work/b/kept','/work/a/parallel0','/work/b/parallel1'):
                        dump=out/f'{bs}-{sector}-{path.rsplit("/",1)[-1]}.dump'
                        run(['debugfs','-R',f'dump {path} {dump}',str(target)],dump.stem);assert dump.read_bytes()==b'namespace-data\n'
                    for path in ('/work/a/file','/work/a/victim','/work/a/sub','/work/b/sub','/work/a/cancelled-dir','/work/a/cancelled-file','/work/a/dead','/etc/forbidden','/pkg/forbidden'):
                        stat=run(['debugfs','-R',f'stat {path}',str(target)],f'{bs}-{sector}-absent-'+path.replace('/','-'))
                        assert 'File not found' in stat,(path,stat)
                assert sha(source)==original
                case=dict(block=bs,sector=sector,result='PASS');manifest['cases'].append(case);print(case,flush=True)
        manifest['result']='PASS'
    finally:
        manifest['changed_compiled_copies']=[p for p,h in manifest.get('compiled_inputs',{}).items() if sha(work/p)!=h]
        manifest['changed_compiled_repository_sources']=[p for p in manifest.get('compiled_inputs',{}) if p in baseline['sources'] and sha(ROOT/p)!=baseline['sources'][p]]
        if manifest['changed_compiled_copies'] or manifest['changed_compiled_repository_sources']:manifest['result']='FAIL_PRESERVATION'
        (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    assert manifest['result']=='PASS';print('Evidence:',out)


if __name__=='__main__':main()
