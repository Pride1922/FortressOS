"""Private cancellation at every multi-commit truncate/unlink boundary."""
import argparse
import hashlib
import json
import pathlib
import re
import shutil
import subprocess
import sys
from test_pkg_service_integration import function
from pkg_call_overlay import integration_overlay

ROOT=pathlib.Path(__file__).resolve().parent.parent


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--block',type=int,choices=(1024,2048,4096));parser.add_argument('--tsan',action='store_true');parser.add_argument('--account',action='store_true');parser.add_argument('--account-all-files',action='store_true');args=parser.parse_args()
    if args.account_all_files:assert args.block==4096 and not args.tsan;args.account=True
    b=subprocess.run([sys.executable,'scripts/test_pkg_service_integration.py','--build-only','--pre-replay'],cwd=ROOT,capture_output=True,text=True,timeout=180)
    assert not b.returncode,(b.stdout,b.stderr);out=pathlib.Path(re.search(r'Evidence: (.+)',b.stdout)[1]).resolve()
    assert out.parent==(ROOT/'.codex-remote-attachments').resolve();work=out/'workspace';base=json.loads((out/'manifest.json').read_text())
    assert base['result']=='BUILD_ONLY' and not base['changed_compiled_repository_sources'];shutil.copyfile(out/'manifest.json',out/'build-only-manifest.json')
    overlays=[];sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
    def replace(name,before,after):
        p=work/name;s=p.read_text();assert s.count(before)==1,(name,before);p.write_text(s.replace(before,after));overlays.append(dict(file=name,before=before,after=after))
    p='tests/pkg_service_integration.inc'
    for signature,body in [('static int e4_semantic_commit(ext4_engine_t *e){','return integrated?mutation_dispatch(e):pg_life_core_commit(e);'),
                           ('static int pg_integration_headroom(void *context){','return mutation_headroom(context);'),
                           ('static int pg_integration_storage(void *context,account_io_t *io){','return mutation_storage(context,io);')]:
        replace(p,function((work/p).read_text(),signature),signature+body+'}')
    replace(p,'int main(int argc,char **argv){','int mutation_previous_main(int argc,char **argv){')
    replace('tests/integration.c','#include "pkg_service_integration.inc"',
            'static int mutation_dispatch(ext4_engine_t *);\nstatic int mutation_headroom(void *);\nstatic int mutation_storage(void *,account_io_t *);\n#include "pkg_service_integration.inc"\n#include "pkg_mutation_integration.inc"')
    integration_overlay(work,replace)
    replace('tests/pkg_mutation_integration.inc','int main(int argc,char **argv){','int call_fixture_main(int argc,char **argv){')
    replace('tests/integration.c','struct ext4_engine;struct vfs_node;',
            'struct ext4_engine;struct vfs_node;\nstatic int cancel_dispatch(struct ext4_engine *);\nstatic bool cancel_preview_permit(struct ext4_engine *);')
    p='tests/pkg_call_integration.inc';old=function((work/p).read_text(),'static int mutation_dispatch(ext4_engine_t *e){')
    replace(p,old,'static int mutation_dispatch(ext4_engine_t *e){return cancel_dispatch(e);}')
    replace('tests/integration.c','#include "pkg_call_integration.inc"','#include "pkg_call_integration.inc"\n#include "pkg_cancel_recovery.inc"')
    replace('tests/pkg_service_integration.inc','if(engine->orphan_count||engine->orphan_recovery_required)return -VFS_EIO;',
            'if(!cancel_preview_permit(engine))return -VFS_EIO;')
    replace('tests/pkg_account_preview.inc','engine->orphan_count||engine->orphan_recovery_required',
            '!cancel_preview_permit(engine)')
    if args.account:
        replace('tests/pkg_cancel_recovery.inc','int main(int argc,char **argv){','int cancel_fixture_main(int argc,char **argv){')
        replace('tests/integration.c','#include "pkg_cancel_recovery.inc"','#include "pkg_cancel_recovery.inc"\n#include "pkg_account_cancel.inc"')
        replace('tests/integration.c','static int cancel_dispatch(struct ext4_engine *);',
                'static int cancel_dispatch(struct ext4_engine *);\nstatic int account_cancel_after(int,unsigned);\nstatic int account_cancel_sync(void *);')
        p='tests/pkg_mutation_integration.inc'
        replace(p,'mutation_call *call=mutation_current;\n    if(context!=mounted',
                'mutation_call *call=mutation_current;\n    if(pc_cancelled(thread_current()))return -VFS_EINTR;\n    if(context!=mounted')
        for signature,expression in (
            ('static int mutation_account_write(void *p,unsigned role,const void *bytes,size_t n){','backend.write(p,role,bytes,n)'),
            ('static int mutation_account_remove(void *p,unsigned role){','backend.remove(p,role)'),
            ('static int mutation_account_rename(void *p,unsigned from,unsigned to){','backend.rename(p,from,to)')):
            old=function((work/p).read_text(),signature)
            replace(p,old,signature+'unsigned before=writes_seen;int r=mutation_headroom(p);return account_cancel_after(r?r:mutation_after('+expression+'),before);}')
        replace(p,'io->write=mutation_account_write;', 'io->sync=account_cancel_sync;io->write=mutation_account_write;')
        if args.account_all_files:
            replace('tests/pkg_account_cancel.inc','int main(int argc,char **argv){','int account_variant_previous_main(int argc,char **argv){')
            replace('tests/integration.c','#include "pkg_account_cancel.inc"','#include "pkg_account_cancel.inc"\n#include "pkg_account_variant.inc"')
    command=base['compile_command'];command.insert(1,'-DTEST_EXT4_ATOMIC_COUNTERS')
    if args.tsan:command=[c for c in command if c!='-fsanitize=address,undefined'];command.insert(1,'-fsanitize=thread')
    m={'result':'INCOMPLETE','kind':'private-account-cancel-recovery' if args.account else 'private-cancel-recovery','sanitizer':'TSan' if args.tsan else 'ASan/UBSan',
       'sources':base['sources'],'overlays':overlays,'cases':[],'commands':[],
       'scope':'semantic-boundary cancellation, fresh-process grant/capacity preview and ordinary orphan recovery; no sector-cut/account-orphan/kernel-boot acceptance'}
    if args.account_all_files:m['kind']='private-account-all-files-cancel-recovery'
    if args.account:m['scope']='completed mutating account callback cuts, actual account service and fresh-process grant/capacity preflight; no interruption inside account callbacks, account-orphan admission, sector-cut or kernel-account-update acceptance'
    def run(cmd,label):
        m['commands'].append(cmd);r=subprocess.run(cmd,cwd=work,text=True,capture_output=True,timeout=600)
        (out/(label+'.log')).write_text(r.stdout+r.stderr);assert not r.returncode,(label,r.returncode,r.stdout[-1200:],r.stderr[-2200:]);return r.stdout+r.stderr
    def probe(path,ss,mode,label):
        cmd=[str(out/'probe'),str(path),str(ss),mode]
        return run((['setarch','x86_64','-R'] if args.tsan else [])+cmd,label)
    def clone(source,target):
        shutil.copyfile(source,target);shutil.copyfile(str(source)+'.ctx',str(target)+'.ctx')
    def audit(path,bs,ss,op,label,forward=False):
        run(['e2fsck','-fn',str(path)],label+'-linux')
        for name,expected in (('/pkg/transaction.v1/unit',b'K'*(3*bs)),('/etc/passwd',b'root:x:0:0:root2:/root:/bin/shell\n' if forward else b'root:x:0:0:root:/root:/bin/shell\n'),('/etc/group',b'root:x:0:root\n' if forward and args.account_all_files else b'root:x:0:\n'),('/etc/shadow',b'root:*:::::::\n' if forward and args.account_all_files else b'root:!:::::::\n')):
            dump=out/(label+'-'+name.rsplit('/',1)[-1]+'.dump');run(['debugfs','-R',f'dump {name} {dump}',str(path)],dump.stem);assert dump.read_bytes()==expected,(name,dump.read_bytes()[:100])
        if args.account:
            for name in ('/etc/passwd.publish','/etc/group.publish','/etc/shadow.publish','/var/lib/fortress/accounts/passwd.new','/var/lib/fortress/accounts/group.new','/var/lib/fortress/accounts/shadow.new','/var/lib/fortress/accounts/intent.tmp','/var/lib/fortress/accounts/pending'):
                assert 'File not found' in run(['debugfs','-R','stat '+name,str(path)],label+'-absent-'+name.rsplit('/',1)[-1])
        elif op=='truncate':
            dump=out/(label+'-empty.dump');run(['debugfs','-R',f'dump /work/a/recover {dump}',str(path)],dump.stem);assert dump.read_bytes()==b''
        else:assert 'File not found' in run(['debugfs','-R','stat /work/a/recover',str(path)],label+'-absent')
    try:
        dep=run(command[:command.index('-o')]+['-MM','-MT','probe'],'cancel-dependencies');names={str((work/p).resolve().relative_to(work)) for p in dep.replace('\\\n',' ').split() if not p.endswith(':')};names.update(('src/kernel/thread.c','src/kernel/syscall.c'))
        m['compiled_inputs']={p:sha(work/p) for p in sorted(names)};run(command,'cancel-compile')
        previous=ROOT/'.codex-remote-attachments/pkg-namespace-kernel-20261021-140820';prior=json.loads((previous/'manifest.json').read_text());assert prior['result']=='PASS'
        for bs in ((args.block,) if args.block else (1024,2048,4096)):
            for ss in ((512,) if args.account_all_files else (512,4096)):
                source=previous/f'{bs}-{ss}-oom.img';original=sha(source);assert next(c['image_sha256'] for c in prior['cases'] if c['name']==source.stem)==original
                seed=out/f'{bs}-{ss}-seed.img';shutil.copyfile(source,seed);probe(seed,ss,'seed',seed.stem)
                for op in (('account',) if args.account else ('truncate','unlink')):
                    normal=out/f'{bs}-{ss}-{op}-normal.img';clone(seed,normal);text=probe(normal,ss,op,normal.stem)
                    total=int(re.search(r'callbacks=(\d+) interrupted=0' if args.account else r'commits=(\d+) interrupted=0',text)[1]);assert total>=4;audit(normal,bs,ss,op,normal.stem,args.account)
                    cuts=(3,7,8,10,11,13,14,15,18) if args.account_all_files else range(1,total+1 if args.account else total)
                    for cut in cuts:
                        name=f'{bs}-{ss}-{op}-{cut}';target=out/(name+'.img');clone(seed,target)
                        interrupted=probe(target,ss,f'{op}={cut}',name+'-cut');before=sha(target)
                        forward=bool(int(re.search(r'forward=(\d)',interrupted)[1])) if args.account else False
                        probe(target,ss,'reject',name+'-reject');assert sha(target)==before
                        for attempt in (1,2):probe(target,ss,'recover',name+f'-recover{attempt}');audit(target,bs,ss,op,name+f'-audit{attempt}',forward)
                        m['cases'].append(dict(block=bs,sector=ss,operation=op,cut=cut,total=total,forward=forward,result='PASS'))
                    print(dict(block=bs,sector=ss,operation=op,cuts=len(cuts),result='PASS'),flush=True)
                assert sha(source)==original
        m['result']='PASS'
    finally:
        m['changed_compiled_copies']=[p for p,h in m.get('compiled_inputs',{}).items() if sha(work/p)!=h]
        m['changed_compiled_repository_sources']=[p for p in m.get('compiled_inputs',{}) if p in base['sources'] and sha(ROOT/p)!=base['sources'][p]]
        if m['changed_compiled_copies'] or m['changed_compiled_repository_sources']:m['result']='FAIL_PRESERVATION'
        (out/'manifest.json').write_text(json.dumps(m,indent=2)+'\n')
    assert m['result']=='PASS';print('Evidence:',out)


if __name__=='__main__':main()
