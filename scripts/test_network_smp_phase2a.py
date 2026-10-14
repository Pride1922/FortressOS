#!/usr/bin/env python3
"""Actual generic transport, inert owner, snapshot-only syscall and launch hooks."""
import argparse
import json
from pathlib import Path
import re
import selectors
import shutil
import subprocess
import tempfile
import time
import test_net_installed as baseline
import test_network_smp_phase0 as phase0
from audit_network_smp_phase0 import audit

ROOT=baseline.fixture.REPO

def critical_source(name):
    return name.startswith(('src/','user/')) or name in {
        'Makefile','linker.ld','limine.conf',
        'tests/net_control_guest_user.c','tests/net_control_guest_observer.c',
        'scripts/test_network_smp_phase2a.py','scripts/test_network_smp_phase0.py',
        'scripts/test_net_installed.py','scripts/create_ext4_guest_workspace.py',
        'scripts/test_root_phase0.py','scripts/test_root_phase2.py','scripts/create_initramfs.py'}


def instrument(workspace):
    phase0.instrument(workspace)
    overrides={}
    def patch(name,anchor,replacement):
        p=workspace/name; before=baseline.fixture.digest(p); text=p.read_text()
        assert text.count(anchor)==1,(name,anchor)
        p.write_text(text.replace(anchor,replacement))
        overrides[name]={'before':before,'after':baseline.fixture.digest(p)}
    patch('src/kernel/syscall.c','    switch (syscall_nr) {',
          '    switch (syscall_nr) {\n        case 0x7ff2: {\n            extern int64_t net_control_fixture_observe(unsigned);\n            result=net_control_fixture_observe((unsigned)frame->rdi);\n            break;\n        }')
    text=(workspace/'src/kernel/syscall.c').read_text()
    start=text.index('    /* Disposable Phase 0 observation after the real Ring 3 entry stub. */')
    end=text.index('    uint64_t syscall_nr = frame->rax;',start)
    patch('src/kernel/syscall.c',text[start:end],
          '    if ((frame->cs & 3)==3 && frame->rax==SYS_NETCTL) {\n        extern void net_control_fixture_entry(void);\n        net_control_fixture_entry();\n    }\n')
    patch('src/net/net_control.c','    control_slot_t *v=&values[t.slot];',
          '    extern void net_control_fixture_step(unsigned);\n    net_control_fixture_step(r.kind);\n    control_slot_t *v=&values[t.slot];')
    patch('src/kernel/thread.c','            *link = t->next;\n            t->wait_channel = NULL;',
          '            extern void net_control_fixture_woke(tcb_t *,const void *);\n            net_control_fixture_woke(t,channel);\n            *link = t->next;\n            t->wait_channel = NULL;')
    with (workspace/'src/net/net_control.c').open('a') as f:
        f.write('''
bool net_control_fixture_clean(void) {
    /* Caller invoked only after acquire-observing every generic slot FREE. */
    for (unsigned i=0;i<HANDOFF_SLOTS;++i) if (values[i].probe) return false;
    return true;
}
''')
    with (workspace/'src/kernel/handoff.c').open('a') as f:
        f.write('''
/* Snapshot-only acquire predicates, never locks under scheduler lock. */
bool handoff_fixture_empty(void *unused) {
    (void)unused;
    for (unsigned i=0;i<HANDOFF_SLOTS;++i) {
        unsigned st=state(&slots[i]);
        if (st!=HANDOFF_FREE && st!=HANDOFF_RETIRED) return false;
    }
    return true;
}
bool handoff_fixture_pending(void *unused) {
    (void)unused;
    for (unsigned i=0;i<HANDOFF_SLOTS;++i) if (state(&slots[i])==HANDOFF_PENDING) return true;
    return false;
}
''')
    shutil.copyfile(workspace/'tests/net_control_guest_observer.c',workspace/'src/net/net_control_fixture.c')
    (workspace/'network-smp2a-overrides.json').write_text(json.dumps(overrides,indent=2))

def boot(out,disk,firmware,cpus,password,restart):
    label=f'{firmware}-smp{cpus}-boot{restart}'
    assert disk.is_file() and disk.resolve().parent==out.resolve()
    argv=['qemu-system-x86_64','-accel','tcg','-M','q35','-m','512M','-smp',str(cpus),
          '-display','none','-monitor','none','-no-reboot','-serial','stdio',
          '-drive',f'file={disk},if=none,id=target,format=raw','-device','nvme,drive=target,serial=NETINSTALLED,bootindex=1',
          '-netdev','user,id=net0','-device','e1000,netdev=net0']
    if firmware=='uefi':
        variables=out/(label+'.vars.fd'); shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
        argv+=['-drive','if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
               '-drive',f'if=pflash,format=raw,unit=1,file={variables}']
    assert audit(argv,out,label)
    (out/(label+'.argv.json')).write_text(json.dumps(argv,indent=2))
    transcript=bytearray(); commands=[]
    err=(out/(label+'.stderr')).open('wb')
    proc=subprocess.Popen(argv,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=err)
    sel=selectors.DefaultSelector(); sel.register(proc.stdout,selectors.EVENT_READ)
    def text(start=0):
        return re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]','',transcript[start:].decode(errors='replace')).replace('\r','')
    def wait(predicate,start=0,timeout=120):
        end=time.monotonic()+timeout
        while time.monotonic()<end:
            for key,_ in sel.select(.1): transcript.extend(key.fileobj.read1(65536))
            (out/(label+'.serial.log')).write_bytes(transcript)
            assert not any(s in text() for s in ('KERNEL PANIC','[FATAL]','[PROCESS FAULT]','HANDOFF CALL FAIL')),text()[-4000:]
            if predicate(text(start)): return text(start)
            assert proc.poll() is None,text()[-4000:]
        raise TimeoutError(text()[-4000:])
    def send(cmd):
        commands.append(cmd if cmd!=password else '<password>')
        proc.stdin.write((cmd+'\n').encode()); proc.stdin.flush()
    prompt=lambda t: re.search(r'(?:^|\n)operator@[^\n]+\$ $',t)
    def command(cmd,expected=None):
        start=len(transcript); send(cmd)
        # UART writes from AP probes and shell Done notifications may interleave.
        # A real shell exit status is the result oracle for the foreground probe.
        if expected is None:
            result=wait(lambda t: cmd+'\n' in t and prompt(t),start)
            check=len(transcript); send('echo $?')
            status=wait(lambda t:'echo $?\n' in t and prompt(t),check)
            assert re.search(r'\n0\n',status),status
            return result
        if cmd.endswith(' &'):
            return wait(lambda t: cmd+'\n' in t and re.search(r'\[\d+\]\s+\d+',t) and prompt(t),start)
        return wait(lambda t: cmd+'\n' in t and expected in t and prompt(t),start)
    caller=' --worker 1' if cpus>1 else ''
    probe=lambda mode: 'net-cpu-probe'+caller+' '+mode
    try:
        wait(lambda t:'FortressOS login: ' in t)
        send('operator'); wait(lambda t:t.endswith('Password: ')); send(password); wait(prompt)
        command('net-cpu-probe a')
        command(probe('a'))
        for cpu in range(cpus): command(f'net-cpu-probe --worker {cpu} r')
        command(probe('p'))
        command(probe('t'))
        # Authorized IFSET pointer/size precedence and owner configuration order.
        start=len(transcript); send('sudo '+probe('c'))
        wait(lambda t:t.endswith('sudo password: '),start); send(password)
        wait(prompt,start)
        command('echo $?','\n0\n')
        command(probe('g'))
        # Actual Ring 3 pending probe, STOP/CONT and KILL abandonment.
        for action in ('STOP','KILL','INT'):
            launched=command(probe('e')+' &','')
            job=int(re.findall(r'\[(\d+)\]\s+\d+',launched)[-1])
            command(probe('m'))
            command(f'kill %{job} {action}','')
            if action=='STOP':
                command('jobs','Stopped')
                # Configuration proceeds while the stopped caller retains its result.
                start=len(transcript); send('sudo '+probe('c'))
                wait(lambda t:t.endswith('sudo password: '),start); send(password)
                wait(prompt,start)
                command('echo $?','\n0\n')
                command(f'kill %{job} CONT','')
            command(probe('g'))
        # Simultaneous actual callers on every AP; queries retain FIFO credits.
        for cpu in range(min(7,cpus)):
            command(f'net-cpu-probe --worker {cpu} r &','')
        command(probe('g'))
        config='address 10.0.2.50/24\ngateway 10.0.2.2\ndns 1.1.1.1\ndns2 8.8.8.8\n'
        if not restart:
            cmd='sudo ifup 10.0.2.50/24 10.0.2.2 1.1.1.1 8.8.8.8'
        else: cmd='sudo ifup'
        start=len(transcript); send(cmd)
        wait(lambda t:t.endswith('sudo password: '),start); send(password)
        wait(lambda t:'gateway 10.0.2.2 applied' in t and prompt(t),start)
        command('cat /etc/network/network.conf',config)
        command('/sbin/ifconfig','inet 10.0.2.50/24')
        command('ping -c 1 10.0.2.2','bytes from 10.0.2.2')
        command(probe('g'))
        start=len(transcript); send('sudo shutdown')
        wait(lambda t:t.endswith('sudo password: '),start); send(password)
        wait(lambda t:'[POWER] Shutdown initiated' in t,start)
        assert proc.wait(timeout=30)==0
    finally:
        if proc.poll() is None: proc.kill(); proc.wait(timeout=20)
        sel.close(); err.close()
        (out/(label+'.serial.log')).write_bytes(transcript)
        (out/(label+'.commands.json')).write_text(json.dumps(commands,indent=2))
    log=text()
    owners=[int(v,16) for v in re.findall(r'\[NET SMP0 OWNER\] cpu=(0x[0-9A-Fa-f]+)',log)]
    assert owners and set(owners)=={0}
    masks=re.findall(r'\[NET SMP2A MASK\] (0x[0-9A-Fa-f]+) kinds=(0x[0-9A-Fa-f]+) steps=(0x[0-9A-Fa-f]+) reclaimed=(0x[0-9A-Fa-f]+) remote=(0x[0-9A-Fa-f]+)',log)
    assert masks,log[-4000:]
    mask,kinds,steps,reclaimed,remote=[int(v,16) for v in masks[-1]]
    assert remote>0 if cpus>1 else remote==0,remote
    assert mask==(1<<cpus)-1 and kinds==30 and steps>0,masks
    # 2 queries, 2*(query+32 sets), 2 probes, 3 lifecycle probes, 3 CLI
    # operations, and each foreground/background reader performs 64 requests.
    assert reclaimed==76+64*cpus+64*min(7,cpus),reclaimed
    root=out/(label+'.root.img'); baseline.extract_root(disk,root)
    baseline.fixture.run(['e2fsck','-fn',str(root)],out/(label+'.fsck.log'))
    print('PASS',label,flush=True)
    for path,expected in [('/etc/network/network.conf',config.encode()),('/etc/resolv.conf',b'nameserver 1.1.1.1\nnameserver 8.8.8.8\n')]:
        target=out/(label+'.'+Path(path).name)
        baseline.fixture.run(['debugfs','-R',f'dump {path} {target}',str(root)],out/(label+'.audit.log'))
        assert target.read_bytes()==expected,path
        metadata=baseline.fixture.run(['debugfs','-R',f'stat {path}',str(root)],out/(label+'.audit.log'))
        assert 'User:     0' in metadata and 'Group:     0' in metadata and '0644' in metadata,metadata
    return {'case':label,'result':'PASS','caller_mask':mask,'kinds':kinds,'owner_steps':steps,'reclaimed':reclaimed,'remote_wakes':remote}

def main():
    parser=argparse.ArgumentParser(); parser.add_argument('--cpus',type=int,nargs='+',default=[1,4,8]); parser.add_argument('--firmware',nargs='+',default=['bios','uefi']); parser.add_argument('--workspace',type=Path); args=parser.parse_args()
    assert set(args.cpus)<={1,4,8} and set(args.firmware)<={'bios','uefi'}
    parent=ROOT/'build/network-smp-phase2a'; parent.mkdir(parents=True,exist_ok=True)
    out=Path(tempfile.mkdtemp(prefix='gate-',dir=parent)); print(out,flush=True)
    manifest={'phase':'2A','result':'FAIL','cases':[],'scope':'control/finite probes only; no UDP/TCP AP or physical claim'}
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2))
    try:
        workspace=args.workspace.resolve() if args.workspace else Path(subprocess.check_output(['python3','scripts/create_ext4_guest_workspace.py'],text=True).strip())
        manifest['workspace']=str(workspace)
        (out/'manifest.json').write_text(json.dumps(manifest,indent=2))
        sources=json.loads((workspace/'workspace-manifest.json').read_text())
        (out/'sources.json').write_text(json.dumps(sources,indent=2))
        if args.workspace:
            assert workspace.is_relative_to(ROOT/'.codex-remote-attachments')
            assert (workspace/'network-smp2a-overrides.json').is_file()
        else: instrument(workspace)
        with (out/'build.log').open('w') as log:
            subprocess.run(['make','-j4','bin/fortress.elf','bin/initramfs.tar'],cwd=workspace,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=600)
        manifest['build_sha256']={n:baseline.fixture.digest(workspace/n) for n in ('bin/fortress.elf','bin/initramfs.tar')}
        # Reuse audited installed-root preparation; replace only its probe input.
        original=baseline.fixture.run
        def run(argv,log,timeout=120):
            argv=[str(workspace/'tests/net_control_guest_user.c') if a=='tests/net_cpu_probe_user.c' else a for a in argv]
            return original(argv,log,timeout)
        baseline.fixture.run=run
        password=baseline.secrets.token_hex(16)
        try: disk=baseline.prepare(workspace,out,password)
        finally: baseline.fixture.run=original
        before=baseline.fixture.digest(disk); manifest['baseline_sha256']=before
        for firmware in args.firmware:
            for cpus in args.cpus:
                target=out/f'{firmware}-smp{cpus}.img'
                baseline.fixture.run(['cp','--sparse=always',str(disk),str(target)],out/'copy.log')
                for restart in (0,1):
                    manifest['cases'].append(boot(out,target,firmware,cpus,password,restart))
                (out/'manifest.json').write_text(json.dumps(manifest,indent=2))
        assert baseline.fixture.digest(disk)==before
        changed=[n for n,sha in sources['sources'].items() if not (ROOT/n).is_file() or baseline.fixture.digest(ROOT/n)!=sha]
        manifest['concurrent_source_changes']=changed
        # Concurrent documentation is preserved, never restored from the snapshot.
        # Saved build/source identities remain authoritative for this campaign.
        critical=[n for n in changed if critical_source(n)]
        manifest['changed_executable_or_fixture_sources']=critical
        assert not critical,critical
        manifest.update(result='PASS',source_preservation='CONCURRENT_NONPRODUCTION_CHANGES' if changed else 'PASS')
    except Exception as error:
        manifest['failure']=repr(error); raise
    finally:
        (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n'); print(out,flush=True)
    subprocess.run(['python3','scripts/audit_network_smp_phase0.py',str(out)],check=True)

if __name__=='__main__': main()
