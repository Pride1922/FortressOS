"""Actual mounted root/VFS sanitizer regressions on copied disposable roots."""
import argparse, hashlib, json, shlex, subprocess, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def digest(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    parser=argparse.ArgumentParser();parser.add_argument('fixture',type=Path);parser.add_argument('--tsan',action='store_true');args=parser.parse_args()
    fixture=args.fixture.resolve();assert fixture.is_relative_to(ROOT/'build/network-smp-phase3')
    root=fixture/'root.img';login=fixture/'tree/bin/login';assert root.is_file() and login.is_file()
    out=Path(tempfile.mkdtemp(prefix='persistence-tsan-' if args.tsan else 'persistence-host-',dir=ROOT/'build/network-smp-phase3'));print(out,flush=True)
    result={'result':'FAIL','fixture':str(fixture),'fixture_sha256':digest(root),'cases':[]}
    try:
        exe=out/'host'
        argv=['gcc','-std=c11','-O1','-g','-fsanitize='+('thread' if args.tsan else 'address,undefined'),'-Wall','-Wextra','-Werror','-no-pie','-pthread',
            '-Itests/ext4_host','-Itests/host','-Isrc/include','-Isrc/fs','-Isrc/drivers','-Isrc/mm','-Isrc/arch/x86_64','-iquote','tests/host','-iquote','src/kernel',
            'tests/root_phase1_host.c','tests/ext4_fault_disk.c','-o',str(exe)]
        dependencies=subprocess.check_output(argv[:-2]+['-MM'],cwd=ROOT,text=True);(out/'dependencies').write_text(dependencies)
        sources={}
        for rule in dependencies.replace('\\\n',' ').splitlines():
            for name in shlex.split(rule.split(':',1)[1]):
                path=(ROOT/name).resolve()
                if path.is_relative_to(ROOT):sources[str(path.relative_to(ROOT))]=digest(path)
        result.update(build_argv=argv,sources=sources)
        with (out/'build.log').open('w') as log:subprocess.run(argv,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=60)
        result['binary_sha256']=digest(exe)
        for sector in (512,4096):
            target=out/f'root-{sector}.img'
            subprocess.run(['cp','--sparse=always',str(root),str(target)],check=True,timeout=30)
            run=[str(exe),str(target),str(sector),str(login)]
            if args.tsan:run=['setarch','x86_64','-R']+run
            case={'sector_size':sector,'argv':run};result['cases'].append(case)
            with (out/f'host-{sector}.log').open('w') as log:
                subprocess.run(run,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=600)
                subprocess.run(['e2fsck','-fn',str(target)],stdout=log,stderr=subprocess.STDOUT,check=True,timeout=60)
            case['result']='PASS'
        assert all(digest(ROOT/n)==sha for n,sha in sources.items())
        assert digest(root)==result['fixture_sha256']
        result['result']='PASS'
    except BaseException as error:result['failure']=repr(error);raise
    finally:(out/'manifest.json').write_text(json.dumps(result,indent=2)+'\n')
if __name__=='__main__':main()
