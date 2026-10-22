"""Private copied-source account/grant/registry/FD coupling; no kernel build."""
import argparse
import datetime
import hashlib
import json
import pathlib
import re
import shutil
import subprocess

ROOT=pathlib.Path(__file__).resolve().parent.parent


def function(source,signature):
    start=source.index(signature);brace=source.index('{',start);depth=1;end=brace+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--block',type=int,choices=(1024,2048,4096))
    parser.add_argument('--pre-replay',action='store_true')
    parser.add_argument('--build-only',action='store_true');args=parser.parse_args()
    prefix='pkg-mutation-context-' if args.build_only else 'pkg-service-integration-'
    out=ROOT/'.codex-remote-attachments'/(prefix+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'))
    work=out/'workspace';work.mkdir(parents=True)
    sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
    sources={}
    for directory in ('src','tests','user'):
        for p in (ROOT/directory).rglob('*'):
            if p.is_file() and p.suffix in ('.c','.h','.inc'):
                name=str(p.relative_to(ROOT));sources[name]=sha(p)
                dst=work/name;dst.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(p,dst);assert sha(dst)==sources[name]
    overlays=[]
    def replace(name,before,after):
        p=work/name;s=p.read_text();assert s.count(before)==1,(name,before)
        p.write_text(s.replace(before,after));overlays.append(dict(file=name,before=before,after=after,sha256=sha(p)))
    replace('src/fs/ext4_transaction.inc','static int e4_semantic_commit(ext4_engine_t *e) {','static int pg_life_core_commit(ext4_engine_t *e) {')
    if args.pre_replay:
        replace('src/fs/ext4_mount_journal.inc','    e4_engine_dispose(engine);engine=NULL;',
                '    if(integration_pre_required){e4_mount_stage(stage,"grant-preview");r=integration_pre_validate(engine);if(r)goto fail;}\n    e4_engine_dispose(engine);engine=NULL;')
        replace('src/fs/ext4_transaction.inc','    if (!r) r=jbd2_writer_checkpoint(e->writer);',
                '    if(!r&&integration_pending_cut){integration_pending_cut=false;r=-VFS_EIO;}\n    if (!r) r=jbd2_writer_checkpoint(e->writer);')
    replace('src/fs/ext4_write.inc','    if (!r) n->opens++;','    if(!r && pg_integration_pin_deny(node))r=-VFS_EAGAIN;\n    if (!r) n->opens++;')
    replace('src/fs/ext4_permissions.inc','    if (!r && node->open) n->opens++;','    if(!r && pg_integration_pin_deny(node))r=-VFS_EAGAIN;\n    if (!r && node->open) n->opens++;')
    replace('src/kernel/accounts.c','int r=ext4_root_account_headroom(storage.context);','int r=pg_integration_headroom(storage.context);')
    replace('src/kernel/accounts.c','        if (!r && generation==UINT64_MAX)', '        if(!r)r=pg_integration_headroom(storage.context);\n        if (!r && generation==UINT64_MAX)')
    replace('src/kernel/accounts.c','storage.validate=coherent;account_workspace_t *w=workspace();','storage.validate=coherent;int g=pg_integration_storage(fs,&storage);if(g)return g;account_workspace_t *w=workspace();')
    # Registry callbacks are deliberately outside its rank-1 exclusion.
    registry='src/kernel/process_table.c';p=work/registry
    for signature,hook in [('void process_record_commit(', 'pg_integration_birth'),('void process_record_abort(','pg_integration_detach'),
                           ('void process_record_forget(','pg_integration_detach'),('bool process_record_exit_accounted_times(','pg_integration_detach')]:
        old=function(p.read_text(),signature);needle='    spin_unlock_irqrestore(&g_process_lock, irq);'
        assert old.count(needle)==1
        replace(registry,old,old.replace(needle,needle+f'\n    {hook}(pid);'))
    old=p.read_text();p.write_text('#include "types.h"\nvoid pg_integration_birth(uint64_t);\nvoid pg_integration_detach(uint64_t);\n'+old)
    overlays.append(dict(file=registry,prefix='private callback declarations',sha256=sha(p)))
    base=(work/'tests/pkg_grant_fs_host.c').read_text().replace('int main(int argc,char **argv){','int pg_previous_main(int argc,char **argv){')
    base=base.replace('/mnt/pkg','/pkg')
    ns=(work/'tests/pkg_account_namespace_host.c').read_text();ns=ns[ns.index('static FILE *image;'):ns.index('int main(int argc,char **argv) {')]
    assert ns.count('static bool flush(block_dev_t *d) {(void)d;return !fflush(image);}')==1
    ns=ns.replace('static bool flush(block_dev_t *d) {(void)d;return !fflush(image);}',
                  'static bool flush(block_dev_t *d) {(void)d;integration_flushes++;return !fflush(image);}')
    # Generate an explicit detached fixture initializer from the previous exact
    # record encoding, without fake trusted flags or temporary root publication.
    wire=base[base.index('    marker_ino=ids[0];'):base.index('    e4_p16(table+256,1);')]
    wire='memset(marker,0,sizeof marker);memset(ledger,0,sizeof ledger);memset(table,0,sizeof table);memset(&binding,0,sizeof binding);binding.epoch=1;\n'+wire
    wire+='''e4_p16(ledger+22,PG_IDLE);memset(ledger+40,0,16);memcpy(ledger+200,ledger+168,32);
memset(ledger+296,0,48);e4_p16(ledger+348,0);p64(ledger+368,1);memset(ledger+392,0,40);
e4_p16(table+22,0);memset(table+32,0,16);seal_records();'''
    thread=(work/'src/kernel/thread.c').read_text()
    signatures=['int fd_init_std(','void fd_clone_table(','void fd_close_cloexec(','int fd_alloc(',
                'struct file *fd_get(','int fd_free(','int fd_dup2(','int fd_dup(','void fd_close_all(']
    fd_source='\n'.join(function(thread,s) for s in signatures)
    bridge=function((work/'src/kernel/syscall.c').read_text(),'int64_t syscall_from_vfs_error(')
    (out/'extracted-fd-functions.c').write_text(fd_source)
    prefix='''#define _POSIX_C_SOURCE 200809L
#include "tests/ext4_host/spinlock.h"
#define HOST_THREAD_H
#include "src/kernel/thread.h"
#include "account_redo.h"
tcb_t *thread_current(void){return NULL;}
struct ext4_engine;struct vfs_node;
static int e4_semantic_commit(struct ext4_engine *);
static bool pg_integration_pin_deny(struct vfs_node *);
static int pg_integration_headroom(void *);
static int pg_integration_storage(void *,account_io_t *);
static bool integration_pre_required;
static unsigned integration_pre_calls;
static unsigned integration_flushes;
static bool integration_pending_cut;
static int integration_pre_validate(struct ext4_engine *);
'''
    generated=work/'tests/integration.c'
    generated.write_text(prefix+base+'''
#define workspace integration_workspace
'''+ns+'''
#define main pg_lifecycle_previous_main
#define e4_semantic_commit pg_tagged_commit
#include "pkg_grant_lifecycle.inc"
#undef e4_semantic_commit
#undef main
#define main pg_idle_previous_main
#include "pkg_grant_idle.inc"
#undef main
#define main pg_capacity_previous_main
#define e4_semantic_commit pg_capacity_commit
#define pg_capacity_core_commit pg_life_core_commit
#include "pkg_account_capacity.inc"
#undef pg_capacity_core_commit
#undef e4_semantic_commit
#undef main
#undef workspace
#include "src/kernel/accounts.c"
#include "src/kernel/thread.h"
#include "src/kernel/process_table.h"
#include "pkg_writer_candidate.h"
uint64_t *vmm_get_active_pml4_virt(void){return (void *)1;}
bool vmm_validate_user_range(uint64_t *p,uintptr_t a,size_t n,bool output){(void)p;(void)output;return a>=4096&&a<0x800000000000ull&&n<=0x800000000000ull-a;}
'''+function((work/'src/kernel/syscall.c').read_text(),'int64_t syscall_from_vfs_error(')+'\n'+fd_source+'''
static void integration_seed_bytes(void){ext4_mount_t *fs=mounted;
'''+wire+'''
}
static void compete_free(ext4_mount_t *fs,uint64_t f[2]){uint8_t sb[1024];assert(e4_bytes(fs,1024,sb,sizeof sb));f[0]=e4_u32(sb+12);f[1]=e4_u32(sb+16);}
static uint64_t integration_compete_floor[2];
#define compete_floor integration_compete_floor
'''+function((work/'tests/pkg_grant_compete.inc').read_text(),'static bool compete_fits(')+'\n'+
        function((work/'tests/pkg_grant_compete.inc').read_text(),'static int compete_empty(')+'\n'+
        function((work/'tests/pkg_grant_compete.inc').read_text(),'static int compete_admit(')+'''
#undef compete_floor
#include "pkg_service_integration.inc"
''')
    manifest={'result':'INCOMPLETE','sources':sources,'overlays':overlays,'cases':[],
              'capacity_preview_negatives':[],
              'capacity_pending_replays':[],
              'fd_functions':signatures,'extracted_fd_sha256':sha(out/'extracted-fd-functions.c'),
              'error_bridge_sha256':hashlib.sha256(bridge.encode()).hexdigest(),
              'pre_replay':args.pre_replay,
              'scope':'private copied account/registry/open hooks, actual FD function extraction; synthetic enrollment, host VMM/memory/device; no production integration/boot/installation'}
    def run(cmd,label,timeout=1200):
        r=subprocess.run(cmd,cwd=work,text=True,capture_output=True,timeout=timeout)
        (out/(label+'.log')).write_text(r.stdout+r.stderr)
        assert r.returncode==0,(label,r.returncode,r.stdout[-1600:],r.stderr[-1600:]);return r.stdout+r.stderr
    try:
        flags=['gcc','-std=c11','-O1','-g','-fsanitize=address,undefined','-Wall','-Wextra','-Werror','-no-pie','-pthread',
               '-DTEST_SMP_MEMORY','-ffunction-sections','-fdata-sections','-Wl,--gc-sections']
        flags+=['-I'+p for p in ('tests','tests/ext4_host','tests/host','src/include','src/fs','src/drivers','src/mm','src/kernel','src/arch/x86_64','user/tools','.')]
        units=['tests/integration.c','tests/pkg_writer_candidate.c','tests/pkg_grant_candidate.c','tests/ext4_fault_disk.c','src/lib/account_redo.c',
               'src/kernel/process_table.c','src/kernel/creds.c','src/fs/permission_values.c','user/tools/digest.c','user/tools/userdb.c']
        deps=run(flags+['-MM','-MT','probe',*units],'dependencies')
        names={str((work/p).resolve().relative_to(work)) for p in deps.replace('\\\n',' ').split() if not p.endswith(':')}
        names.update(('src/kernel/thread.c','src/kernel/syscall.c'))
        manifest['compiled_inputs']={p:sha(work/p) for p in sorted(names)}
        manifest['compile_command']=flags+units+['-o',str(out/'probe')]
        run(flags+units+['-o',str(out/'probe')],'compile')
        if args.build_only:
            manifest['result']='BUILD_ONLY';print('Evidence:',out);return
        previous=ROOT/'.codex-remote-attachments/pkg-namespace-kernel-20261021-140820'
        prior=json.loads((previous/'manifest.json').read_text());assert prior['result']=='PASS'
        expected=(b'root:x:0:0:root2:/root:/bin/shell\n',b'root:x:0:\n',b'root:!:::::::\n')
        for bs in ((args.block,) if args.block else (1024,2048,4096)):
            for ss in (512,4096):
                src=previous/f'{bs}-{ss}-oom.img';digest=sha(src)
                assert next(c['image_sha256'] for c in prior['cases'] if c['name']==src.stem)==digest
                for mode in ('normal','interrupt'):
                    dst=out/f'{bs}-{ss}-{mode}.img';shutil.copyfile(src,dst)
                    run([str(out/'probe'),str(dst),str(ss),mode],f'{bs}-{ss}-{mode}')
                    if args.pre_replay and mode=='normal':
                        run([str(out/'probe'),str(dst),str(ss),'resume-probe'],f'{bs}-{ss}-idle-probe')
                        pending=out/f'{bs}-{ss}-capacity-pending.img';shutil.copyfile(dst,pending)
                        shutil.copyfile(str(dst)+'.ctx',str(pending)+'.ctx')
                        first=run([str(out/'probe'),str(pending),str(ss),'resume-capacity-pending'],pending.stem+'-commit')
                        second=run([str(out/'probe'),str(pending),str(ss),'resume-capacity-pending-check'],pending.stem+'-replay')
                        required=lambda text:tuple(map(int,re.search(r'preview-capacity R=(\d+),(\d+)',text).groups()))
                        old,new=required(first),required(second);assert new==(old[0],old[1]-1)
                        run(['e2fsck','-fn',str(pending)],pending.stem+'-linux')
                        stat=run(['debugfs','-R','stat /etc/group.publish',str(pending)],pending.stem+'-stat')
                        assert int(re.search(r'Size:\s*(\d+)',stat)[1])==0
                        assert int(re.search(r'Blockcount:\s*(\d+)',stat)[1])==0
                        assert int(re.search(r'Links:\s*(\d+)',stat)[1])==1
                        occupied=0
                        for name in ('passwd','group','shadow'):
                            stat=run(['debugfs','-R',f'stat /etc/{name}',str(pending)],pending.stem+'-'+name+'-stat')
                            sectors=int(re.search(r'Blockcount:\s*(\d+)',stat)[1]);assert sectors%(bs//512)==0
                            occupied+=sectors//(bs//512)
                        d=(8192+bs-1)//bs;c=9*(d+(1 if d>4 else 0))+2
                        assert new==(c-occupied,11-4)
                        manifest['capacity_pending_replays'].append(dict(block=bs,sector=ss,initial_R=old,replayed_R=new,result='PASS'))
                        before=sha(dst)
                        run([str(out/'probe'),str(dst),str(ss),'resume-capacity-oom'],f'{bs}-{ss}-capacity-oom')
                        assert sha(dst)==before
                        manifest['capacity_preview_negatives'].append(dict(block=bs,sector=ss,kind='workspace-oom',result='PASS'))
                        for kind in ('mode','oversize'):
                            bad=out/f'{bs}-{ss}-capacity-{kind}.img';shutil.copyfile(dst,bad)
                            shutil.copyfile(str(dst)+'.ctx',str(bad)+'.ctx')
                            if kind=='mode':
                                run(['debugfs','-w','-R','set_inode_field /etc/passwd mode 0100600',str(bad)],bad.stem+'-inject')
                            else:
                                run([str(out/'probe'),str(bad),str(ss),'resume-capacity-inject-oversize'],bad.stem+'-inject')
                            run(['e2fsck','-fn',str(bad)],bad.stem+'-linux')
                            before=sha(bad)
                            run([str(out/'probe'),str(bad),str(ss),'resume-capacity-reject'],bad.stem)
                            assert sha(bad)==before
                            manifest['capacity_preview_negatives'].append(dict(block=bs,sector=ss,kind=kind,result='PASS'))
                    if mode=='interrupt':
                        if args.pre_replay:
                            before=sha(dst)
                            run([str(out/'probe'),str(dst),str(ss),'resume-reject'],f'{bs}-{ss}-resume-reject')
                            assert sha(dst)==before
                        for res in ('resume-keep','resume-clean'):
                            run([str(out/'probe'),str(dst),str(ss),res],f'{bs}-{ss}-{res}')
                    run(['e2fsck','-fn',str(dst)],f'{bs}-{ss}-{mode}-linux')
                    for path,data in zip(('/etc/passwd','/etc/group','/etc/shadow'),expected):
                        dump=out/f'{bs}-{ss}-{mode}-{path.split("/")[-1]}.dump'
                        run(['debugfs','-R',f'dump {path} {dump}',str(dst)],dump.stem);assert dump.read_bytes()==data
                    l=out/f'{bs}-{ss}-{mode}-ledger.dump';t=out/f'{bs}-{ss}-{mode}-table.dump'
                    for name,dump in [('grant.v1',l),('ownership.v1',t)]:run(['debugfs','-R',f'dump /pkg/{name} {dump}',str(dst)],dump.stem)
                    ledger_bytes=l.read_bytes();table_bytes=t.read_bytes()
                    assert ledger_bytes[22:24]==bytes(2) and ledger_bytes[296:344]==bytes(48)
                    assert table_bytes[22:24]==bytes(2) and table_bytes[256:4352]==bytes(4096)
                    assert hashlib.sha256(table_bytes).digest()==ledger_bytes[264:296]
                    manifest['cases'].append(dict(block=bs,sector=ss,mode=mode,result='PASS'));print(manifest['cases'][-1],flush=True)
                assert sha(src)==digest
        manifest['result']='PASS'
    finally:
        manifest['changed_compiled_copies']=[p for p,h in manifest.get('compiled_inputs',{}).items() if sha(work/p)!=h]
        manifest['changed_compiled_repository_sources']=[p for p in manifest.get('compiled_inputs',{}) if p in sources and sha(ROOT/p)!=sources[p]]
        if manifest['changed_compiled_copies'] or manifest['changed_compiled_repository_sources']:
            manifest['result']='FAIL_PRESERVATION'
        (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    assert not manifest['changed_compiled_copies'] and not manifest['changed_compiled_repository_sources']
    print('Evidence:',out)


if __name__=='__main__':main()
