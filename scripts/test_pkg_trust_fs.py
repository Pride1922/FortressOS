"""Signed floor durability through the actual EXT4/JBD2 host fault disk."""
import inspect,json,os,pathlib,re,subprocess,sys,tempfile
from concurrent.futures import ThreadPoolExecutor,as_completed
import test_pkg_pending as gate
import pkg_running_root_records as running
import pkg_trust_records as trust
from pkg_boot_admission_overlay import overlay as base
def overlay(work,replace):
    base(work,replace)
    replace('tests/pkg_pending_fs.inc','static const char *names[][10]','static const char *names[][11]')
    replace('tests/pkg_pending_fs.inc','"descriptor.v2","artifact.fpk"}','"descriptor.v2","artifact.fpk","trust-floor.v1"}')
    replace('tests/pkg_pending_fs.inc','count=d==0?10:','count=d==0?11:')
    replace('tests/pkg_pending_fs.inc','seen!=4095','seen!=8191')
    p='tests/pkg_pending_fs_host.c'
    replace(p,'#include "pkg_pending_fs.inc"','#include "pkg_pending_fs.inc"\n#include "pkg_trust_fs.inc"')
    replace(p,'long tool_syscall(', 'static ext4_fault_disk_t *tf_fault;\nlong tool_syscall(')
    replace(p,'    if(!r)r=pn_load(e->fs);','    if(!r)r=pn_load(e->fs);\n    if(!r)r=ft_load(e->fs);')
    replace(p,'    int r=0;unsigned steps=0;','    if(getenv("TF_TRACE"))printf("TF PRE BEGIN %zu\\n",tf_fault->events);\n    uint64_t flags=spin_lock_irqsave(&e4_lock);int r=ft_publish(e,1);spin_unlock_irqrestore(&e4_lock,flags);if(getenv("TF_TRACE"))printf("TF PRE END %zu\\n",tf_fault->events);if(r)return r;unsigned steps=0;')
    replace(p,'    return r;\n}\nstatic size_t exact','    if(!r){if(getenv("TF_TRACE"))printf("TF END BEGIN %zu\\n",tf_fault->events);flags=spin_lock_irqsave(&e4_lock);r=ft_publish(e,2);spin_unlock_irqrestore(&e4_lock,flags);if(getenv("TF_TRACE"))printf("TF END END %zu\\n",tf_fault->events);}\n    return r;\n}\nstatic size_t exact')
    replace(p,'pn_baseline_n=cn-1152;','pn_baseline_n=e4_u32(capsule+1168);assert(cn==1152+pn_baseline_n+3072);memcpy(ft_module,capsule+1152+pn_baseline_n,3072);ft_active=true;')
    replace(p,'d.cut=cut;assert(!native_open(&disk,&e));','d.cut=cut;d.after=getenv("TF_AFTER")!=NULL;d.tear_bytes=getenv("TF_TEAR")?256:0;assert(!native_open(&disk,&e));')
    replace(p,'    block_dev_t disk={','    tf_fault=&d;d.trace=getenv("TF_TRACE")!=NULL;\n    block_dev_t disk={')
    replace(p,'ext4_fault_restart(&d);assert(!native_open(&disk,&e));assert(!native_recover(e));events=d.events;',
            'ext4_fault_restart(&d);r=native_open(&disk,&e);if(r){assert(!d.writes&&!d.flushes);native_close(e);puts("PASS torn image refused before replay writes");goto done;}assert(!native_recover(e));events=d.events;')
def prepare(image,out,roles,kernel,bootstrap,*unused):return trust.build(image,out,roles,kernel,bootstrap)
def main():
    assert len(sys.argv)==2,'measured private bootstrap tar required'
    running.BUILD_BOOTSTRAP=pathlib.Path(sys.argv[1]).resolve();assert running.BUILD_BOOTSTRAP.is_file()
    gate.overlay=overlay;gate.prepare=prepare
    source=inspect.getsource(gate.main).replace("('reserved',1,False,False),('pending-staged',2,False,False),('pending-published',2,True,False),('pending-state',2,True,True),('retiring',3,True,True)","('pending-staged',2,False,False),")
    source=source.replace("pure=['tests/pkg_pending_candidate.c'","pure=['tests/pkg_trust_floor.c','tests/pkg_pending_candidate.c'")
    source=source.replace("work=out/'workspace'","work=pathlib.Path(tempfile.mkdtemp(prefix='fortress-trust-fs-work-'))/'workspace'")
    before=set((gate.ROOT/'.codex-remote-attachments').glob('pkg-pending-20*'))
    exec(compile(source,'<trust actual filesystem gate>','exec'),vars(gate));gate.main()
    out=(set((gate.ROOT/'.codex-remote-attachments').glob('pkg-pending-20*'))-before).pop();directory=out/'pending-staged';native=out/'pending-native'
    m={'runtime':'INCOMPLETE','boot_acceptance':'NOT RUN','cuts':[],'commands':[]}
    def save():(out/'trust-cuts.json').write_text(json.dumps(m,indent=2)+'\n')
    try:
        with tempfile.TemporaryDirectory(prefix='fortress-trust-cuts-') as td:
            def argv(output,cut):return [str(native),*[str(directory/p) for p in ('disk.img','capsule','selector','witness')],str(output),str(cut),'1']
            cmd=argv(pathlib.Path(td)/'trace.img',-1);m['commands'].append(cmd);r=subprocess.run(cmd,capture_output=True,text=True,env=dict(os.environ,TF_TRACE='1'));(out/'trust-trace.log').write_text(r.stdout+r.stderr);assert not r.returncode
            trace=r.stdout.split('PASS native recovery')[0];chosen=set()
            for name in ('PRE','END'):
                start=int(re.search('TF '+name+' BEGIN (\\d+)',trace)[1]);end=int(re.search('TF '+name+' END (\\d+)',trace)[1]);chosen.update(range(start,end));m[name.lower()+'_events']=[start,end]
            total=int(re.search('events=(\\d+)',r.stdout.split('PASS native recovery')[1])[1])
            for index in re.findall(r'replay event=(\d+) op=flush',trace):chosen.update(x for x in (int(index)-1,int(index),int(index)+1) if 0<=x<total)
            def test(cut,model):
                output=pathlib.Path(td)/f'{model}-{cut}.img';cmd=argv(output,cut);env=dict(os.environ)
                if model=='after':env['TF_AFTER']='1'
                if model=='tear':env['TF_TEAR']='1'
                p=subprocess.run(cmd,capture_output=True,text=True,env=env);output.unlink(missing_ok=True);return {'cut':cut,'model':model,'command':cmd,'returncode':p.returncode,'output':p.stdout+p.stderr}
            with ThreadPoolExecutor(max_workers=4) as pool:
                for future in as_completed([pool.submit(test,c,model) for c in sorted(chosen) for model in ('before','after','tear')]):
                    value=future.result();m['cuts'].append(value);save();assert not value['returncode'],value
            m['runtime']='PASS'
    except BaseException as e:m['runtime']='FAIL';m['error']=repr(e);raise
    finally:save();print(out,'trust cuts',m['runtime'],len(m['cuts']),flush=True)
if __name__=='__main__':main()
