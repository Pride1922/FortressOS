"""Private copied-TCB retained binding and cancellation with actual registry."""
import datetime
import hashlib
import json
import pathlib
import shutil
import subprocess
from pkg_call_overlay import tcb_overlay
from test_pkg_service_integration import function

ROOT=pathlib.Path(__file__).resolve().parent.parent


def main():
    out=ROOT/'.codex-remote-attachments'/('pkg-call-binding-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'))
    work=out/'workspace';work.mkdir(parents=True);sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest();sources={}
    for directory in ('src','tests','user'):
        for p in (ROOT/directory).rglob('*'):
            if p.is_file() and p.suffix in ('.c','.h','.inc'):
                name=str(p.relative_to(ROOT));sources[name]=sha(p);dst=work/name
                dst.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(p,dst);assert sha(dst)==sources[name]
    overlays=[]
    def replace(name,before,after):
        p=work/name;s=p.read_text();assert s.count(before)==1,(name,before)
        p.write_text(s.replace(before,after));overlays.append(dict(file=name,before=before,after=after))
    tcb_overlay(work,replace)
    name='src/kernel/process_table.c';s=(work/name).read_text()
    for signature in ('bool process_record_exit_accounted_times(', 'void process_record_abort(', 'void process_record_forget('):
        old=function((work/name).read_text(),signature);needle='    spin_unlock_irqrestore(&g_process_lock, irq);'
        assert old.count(needle)==1;replace(name,old,old.replace(needle,needle+'\n    pc_cancel_pid(pid);'))
    replace(name,'#include "process_table.h"','#include "process_table.h"\nvoid pc_cancel_pid(uint64_t);')
    m={'result':'INCOMPLETE','sources':sources,'overlays':overlays,'commands':[],
       'scope':'copied TCB field and unlocked registry hooks; actual registry, host current-task delivery; no scheduler/production/boot acceptance'}
    def run(cmd,label):
        m['commands'].append(cmd);r=subprocess.run(cmd,cwd=work,text=True,capture_output=True,timeout=180)
        (out/(label+'.log')).write_text(r.stdout+r.stderr);assert not r.returncode,(label,r.returncode,r.stdout[-1200:],r.stderr[-2400:]);return r.stdout
    flags=['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-no-pie','-pthread','-D_XOPEN_SOURCE=700','-DTEST_SMP_MEMORY',
           '-Itests/ext4_host','-Itests','-Isrc/include','-Isrc/kernel','-Isrc/fs','-Isrc/arch/x86_64','-Isrc/mm']
    units=['tests/pkg_call_host.c','tests/pkg_writer_candidate.c','src/kernel/process_table.c','src/kernel/creds.c']
    try:
        dep=run(flags+['-MM','-MT','probe',*units],'dependencies')
        names={str((work/p).resolve().relative_to(work)) for p in dep.replace('\\\n',' ').split() if not p.endswith(':')}
        m['compiled_inputs']={p:sha(work/p) for p in sorted(names)}
        for name,sanitizer in (('asan','address,undefined'),('tsan','thread')):
            binary=out/('probe-'+name);run(flags+['-fsanitize='+sanitizer,*units,'-o',str(binary)],'compile-'+name)
            invoke=[str(binary)] if name=='asan' else ['setarch','x86_64','-R',str(binary)]
            text=run(invoke,'runtime-'+name);assert 'PASS retained call rounds=128' in text
        m['rounds_per_sanitizer']=128;m['result']='PASS'
    finally:
        m['changed_compiled_copies']=[p for p,h in m.get('compiled_inputs',{}).items() if sha(work/p)!=h]
        m['changed_compiled_repository_sources']=[p for p in m.get('compiled_inputs',{}) if p in sources and sha(ROOT/p)!=sources[p]]
        if m['changed_compiled_copies'] or m['changed_compiled_repository_sources']:m['result']='FAIL_PRESERVATION'
        (out/'manifest.json').write_text(json.dumps(m,indent=2)+'\n')
    assert m['result']=='PASS';print('Evidence:',out)


if __name__=='__main__':main()
