"""Private CLI actual-code sanitizers and freestanding stack check.

Only retained private build outputs; no canonical media prerequisites.
"""
import datetime,json,pathlib,shutil,subprocess
from test_pkg_cli_host import SOURCES,VENDORS,INCLUDES
from test_pkg_boot_admission import ROOT,H

def main():
    out=ROOT/'.codex-remote-attachments'/('pkg-exec-cli-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'));out.mkdir()
    work=out/'workspace'
    inputs=SOURCES+VENDORS+['tests/pkg_exec_cli.c','tests/pkg_exec_cli.h','tests/pkg_exec_cli_host.c',
        'tests/pkg_exec_wire.h','user/pkg/transaction_cli.c','user/pkg/mutation_cli.c']
    before={p:H(ROOT/p) for p in inputs}
    m={'runtime':'INCOMPLETE','preservation':'INCOMPLETE','commands':[],'inputs':before}
    def run(args,label):
        m['commands'].append(args)
        r=subprocess.run(args,cwd=work,capture_output=True,text=True,timeout=120)
        (out/(label+'.log')).write_text(r.stdout+r.stderr);assert not r.returncode,(label,r.stderr[-4000:])
    try:
        for name in ('src','user','tests'):shutil.copytree(ROOT/name,work/name,ignore=shutil.ignore_patterns('__pycache__'))
        assert all(H(work/p)==h for p,h in before.items())
        flags=['gcc','-std=c11','-Wall','-Wextra','-Werror','-O1','-g','-DTOOL_HOST_TEST',
            '-fsanitize=address,undefined','-fno-omit-frame-pointer','-ffunction-sections','-fdata-sections',*INCLUDES,'-Itests']
        objects=[]
        for i,path in enumerate(VENDORS):
            obj=str(out/f'mono-{i}.o');run(flags+['-include','src/lib/crypto/namespace.h','-c',path,'-o',obj],f'crypto-{i}');objects.append(obj)
        run(flags+['-no-pie','-Wl,--gc-sections',*SOURCES,'user/pkg/transaction_cli.c','user/pkg/mutation_cli.c',
            'tests/pkg_exec_cli.c','tests/pkg_exec_cli_host.c',*objects,'-o',str(out/'host')],'compile')
        run([str(out/'host')],'runtime')
        run(['gcc','-std=c11','-Os','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin',
            '-fno-pie','-fno-stack-protector','-mno-red-zone','-mno-sse','-mno-mmx','-msoft-float',
            '-fstack-usage','-Wframe-larger-than=1024',*INCLUDES,'-Itests',
            '-c','tests/pkg_exec_cli.c','-o',str(out/'private-cli.o')],'freestanding')
        m['frames']={s.split('\t')[0]:int(s.split('\t')[1]) for s in (out/'private-cli.su').read_text().splitlines()}
        assert m['frames'] and max(m['frames'].values())<=1024
        m['runtime']='PASS'
    except BaseException as e:m['runtime']='FAIL';m['error']=repr(e);raise
    finally:
        m['changed_sources']=[p for p,h in before.items() if H(ROOT/p)!=h]
        m['changed_copies']=[p for p,h in before.items() if H(work/p)!=h]
        m['preservation']='FAIL' if m['changed_sources'] or m['changed_copies'] else 'PASS'
        (out/'manifest.json').write_text(json.dumps(m,indent=2)+'\n');print(out,m['runtime'],m['preservation'],flush=True)
if __name__=='__main__':main()
