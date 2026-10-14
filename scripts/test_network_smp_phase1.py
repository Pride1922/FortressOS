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

def instrument(workspace):
    phase0.instrument(workspace)  # Includes the proven staged AP launch exception.
    overrides={}
    def patch(name,anchor,replacement):
        path=workspace/name
        before=baseline.fixture.digest(path)
        text=path.read_text()
        assert text.count(anchor)==1,(name,anchor)
        path.write_text(text.replace(anchor,replacement))
        overrides[name]={'before':before,'after':baseline.fixture.digest(path)}
    patch('src/net/net_control.c','void net_control_service(void) {',
          'void net_control_service(void) {\n    return; /* Snapshot inert-owner isolation: no production queue consumer. */')
    patch('src/kernel/syscall.c','            result=net_control_call(kind,&value);',
          '            result=kind==NETCTL_IFGET ? net_get_ifconfig(&value.get) : SYSCALL_EIO;')
    patch('src/kernel/main.c','    net_start(boot_info.cmdline, sizeof(boot_info.cmdline));',
          '    net_start(boot_info.cmdline, sizeof(boot_info.cmdline));\n    extern void handoff_fixture_start(void);\n    handoff_fixture_start();')
    patch('src/kernel/syscall.c','    switch (syscall_nr) {',
          '    switch (syscall_nr) {\n        case 0x7ff0: {\n            extern int64_t handoff_fixture_call(unsigned);\n            result=handoff_fixture_call((unsigned)frame->rdi);\n            break;\n        }')
    patch('src/kernel/thread.c','        uint64_t suspended_irq_depth = cpu_current()->irq_depth;',
          '        extern void handoff_fixture_switch(tcb_t *,const void *);\n        handoff_fixture_switch(old,channel);\n        uint64_t suspended_irq_depth = cpu_current()->irq_depth;')
    patch('src/kernel/thread.c','            *link = t->next;\n            t->wait_channel = NULL;',
          '            extern void handoff_fixture_woke(tcb_t *,const void *);\n            handoff_fixture_woke(t,channel);\n            *link = t->next;\n            t->wait_channel = NULL;')
    shutil.copyfile(workspace/'tests/handoff_guest_owner.c',workspace/'src/kernel/handoff_fixture.c')
    overrides['src/kernel/handoff_fixture.c']={'after':baseline.fixture.digest(workspace/'src/kernel/handoff_fixture.c')}
    (workspace/'network-smp1-overrides.json').write_text(json.dumps(overrides,indent=2))

def boot(out,disk,firmware,cpus,password):
    label=f'{firmware}-smp{cpus}-boot0'
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
    def command(cmd,expected='HANDOFF CALL PASS'):
        start=len(transcript); send(cmd)
        # UART writes from AP probes and shell Done notifications may interleave.
        # A real shell exit status is the result oracle for the foreground probe.
        if expected=='HANDOFF CALL PASS':
            result=wait(lambda t: cmd+'\n' in t and prompt(t),start)
            check=len(transcript); send('echo $?')
            status=wait(lambda t:'echo $?\n' in t and prompt(t),check)
            assert re.search(r'\n0\n',status),status
            return result
        return wait(lambda t: cmd+'\n' in t and expected in t and prompt(t),start)
    caller=' --worker 1' if cpus>1 else ''
    probe=lambda mode: 'net-cpu-probe'+caller+' '+mode
    try:
        wait(lambda t:'FortressOS login: ' in t)
        send('operator'); wait(lambda t:t.endswith('Password: ')); send(password); wait(prompt)
        command('net-cpu-probe a')
        command(probe('a'))
        command(probe('c'))  # Completion before enrollment.
        command(probe('l'))  # Owner parked at pre-sleep barrier.
        command(probe('b'))  # Publication resolves barrier before actual sleep.
        if cpus>1: command(probe('d'))  # Remote wake before outgoing save.
        command(probe('h'))  # Cancellation acknowledgement.
        command(probe('g'))  # Exact drain before saturation.
        command(probe('k'))  # 64 synthetic retained identities, 32 credits.
        command(probe('j'))  # Close inert pending gate.
        launched=command(probe('e')+' &','')
        job=int(re.findall(r'\[(\d+)\]\s+\d+',launched)[-1])
        command(probe('m'))
        command(f'kill %{job} STOP','')
        command('jobs','Stopped')
        command(f'kill %{job} CONT','')
        command(probe('i'))
        command(probe('g'))
        command(probe('j'))
        launched=command(probe('e')+' &','')
        job=int(re.findall(r'\[(\d+)\]\s+\d+',launched)[-1])
        command(probe('m'))
        command(f'kill %{job} KILL','')
        command(probe('g'))
        # Simultaneous real Ring 3 producers held pending on multiple APs.
        command(probe('j'))
        start=len(transcript)
        for i in range(7):
            cpu=0 if cpus==1 else 1+i%(cpus-1)
            command(f'net-cpu-probe --worker {cpu} e &','')
        command(probe('n'))
        command(probe('i'))
        command(probe('g'))
        command(probe('o'))
        command('/sbin/ifconfig','inet 10.0.2.15/24')  # Production guard/pinned BSP regression.
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
    entries=[int(v,16) for v in re.findall(r'\[HANDOFF ENTRY CPU\] (0x[0-9A-Fa-f]+)',log)]
    owners=[int(v,16) for v in re.findall(r'\[HANDOFF OWNER CPU\] (0x[0-9A-Fa-f]+)',log)]
    assert owners and set(owners)=={0}
    assert entries and (any(v>0 for v in entries) if cpus>1 else set(entries)=={0})
    masks=[int(v,16) for v in re.findall(r'\[HANDOFF CALLER MASK\] (0x[0-9A-Fa-f]+)',log)]
    assert masks and masks[-1]==(1<<cpus)-1,masks
    assert '[HANDOFF PUBLISH BEFORE SLEEP]' in log and '[HANDOFF COMPLETE BEFORE ENROLLMENT]' in log
    saturation=[int(v,16) for v in re.findall(r'\[HANDOFF SATURATION LIVE\] (0x[0-9A-Fa-f]+)',log)]
    assert saturation and saturation==[64]
    recovered=[int(v,16) for v in re.findall(r'\[HANDOFF RECOVERED\] (0x[0-9A-Fa-f]+)',log)]
    assert recovered and recovered[-1]==(125 if cpus>1 else 124),recovered
    if cpus>1:
        wakes=[int(v,16) for v in re.findall(r'\[HANDOFF WAKE BEFORE SAVE\] (0x[0-9A-Fa-f]+)',log)]
        assert wakes and max(wakes)>0
    root=out/(label+'.root.img'); baseline.extract_root(disk,root)
    baseline.fixture.run(['e2fsck','-fn',str(root)],out/(label+'.fsck.log'))
    print('PASS',label,flush=True)
    return {'case':label,'result':'PASS','foreground_entry_cpus':sorted(set(entries)),
            'caller_mask':masks[-1], 'caller_cpus':[cpu for cpu in range(cpus) if masks[-1]&(1<<cpu)],
            'reclaimed':recovered[-1], 'owner_cpus':sorted(set(owners))}

def main():
    parser=argparse.ArgumentParser(); parser.add_argument('--cpus',type=int,nargs='+',default=[1,4,8]); parser.add_argument('--firmware',nargs='+',default=['bios','uefi']); parser.add_argument('--workspace',type=Path); args=parser.parse_args()
    assert set(args.cpus)<={1,4,8} and set(args.firmware)<={'bios','uefi'}
    parent=ROOT/'build/network-smp-phase1'; parent.mkdir(parents=True,exist_ok=True)
    out=Path(tempfile.mkdtemp(prefix='gate-',dir=parent)); print(out,flush=True)
    manifest={'phase':1,'result':'FAIL','cases':[],'scope':'generic mechanism and inert owner; no production AP networking'}
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2))
    try:
        workspace=args.workspace.resolve() if args.workspace else Path(subprocess.check_output(['python3','scripts/create_ext4_guest_workspace.py'],text=True).strip())
        manifest['workspace']=str(workspace)
        (out/'manifest.json').write_text(json.dumps(manifest,indent=2))
        sources=json.loads((workspace/'workspace-manifest.json').read_text())
        (out/'sources.json').write_text(json.dumps(sources,indent=2))
        if args.workspace:
            assert workspace.is_relative_to(ROOT/'.codex-remote-attachments')
            assert (workspace/'network-smp1-overrides.json').is_file()
        else: instrument(workspace)
        with (out/'build.log').open('w') as log:
            subprocess.run(['make','-j4','bin/fortress.elf','bin/initramfs.tar'],cwd=workspace,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=600)
        manifest['build_sha256']={n:baseline.fixture.digest(workspace/n) for n in ('bin/fortress.elf','bin/initramfs.tar')}
        # Reuse audited installed-root preparation; replace only its probe input.
        original=baseline.fixture.run
        def run(argv,log,timeout=120):
            argv=[str(workspace/'tests/handoff_guest_user.c') if a=='tests/net_cpu_probe_user.c' else a for a in argv]
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
                manifest['cases'].append(boot(out,target,firmware,cpus,password))
                (out/'manifest.json').write_text(json.dumps(manifest,indent=2))
        assert baseline.fixture.digest(disk)==before
        changed=[n for n,sha in sources['sources'].items() if not (ROOT/n).is_file() or baseline.fixture.digest(ROOT/n)!=sha]
        assert not changed,changed
        manifest.update(result='PASS',source_preservation='PASS')
    except Exception as error:
        manifest['failure']=repr(error); raise
    finally:
        (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n'); print(out,flush=True)
    subprocess.run(['python3','scripts/audit_network_smp_phase0.py',str(out)],check=True)

if __name__=='__main__': main()
