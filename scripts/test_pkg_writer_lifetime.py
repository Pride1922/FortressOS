"""Private writer references with actual process registry and host lifecycle notifications."""
import argparse
import datetime
import hashlib
import json
import pathlib
import re
import shutil
import subprocess

ROOT=pathlib.Path(__file__).resolve().parent.parent


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--backend',action='store_true');args=parser.parse_args()
    out=ROOT/'.codex-remote-attachments'/(('pkg-backend-' if args.backend else 'pkg-writer-lifetime-')+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'))
    work=out/'workspace';work.mkdir(parents=True,exist_ok=False)
    sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
    sources={}
    for directory in ('src','tests','user'):
        for p in (ROOT/directory).rglob('*'):
            if p.is_file() and p.suffix in ('.c','.h','.inc'):
                name=str(p.relative_to(ROOT));sources[name]=sha(p)
                target=work/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(p,target)
                assert sha(target)==sources[name]
    manifest={'result':'INCOMPLETE','sources':sources,'cases':[],'commands':[],
        'scope':'actual process registry; private owner/value arena and host birth/death/exec notifications; durable IDLE is a trusted mock input; no production/TCB/FD/FS integration'}
    def run(cmd,label,timeout=180):
        manifest['commands'].append(cmd)
        r=subprocess.run(cmd,cwd=work,text=True,capture_output=True,timeout=timeout)
        (out/(label+'.log')).write_text(r.stdout+r.stderr)
        assert r.returncode==0,(label,r.returncode,r.stdout[-2000:],r.stderr[-2000:])
        return r.stdout+r.stderr
    try:
        flags=['gcc','-std=c11','-O1','-g','-fsanitize=address,undefined','-Wall','-Wextra','-Werror',
               '-no-pie','-pthread','-D_XOPEN_SOURCE=700','-DTEST_SMP_MEMORY',
               '-Itests/ext4_host','-Itests','-Isrc/include','-Isrc/kernel']
        units=['tests/pkg_writer_lifetime_host.c','tests/pkg_writer_candidate.c','src/kernel/process_table.c','src/kernel/creds.c']
        if args.backend:
            flags+=['-I.','-Isrc/fs']
            units[0]='tests/pkg_backend_host.c';units.append('tests/pkg_backend_candidate.c')
        dep=run(flags+['-MM','-MT','probe',*units],'dependencies')
        dependencies={str((work/p).resolve().relative_to(work)) for p in dep.replace('\\\n',' ').split() if not p.endswith(':')}
        manifest['compiled_inputs']={p:sha(work/p) for p in sorted(dependencies)}
        run(flags+units+['-o',str(out/'probe')],'compile')
        if args.backend:
            manifest['scope']='private value backend and actual process registry; host lifecycle notifications, mocked storage; no syscall, TCB, mount or boot acceptance'
            free=['gcc','-std=c11','-Os','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-fPIE','-fno-stack-protector','-mno-red-zone','-mno-sse','-mno-mmx','-msoft-float','-fstack-usage','-Wframe-larger-than=1024','-Itests','-I.','-Isrc/include','-Isrc/kernel','-Isrc/fs']
            run(free+['-c','tests/pkg_backend_candidate.c','-o',str(out/'backend-free.o')],'freestanding-backend')
        result=run([str(out/'probe')],'runtime')
        assert ('PASS backend lifetimes=128' if args.backend else 'PASS writer lifetimes=128') in result
        manifest['cases'].append({'lifetimes':128,'result':'PASS'})
        manifest['result']='PASS'
    except Exception as error:
        manifest['result']='FAIL';manifest['error']=repr(error);raise
    finally:
        manifest['changed_compiled_copies']=[p for p,h in manifest.get('compiled_inputs',{}).items() if sha(work/p)!=h]
        manifest['changed_repository_sources']=[p for p,h in sources.items() if sha(ROOT/p)!=h]
        compiled_repo={p for p in manifest.get('compiled_inputs',{}) if p in sources}
        manifest['changed_compiled_repository_sources']=[p for p in compiled_repo if sha(ROOT/p)!=sources[p]]
        (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    assert not manifest['changed_compiled_copies'] and not manifest['changed_compiled_repository_sources']
    print('Evidence:',out)


if __name__=='__main__':main()
