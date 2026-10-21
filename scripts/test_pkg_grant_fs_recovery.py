"""Private committed grant transaction: actual JBD2 replay cut campaign."""
import datetime
import argparse
import hashlib
import json
import pathlib
import re
import subprocess
from test_ext4_time_host import fixture

ROOT=pathlib.Path(__file__).resolve().parent.parent


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--block',type=int,choices=(1024,2048,4096))
    args=parser.parse_args()
    out=ROOT/'.codex-remote-attachments'/('pkg-grant-fs-recovery-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
    out.mkdir(parents=True,exist_ok=False)
    manifest={'result':'INCOMPLETE','cases':[],'commands':[],
        'scope':'actual JBD2 replay callback cuts of committed private block charge/refund; no production mount hooks, enrollment, IRQ/SMP, boot or Ring3 acceptance'}
    def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
    def run(cmd,label,timeout=180):
        manifest['commands'].append(cmd)
        r=subprocess.run(cmd,text=True,capture_output=True,cwd=ROOT,timeout=timeout)
        (out/(label+'.log')).write_text(r.stdout+r.stderr)
        assert r.returncode==0,(label,r.returncode,r.stdout[-1500:],r.stderr[-1500:])
        return r.stdout
    source=(ROOT/'tests/pkg_grant_fs_host.c').read_text()
    assert source.count('int main(int argc,char **argv){')==1
    generated=out/'recovery.c'
    generated.write_text(source.replace('int main(int argc,char **argv){','int pg_positive_main(int argc,char **argv){')+
                         '\n#include "pkg_grant_fs_recovery.inc"\n')
    flags=['gcc','-std=c11','-O1','-g','-fsanitize=address,undefined','-Wall','-Wextra','-Werror',
           '-no-pie','-pthread','-ffunction-sections','-fdata-sections','-Wl,--gc-sections']
    flags+=['-I'+p for p in ('tests','tests/ext4_host','tests/host','src/include','src/fs','src/drivers','src/mm','user/tools','.')]
    units=[str(generated),'tests/pkg_grant_candidate.c','tests/ext4_fault_disk.c',
           'src/fs/permission_values.c','src/kernel/creds.c','user/tools/digest.c']
    dep=run(flags+['-MM','-MT','probe',*units],'dependencies')
    paths={str((ROOT/token).resolve().relative_to(ROOT)) for token in dep.replace('\\\n',' ').split() if not token.endswith(':')}
    paths.update(('tests/pkg_grant_fs_host.c','scripts/test_pkg_grant_fs_recovery.py','AGENTS.md','README.md'))
    manifest['inputs']={p:sha(ROOT/p) for p in sorted(paths)}
    try:
        run(flags+units+['-o',str(out/'probe')],'compile')
        for bs in ((args.block,) if args.block else (1024,2048,4096)):
            image=fixture(out,bs,inodes=128,size_mib=32 if bs==4096 else 16);original=sha(image)
            for ss in (512,4096):
                prefix=out/f'{bs}-{ss}'
                text=run([str(out/'probe'),str(image),str(ss),str(prefix)],f'{bs}-{ss}',timeout=900)
                cuts=int(re.search(r'recovery-cuts=(\d+)',text)[1])
                for op in ('charge','refund'):
                    run(['e2fsck','-fn',str(prefix)+'-'+op+'-replay.img'],f'{bs}-{ss}-{op}-linux')
                manifest['cases'].append({'block':bs,'sector':ss,'recovery_cuts':cuts,'restarts_per_cut':2,'linux_audits':2,'result':'PASS'})
                print(manifest['cases'][-1],flush=True)
            assert sha(image)==original
        manifest['result']='PASS'
    finally:
        manifest['changed_inputs']=[p for p,h in manifest['inputs'].items() if sha(ROOT/p)!=h]
        (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    assert not manifest['changed_inputs']
    print('Evidence:',out)


if __name__=='__main__':main()
