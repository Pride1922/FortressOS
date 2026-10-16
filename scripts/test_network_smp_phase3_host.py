"""Actual staged functions with only privileged IRQ instructions adapted."""
import hashlib, json, os, shlex, subprocess, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def main():
    parent=ROOT/'build/network-smp-phase3'; parent.mkdir(exist_ok=True,parents=True)
    out=Path(tempfile.mkdtemp(prefix='host-',dir=parent)); print(out,flush=True)
    sources=['src/kernel/thread.c','src/kernel/thread.h','tests/staged_spawn_host.c',
        'scripts/test_network_smp_phase3_host.py','tests/s8_terminal_host.c','src/drivers/input.c',
        'src/drivers/input.h','src/drivers/input_buffer.h','src/kernel/process_table.c','src/kernel/creds.c']
    digest=lambda p:hashlib.sha256(Path(p).read_bytes()).hexdigest()
    result={'result':'FAIL','sources':{str(p):digest(ROOT/p) for p in sources},'runs':[],
        'git_head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
        'compiler':subprocess.check_output(['gcc','--version'],text=True).splitlines()[0]}
    def dependencies(argv,label):
        data=subprocess.check_output(argv[:-2]+['-MM'],cwd=ROOT,text=True)
        (out/(label+'.dependencies')).write_text(data)
        for rule in data.replace('\\\n',' ').splitlines():
            for name in shlex.split(rule.split(':',1)[1]):
                path=(ROOT/name).resolve()
                if path.is_relative_to(ROOT): result['sources'][str(path.relative_to(ROOT))]=digest(path)
    try:
        text=(ROOT/'src/kernel/thread.c').read_text()
        start=text.index('static void cancel_staged_children(uint64_t parent) {')
        end=text.index('\ntcb_t *process_spawn(',start)
        actual=text[start:end]
        actual=actual.replace('__asm__ volatile("pushfq; pop %0; cli" : "=r"(irq) :: "memory");','irq=0;')
        actual=actual.replace('__asm__ volatile("push %0; popfq" :: "r"(irq) : "memory");','(void)irq;')
        (out/'staged-functions.inc').write_text(actual)
        for sanitizer in ('address,undefined','thread'):
            exe=out/sanitizer.replace(',','-')
            argv=['gcc','-std=c11','-O1','-g','-pthread','-Wall','-Wextra','-Werror','-DTEST_SMP_MEMORY',
                '-fsanitize='+sanitizer,'-no-pie','-Isrc/include','-iquote','src/kernel','-Isrc/arch/x86_64','-I'+str(out),
                'tests/staged_spawn_host.c','-o',str(exe)]
            run=['setarch','x86_64','-R',str(exe)] if sanitizer=='thread' else [str(exe)]
            entry={'build_argv':argv,'run_argv':run}; result['runs'].append(entry)
            dependencies(argv,exe.name)
            with (out/(exe.name+'.log')).open('w') as log:
                subprocess.run(argv,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=60)
                subprocess.run(run,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=120,
                    env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})
            entry.update(result='PASS',sha256=digest(exe))
            terminal=out/('terminal-'+exe.name)
            argv=['gcc','-std=c11','-O1','-g','-pthread','-Wall','-Wextra','-Werror','-DTEST_SMP_MEMORY',
                '-DTERMINAL_CONCURRENT_TEST','-fsanitize='+sanitizer,'-no-pie','-Isrc/include','-iquote','src/kernel',
                '-Isrc/drivers','-Isrc/arch/x86_64','-Isrc/fs','tests/s8_terminal_host.c','src/kernel/creds.c','-o',str(terminal)]
            run=['setarch','x86_64','-R',str(terminal)] if sanitizer=='thread' else [str(terminal)]
            entry={'build_argv':argv,'run_argv':run}; result['runs'].append(entry)
            dependencies(argv,terminal.name)
            with (out/(terminal.name+'.log')).open('w') as log:
                subprocess.run(argv,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=60)
                subprocess.run(run,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=120)
            entry.update(result='PASS',sha256=digest(terminal))
            vfs=out/('vfs-'+exe.name)
            argv=['gcc','-std=c11','-O1','-g','-pthread','-Wall','-Wextra','-Werror',
                '-fsanitize='+sanitizer,'-no-pie','-Itests/ext4_host','-Itests/host','-Isrc/include',
                '-Isrc/fs','-Isrc/drivers','-Isrc/mm','tests/perm_fs_host.c','-o',str(vfs)]
            run=['setarch','x86_64','-R',str(vfs)] if sanitizer=='thread' else [str(vfs)]
            entry={'build_argv':argv,'run_argv':run};result['runs'].append(entry);dependencies(argv,vfs.name)
            with (out/(vfs.name+'.log')).open('w') as log:
                subprocess.run(argv,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=60)
                subprocess.run(run,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=120)
            entry.update(result='PASS',sha256=digest(vfs))
            keyboard=out/('keyboard-'+exe.name)
            argv=['gcc','-std=c11','-O1','-g','-pthread','-Wall','-Wextra','-Werror',
                '-fsanitize='+sanitizer,'-no-pie','-Isrc/include','-Isrc/drivers',
                'tests/keyboard_smp_host.c','src/drivers/keyboard.c','-o',str(keyboard)]
            run=['setarch','x86_64','-R',str(keyboard)] if sanitizer=='thread' else [str(keyboard)]
            entry={'build_argv':argv,'run_argv':run};result['runs'].append(entry);dependencies(argv,keyboard.name)
            with (out/(keyboard.name+'.log')).open('w') as log:
                subprocess.run(argv,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=60)
                subprocess.run(run,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=120)
            entry.update(result='PASS',sha256=digest(keyboard))
        assert all(digest(ROOT/p)==sha for p,sha in result['sources'].items())
        result['result']='PASS'
    except BaseException as error:
        result['failure']=repr(error); raise
    finally: (out/'manifest.json').write_text(json.dumps(result,indent=2)+'\n')
if __name__=='__main__': main()
