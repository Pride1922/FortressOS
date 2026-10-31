"""Actual normal package transport/owners with host VMM and storage adapters."""
import datetime,json,pathlib,shutil,subprocess,hashlib
ROOT=pathlib.Path(__file__).resolve().parents[1]
H=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()

def main():
    out=ROOT/'.codex-remote-attachments'/('pkg-system-host-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'))
    work=out/'workspace';work.mkdir(parents=True)
    inputs={str(p.relative_to(ROOT)):H(p) for folder in ('src','user','tests') for p in (ROOT/folder).rglob('*') if p.is_file() and p.suffix in ('.c','.h','.inc')}
    m={'runtime':'INCOMPLETE','preservation':'INCOMPLETE','inputs':inputs,'commands':[]}
    def run(args,label):
        m['commands'].append(args);r=subprocess.run(args,cwd=work,capture_output=True,text=True,timeout=120)
        (out/(label+'.log')).write_text(r.stdout+r.stderr);assert not r.returncode,(label,r.stdout[-2000:],r.stderr[-4000:])
    try:
        for folder in ('src','user','tests'):shutil.copytree(ROOT/folder,work/folder,ignore=shutil.ignore_patterns('__pycache__'))
        assert all(H(work/p)==v for p,v in inputs.items())
        flags=['gcc','-std=c11','-Wall','-Wextra','-Werror','-O1','-g','-no-pie',
            '-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread','-D_XOPEN_SOURCE=700',
            '-DTEST_SMP_MEMORY','-Itests/ext4_host','-Itests','-I.','-Isrc/include','-Isrc/kernel','-Isrc/mm','-Isrc/fs','-Isrc/drivers','-Isrc/arch/x86_64']
        run(flags+['tests/pkg_system_host.c','src/kernel/pkg_service.c','src/kernel/pkg_owner.c',
            'src/kernel/pkg_backend.c','src/kernel/process_table.c','src/kernel/creds.c','-o',str(out/'host')],'compile')
        run([str(out/'host')],'runtime')
        run(flags+['-DFORTRESS_PKG_FETCH=1','tests/pkg_install_source_host.c',
            'user/pkg/system_cli.c','user/pkg/format.c','user/pkg/manifest.c','-o',str(out/'frontend')],'frontend-compile')
        run([str(out/'frontend')],'frontend-runtime')
        freestanding=['gcc','-std=c11','-Os','-Wall','-Wextra','-Werror','-ffreestanding',
            '-fno-builtin','-fno-pie','-fno-stack-protector','-mno-red-zone','-mno-sse','-mno-mmx',
            '-msoft-float','-fstack-usage','-Wframe-larger-than=1024',
            '-I.','-Isrc/include','-Isrc/kernel','-Isrc/mm','-Isrc/fs','-Isrc/drivers','-Isrc/arch/x86_64']
        run(freestanding+['-c','src/kernel/pkg_service.c','-o',str(out/'service.o')],'service-freestanding')
        run(freestanding+['-DFORTRESS_PKG_FETCH=1','-c','user/pkg/system_cli.c','-o',str(out/'frontend.o')],'frontend-freestanding')
        m['frames']={str(p.name):p.read_text() for p in out.glob('*.su')}
        assert m['frames'] and all(int(s.split('\t')[1])<=1024 for p in out.glob('*.su') for s in p.read_text().splitlines())
        m['runtime']='PASS'
    except BaseException as e:m['runtime']='FAIL';m['error']=repr(e);raise
    finally:
        m['changed_sources']=[p for p,v in inputs.items() if not (ROOT/p).is_file() or H(ROOT/p)!=v]
        m['changed_copies']=[p for p,v in inputs.items() if not (work/p).is_file() or H(work/p)!=v]
        m['preservation']='FAIL' if m['changed_sources'] or m['changed_copies'] else 'PASS'
        (out/'manifest.json').write_text(json.dumps(m,indent=2)+'\n');print(out,m['runtime'],m['preservation'],flush=True)
if __name__=='__main__':main()
