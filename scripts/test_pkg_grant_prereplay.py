"""Private grant validation of replay preview; copied sources, regular files."""
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
    out=ROOT/'.codex-remote-attachments'/('pkg-grant-prereplay-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
    out.mkdir(parents=True,exist_ok=False)
    manifest={'result':'INCOMPLETE','cases':[],'commands':[],
        'scope':'copied-source grant pre-replay overlay; actual JBD2 replay callback cuts of committed private block charge/refund; no production mount hooks, enrollment, IRQ/SMP, boot or Ring3 acceptance'}
    def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
    def run(cmd,label,timeout=180):
        manifest['commands'].append(cmd)
        r=subprocess.run(cmd,text=True,capture_output=True,cwd=work if work.exists() else ROOT,timeout=timeout)
        (out/(label+'.log')).write_text(r.stdout+r.stderr)
        assert r.returncode==0,(label,r.returncode,r.stdout[-1500:],r.stderr[-1500:])
        return r.stdout
    work=out/'workspace'
    import shutil
    for directory in ('src','tests','user'):
        for p in (ROOT/directory).rglob('*'):
            if p.is_file() and p.suffix in ('.c','.h','.inc'):
                dst=work/p.relative_to(ROOT);dst.parent.mkdir(parents=True,exist_ok=True)
                shutil.copyfile(p,dst)
    overlay=work/'src/fs/ext4_mount_journal.inc'
    text=overlay.read_text();needle='    e4_engine_dispose(engine);engine=NULL;'
    assert text.count(needle)==1
    overlay.write_text(text.replace(needle,'    if(pg_pre_enabled){e4_mount_stage(stage,"grant-preview");r=pg_pre_validate(engine);if(r)goto fail;}\n'+needle))
    source=(ROOT/'tests/pkg_grant_fs_host.c').read_text()
    assert source.count('int main(int argc,char **argv){')==1
    generated=work/'tests/recovery.c'
    source=source.replace('static int validate(ext4_mount_t *fs,pg_view *out){','static int validate_engine(ext4_mount_t *fs,ext4_engine_t *engine,pg_view *out){').replace('e4_orphan_validate(fs->engine)','e4_orphan_validate(engine)')
    pos=source.index('static void discover(')
    source=source[:pos]+'static int validate(ext4_mount_t *fs,pg_view *v){return validate_engine(fs,fs->engine,v); }\n'+source[pos:]
    source='struct ext4_engine;static int pg_pre_validate(struct ext4_engine *);static int pg_pre_enabled;\n'+source
    recovery=(ROOT/'tests/pkg_grant_fs_recovery.inc').read_text()
    recovery=recovery.replace('seed(fs);','seed(fs);pre_capture(fs);')
    recovery=recovery.replace('    unsigned cuts=0;','    pg_pre_enabled=1;unsigned cuts=0;')
    recovery=recovery.replace('        assert(found);','        assert(found);pre_negative(&dev,pending,op);')
    generated.write_text(source.replace('int main(int argc,char **argv){','int pg_positive_main(int argc,char **argv){')+
                         '\n#include "pkg_grant_prereplay.inc"\n'+recovery)
    flags=['gcc','-std=c11','-O1','-g','-fsanitize=address,undefined','-Wall','-Wextra','-Werror',
           '-no-pie','-pthread','-ffunction-sections','-fdata-sections','-Wl,--gc-sections']
    flags+=['-I'+p for p in ('tests','tests/ext4_host','tests/host','src/include','src/fs','src/drivers','src/mm','user/tools','.')]
    units=[str(generated),'tests/pkg_grant_candidate.c','tests/ext4_fault_disk.c',
           'src/fs/permission_values.c','src/kernel/creds.c','user/tools/digest.c']
    dep=run(flags+['-MM','-MT','probe',*units],'dependencies')
    paths={str((work/token).resolve().relative_to(work)) for token in dep.replace('\\\n',' ').split() if not token.endswith(':')}
    paths.update(('tests/pkg_grant_fs_host.c','tests/pkg_grant_fs_recovery.inc',
                  'scripts/test_pkg_grant_prereplay.py','AGENTS.md','README.md'))
    manifest['inputs']={p:sha(ROOT/p) for p in sorted(paths) if (ROOT/p).exists()}
    manifest['compiled_copies']={p:sha(work/p) for p in sorted(paths) if (work/p).exists()}
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
                assert text.count('negative=7 zero-write-flush positive=1')==2
                manifest['cases'].append({'block':bs,'sector':ss,'recovery_cuts':cuts,'restarts_per_cut':2,
                                         'zero_write_rejections':14,'positive_pending_mounts':2,
                                         'linux_audits':2,'result':'PASS'})
                print(manifest['cases'][-1],flush=True)
            assert sha(image)==original
        manifest['result']='PASS'
    finally:
        manifest['changed_copies']=[p for p,h in manifest['compiled_copies'].items() if sha(work/p)!=h]
        manifest['changed_inputs']=[p for p,h in manifest['inputs'].items() if sha(ROOT/p)!=h]
        (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    assert not manifest['changed_inputs'] and not manifest['changed_copies']
    print('Evidence:',out)


if __name__=='__main__':main()
