#!/usr/bin/env python3
"""Actual UDP owner; disposable installed roots and real Ring 3 AP callers."""
import argparse
import json
from pathlib import Path
import re
import selectors
import shutil
import subprocess
import tempfile
import time
import socket
import threading
import concurrent.futures
from test_net_udp import decode
from test_net_rings import packets
from net_dns_peer import HostDNS
import test_net_installed as baseline
import test_network_smp_phase0 as phase0
from audit_network_smp_phase2b import audit

ROOT=baseline.fixture.REPO

def critical_source(name):
    return name.startswith(('src/','user/','scripts/test_net_','scripts/test_network_smp_')) or name in {
        'Makefile','linker.ld','limine.conf',
        'tests/net_udp_owner_guest_user.c','tests/net_control_guest_observer.c',
        'tests/net_control_guest_user.c','scripts/audit_network_smp_phase2b.py',
        'scripts/network_smp_dns_peer.py','scripts/net_dns_peer.py',
        'scripts/test_network_smp_phase2b.py','scripts/test_network_smp_phase0.py',
        'scripts/test_net_installed.py','scripts/create_ext4_guest_workspace.py',
        'scripts/test_root_phase0.py','scripts/test_root_phase2.py','scripts/create_initramfs.py'}


def instrument(workspace):
    phase0.instrument(workspace)
    overrides={}
    def patch(name,anchor,replacement):
        p=workspace/name; before=baseline.fixture.digest(p); text=p.read_text()
        assert text.count(anchor)==1,(name,anchor)
        p.write_text(text.replace(anchor,replacement))
        entry=overrides.setdefault(name,{'before':before,'patches':[]})
        entry['after']=baseline.fixture.digest(p)
        entry['patches'].append({'before':before,'after':entry['after']})
    patch('src/kernel/syscall.c','    switch (syscall_nr) {',
          '    handoff_token_t fixture_before=thread_current()->handoff.token;\n'
          '    switch (syscall_nr) {\n        case 0x7ff2: {\n            extern int64_t net_control_fixture_observe(unsigned);\n            result=net_control_fixture_observe((unsigned)frame->rdi);\n            break;\n        }')
    patch('src/kernel/syscall.c','    frame->rax = (uint64_t)result;',
          '    extern void net_udp_fixture_return(handoff_token_t,handoff_token_t,bool);\n'
          '    net_udp_fixture_return(fixture_before,thread_current()->handoff.token,\n'
          '        syscall_nr==SYS_NETCTL || (syscall_nr>=SYS_SOCKET && syscall_nr<=SYS_RECVFROM));\n'
          '    frame->rax = (uint64_t)result;')
    text=(workspace/'src/kernel/syscall.c').read_text()
    start=text.index('    /* Disposable Phase 0 observation after the real Ring 3 entry stub. */')
    end=text.index('    uint64_t syscall_nr = frame->rax;',start)
    patch('src/kernel/syscall.c',text[start:end],
          '    if ((frame->cs & 3)==3 && frame->rax==SYS_NETCTL) {\n        extern void net_control_fixture_entry(void);\n        net_control_fixture_entry();\n    }\n')
    patch('src/net/net_control.c','    control_slot_t *v=&values[t.slot];',
          '    extern void net_control_fixture_step(unsigned);\n    net_control_fixture_step(r.kind);\n    control_slot_t *v=&values[t.slot];')
    patch('src/net/net_udp_owner.c','    udp_request_t *v=&values[t.slot];\n    uint64_t now=',
          '    extern void net_control_fixture_step(unsigned);\n    net_control_fixture_step(r.kind);\n    udp_request_t *v=&values[t.slot];\n    uint64_t now=')
    patch('src/net/net_control.c','    int64_t result=0;\n    bool pending=false;',
          '    extern bool net_udp_fixture_hold(void);\n'
          '    if (v->probe && net_udp_fixture_hold() && !handoff_cancelled(t)) {\n'
          '        handoff_finish(t,(handoff_reply_t){0},true); return;\n    }\n'
          '    int64_t result=0;\n    bool pending=false;')
    patch('src/net/net_udp_owner.c','    uint64_t now=apic_timer_get_bsp_ticks(),hz=apic_timer_get_frequency();',
          '    extern bool net_udp_fixture_hold(void);\n'
          '    if (r.kind==NET_OWNER_UDP_RECV && v->active && net_udp_fixture_hold() && !handoff_cancelled(t)) {\n'
          '        handoff_finish(t,(handoff_reply_t){0},true); return;\n    }\n'
          '    uint64_t now=apic_timer_get_bsp_ticks(),hz=apic_timer_get_frequency();')
    text=(workspace/'src/kernel/syscall.c').read_text()
    patch('src/kernel/syscall.c','frame->rax==SYS_NETCTL)',
          '(frame->rax==SYS_NETCTL || frame->rax==SYS_SOCKET || frame->rax==SYS_BIND || frame->rax==SYS_SENDTO || frame->rax==SYS_RECVFROM))')
    patch('src/kernel/syscall.c','extern void net_control_fixture_entry(void);\n        net_control_fixture_entry();',
          'extern void net_control_fixture_entry(unsigned);\n        net_control_fixture_entry((unsigned)frame->rax);')
    patch('src/kernel/thread.c','            *link = t->next;\n            t->wait_channel = NULL;',
          '            extern void net_control_fixture_woke(tcb_t *,const void *);\n            net_control_fixture_woke(t,channel);\n            *link = t->next;\n            t->wait_channel = NULL;')
    with (workspace/'src/net/net_control.c').open('a') as f:
        f.write('''
bool net_control_fixture_clean(void) {
    /* Caller invoked only after acquire-observing every generic slot FREE. */
    for (unsigned i=0;i<HANDOFF_SLOTS;++i) if (values[i].probe) return false;
    extern bool net_udp_fixture_clean(void);
    if (!net_udp_fixture_clean()) return false;
    return true;
}
''')
    with (workspace/'src/net/net_udp_owner.c').open('a') as f:
        f.write('''
bool net_udp_fixture_clean(void) {
    for (unsigned i=0;i<HANDOFF_SLOTS;++i)
        if (values[i].file || values[i].pin || values[i].active || values[i].slab>=0) return false;
    for (unsigned i=0;i<8;++i) if (slab_used[i]) return false;
    return !__atomic_load_n(&copy_decisions,__ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&slab_waiters,__ATOMIC_ACQUIRE) &&
        __atomic_load_n(&free_slabs,__ATOMIC_ACQUIRE)==8;
}
'''.replace(' || values[i].slab>=0',' || (values[i].generation && values[i].slab>=0)'))
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
    patch('src/net/net_socket.c','    file->node->fs_private=s;\n    spin_unlock_irqrestore(&s_lock,irq); return 0;',
          '    file->node->fs_private=s;\n    spin_unlock_irqrestore(&s_lock,irq);\n'
          '    extern void net_udp_fixture_endpoint(bool);\n    net_udp_fixture_endpoint(true); return 0;')
    patch('src/net/net_socket.c','            s->detaching=false; s->used=false;\n            spin_unlock_irqrestore(&s_lock,irq);',
          '            s->detaching=false; s->used=false;\n            spin_unlock_irqrestore(&s_lock,irq);\n'
          '            extern void net_udp_fixture_endpoint(bool);\n    net_udp_fixture_endpoint(false);')
    patch('src/net/net_control_fixture.c','    if (!mode) {',
          '    if (!mode) {\n        extern bool net_udp_fixture_detached(void *);\n'
          '        sched_wait_until(&handoff_admission_channel,net_udp_fixture_detached,NULL);')
    patch('src/net/net_control_fixture.c','void net_control_fixture_entry(void) {',
          'void net_control_fixture_entry(unsigned nr) {\n    extern void net_udp_fixture_enter(void);\n    net_udp_fixture_enter();\n'
          '    extern void net_udp_fixture_cpu(unsigned);\n    net_udp_fixture_cpu(nr);')
    patch('src/net/net_control_fixture.c','    extern bool handoff_fixture_empty(void *);',
          '    extern bool net_udp_fixture_arm(unsigned);\n'
          '    if (net_udp_fixture_arm(mode)) return 0;\n'
          '    extern bool handoff_fixture_empty(void *);')
    patch('src/net/net_control_fixture.c','        serial_puts("[NET SMP2A MASK] ");',
          '        extern void net_udp_fixture_exact(void);\n        net_udp_fixture_exact();\n'
          '        serial_puts("[NET SMP2B MASK] ");')
    with (workspace/'src/net/net_control_fixture.c').open('a') as f:
        f.write('''
#include "syscall_abi.h"
static uint64_t syscall_operations, endpoint_created, endpoint_detached, endpoint_live, in_flight;
static uint64_t udp_entry_cpus[4];
void net_udp_fixture_cpu(unsigned nr) {
    if (nr>=SYS_SOCKET && nr<=SYS_RECVFROM)
        __atomic_fetch_or(&udp_entry_cpus[nr-SYS_SOCKET],1ULL<<cpu_current()->id,__ATOMIC_RELEASE);
}
static bool hold_pending;
bool net_udp_fixture_hold(void) { return __atomic_load_n(&hold_pending,__ATOMIC_ACQUIRE); }
bool net_udp_fixture_arm(unsigned mode) {
    if (mode!=3 && mode!=4) return false;
    __atomic_store_n(&hold_pending,mode==3,__ATOMIC_RELEASE); handoff_notify(); return true;
}
void net_udp_fixture_enter(void) { __atomic_add_fetch(&in_flight,1,__ATOMIC_RELEASE); }
void net_udp_fixture_return(handoff_token_t before,handoff_token_t after,bool network) {
    if (before.generation!=after.generation || before.slot!=after.slot)
        __atomic_add_fetch(&syscall_operations,1,__ATOMIC_RELEASE);
    if (network) { __atomic_sub_fetch(&in_flight,1,__ATOMIC_RELEASE); handoff_notify(); }
}
void net_udp_fixture_endpoint(bool created) {
    if (cpu_current()->id) __builtin_trap();
    if (created) { ++endpoint_created; __atomic_add_fetch(&endpoint_live,1,__ATOMIC_RELEASE); }
    else { ++endpoint_detached; __atomic_sub_fetch(&endpoint_live,1,__ATOMIC_RELEASE); }
    handoff_notify();
}
bool net_udp_fixture_detached(void *unused) {
    (void)unused; return !__atomic_load_n(&endpoint_live,__ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&in_flight,__ATOMIC_ACQUIRE);
}
void net_udp_fixture_exact(void) {
    uint64_t expected=__atomic_load_n(&syscall_operations,__ATOMIC_ACQUIRE);
    if (handoff_stats().reclaimed!=expected || endpoint_created!=endpoint_detached) __builtin_trap();
    serial_puts("[NET SMP2B EXACT] operations="); serial_print_hex(expected);
    serial_puts(" created="); serial_print_hex(endpoint_created);
    serial_puts(" detached="); serial_print_hex(endpoint_detached); serial_puts("\\n");
    serial_puts("[NET SMP2B UDP CPUS]");
    for (unsigned i=0;i<4;++i) { serial_puts(" "); serial_print_hex(__atomic_load_n(&udp_entry_cpus[i],__ATOMIC_ACQUIRE)); }
    serial_puts("\\n");
}
''')
    (workspace/'network-smp2b-overrides.json').write_text(json.dumps(overrides,indent=2))
    inputs={str(p.relative_to(workspace)):baseline.fixture.digest(p)
            for folder in ('src','user','tests') for p in (workspace/folder).rglob('*')
            if p.is_file() and p.suffix in ('.c','.h','.inc','.asm','.ld')}
    inputs['Makefile']=baseline.fixture.digest(workspace/'Makefile')
    (workspace/'network-smp2b-compiled-inputs.json').write_text(json.dumps(inputs,indent=2))

def boot(out,disk,firmware,cpus,password,restart,nic='e1000'):
    label=f'{firmware}-smp{cpus}-boot{restart}'
    assert disk.is_file() and disk.resolve().parent==out.resolve()
    argv=['qemu-system-x86_64','-accel','tcg','-M','q35','-m','512M','-smp',str(cpus),
          '-display','none','-monitor','none','-no-reboot','-serial','stdio',
          '-drive',f'file={disk},if=none,id=target,format=raw','-device','nvme,drive=target,serial=NETINSTALLED,bootindex=1',
          '-netdev','user,id=net0','-device',f'{nic},netdev=net0',
          '-object',f'filter-dump,id=capture,netdev=net0,file={out/(label+".pcap")}']
    if firmware=='uefi':
        variables=out/(label+'.vars.fd'); shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
        argv+=['-drive','if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
               '-drive',f'if=pflash,format=raw,unit=1,file={variables}']
    assert audit(argv,out,label,nic)
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
        if cmd.endswith(' g'):
            # Generic FREE is transient between another actor's syscalls.
            # Establish producer exit before comparing the independent ledgers.
            end=time.monotonic()+120
            while True:
                jobs=command('jobs','')
                assert not re.search(r'Done\([1-9][0-9]*\)',text()),text()[-4000:]
                if not re.search(r'\[\d+\].*\b(?:Running|Stopped)\b',jobs): break
                assert time.monotonic()<end,jobs
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
        for cpu in range(cpus):
            command(f'net-cpu-probe --worker {cpu} u')
            command(probe('g'))
            command(f'net-cpu-probe --worker {cpu} d')
            command(probe('g'))
        command('nslookup -s 10.0.2.2 service.test','Address: 10.0.2.2')
        for action in ('STOP','KILL','INT'):
            command(probe('l'))
            launched=command(probe('w')+' &','')
            job=int(re.findall(r'\[(\d+)\]\s+\d+',launched)[-1])
            command(probe('m'))
            command(f'kill %{job} {action}','')
            command(probe('h'))
            if action=='STOP':
                command('jobs','Stopped')
                command(probe('v'))
                command(f'kill %{job} CONT','')
            command(probe('g'))
        for cpu in range(min(7,cpus)):
            command(f'net-cpu-probe --worker {cpu} v &','')
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
            command(probe('l'))
            launched=command(probe('e')+' &','')
            job=int(re.findall(r'\[(\d+)\]\s+\d+',launched)[-1])
            command(probe('m'))
            command(f'kill %{job} {action}','')
            command(probe('h'))
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
    masks=re.findall(r'\[NET SMP2B MASK\] (0x[0-9A-Fa-f]+) kinds=(0x[0-9A-Fa-f]+) steps=(0x[0-9A-Fa-f]+) reclaimed=(0x[0-9A-Fa-f]+) remote=(0x[0-9A-Fa-f]+)',log)
    assert masks,log[-4000:]
    mask,kinds,steps,reclaimed,remote=[int(v,16) for v in masks[-1]]
    assert remote>0 if cpus>1 else remote==0,remote
    assert mask==(1<<cpus)-1 and kinds==30+(15<<16) and steps>0,masks
    # 2 queries, 2*(query+32 sets), 2 probes, 3 lifecycle probes, 3 CLI
    # operations, and each foreground/background reader performs 64 requests.
    exact=re.findall(r'\[NET SMP2B EXACT\] operations=(0x[0-9A-Fa-f]+) created=(0x[0-9A-Fa-f]+) detached=(0x[0-9A-Fa-f]+)',log)
    expected_operations,created,detached=map(lambda v:int(v,16),exact[-1])
    assert reclaimed==expected_operations and created==detached and created>0,(reclaimed,exact[-1])
    entries=re.findall(r'\[NET SMP2B UDP CPUS\] ((?:0x[0-9A-Fa-f]+ ?){4})',log)
    udp_cpus=[int(v,16) for v in entries[-1].split()]
    assert udp_cpus==[(1<<cpus)-1]*4,udp_cpus
    udp=[]
    for wire in packets(out/(label+'.pcap')):
        if len(wire)>=34 and wire[12:14]==b'\x08\x00' and wire[23]==17:
            src,dst,sport,dport,data=decode(wire)
            if dport==17777 or sport==17777:
                udp.append(dict(source=src.hex(),destination=dst.hex(),sport=sport,dport=dport,data=data.hex()))
    sent=[p for p in udp if p['dport']==17777]; received=[p for p in udp if p['sport']==17777]
    # ABI probe: four lengths, truncation, fault/retry and dup = seven sends.
    assert len(sent)==len(received)==7*cpus+32*(1+min(7,cpus)),(len(sent),len(received))
    from collections import Counter
    assert Counter((p['sport'],p['data']) for p in sent)==Counter((p['dport'],p['data']) for p in received)
    (out/(label+'.udp-audit.json')).write_text(json.dumps(udp,indent=2))
    root=out/(label+'.root.img'); baseline.extract_root(disk,root)
    baseline.fixture.run(['e2fsck','-fn',str(root)],out/(label+'.fsck.log'))
    print('PASS',label,flush=True)
    for path,expected in [('/etc/network/network.conf',config.encode()),('/etc/resolv.conf',b'nameserver 1.1.1.1\nnameserver 8.8.8.8\n')]:
        target=out/(label+'.'+Path(path).name)
        baseline.fixture.run(['debugfs','-R',f'dump {path} {target}',str(root)],out/(label+'.audit.log'))
        assert target.read_bytes()==expected,path
        metadata=baseline.fixture.run(['debugfs','-R',f'stat {path}',str(root)],out/(label+'.audit.log'))
        assert 'User:     0' in metadata and 'Group:     0' in metadata and '0644' in metadata,metadata
    return {'case':label,'result':'PASS','caller_mask':mask,'udp_syscall_cpu_masks':udp_cpus,'kinds':kinds,'owner_steps':steps,'reclaimed':reclaimed,'syscall_operations':expected_operations,'created':created,'detached':detached,'echo_datagrams':len(sent),'remote_wakes':remote}

def main():
    parser=argparse.ArgumentParser(); parser.add_argument('--cpus',type=int,nargs='+',default=[1,4,8]); parser.add_argument('--firmware',nargs='+',default=['bios','uefi']); parser.add_argument('--workspace',type=Path); parser.add_argument('--nic',choices=['e1000','e1000e'],default='e1000'); parser.add_argument('--jobs',type=int,choices=[1,2],default=1); args=parser.parse_args()
    assert set(args.cpus)<={1,4,8} and set(args.firmware)<={'bios','uefi'}
    parent=ROOT/'build/network-smp-phase2b'; parent.mkdir(parents=True,exist_ok=True)
    out=Path(tempfile.mkdtemp(prefix='gate-',dir=parent)); print(out,flush=True)
    manifest={'phase':'2B','result':'FAIL','cases':[],'nic':args.nic,'scope':'UDP owner/real BSP/AP DNS; no TCP AP or physical claim'}
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2))
    try:
        workspace=args.workspace.resolve() if args.workspace else Path(subprocess.check_output(['python3','scripts/create_ext4_guest_workspace.py'],text=True).strip())
        manifest['workspace']=str(workspace)
        (out/'manifest.json').write_text(json.dumps(manifest,indent=2))
        sources=json.loads((workspace/'workspace-manifest.json').read_text())
        (out/'sources.json').write_text(json.dumps(sources,indent=2))
        if args.workspace:
            assert workspace.is_relative_to(ROOT/'.codex-remote-attachments')
            assert (workspace/'network-smp2b-overrides.json').is_file()
        else: instrument(workspace)
        if (workspace/'network-smp2b-compiled-inputs.json').is_file():
            shutil.copyfile(workspace/'network-smp2b-compiled-inputs.json',out/'compiled-inputs.json')
        with (out/'build.log').open('w') as log:
            subprocess.run(['make','-j4','bin/fortress.elf','bin/initramfs.tar'],cwd=workspace,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=600)
        manifest['build_sha256']={n:baseline.fixture.digest(workspace/n) for n in ('bin/fortress.elf','bin/initramfs.tar')}
        # Reuse audited installed-root preparation; replace only its probe input.
        original=baseline.fixture.run
        def run(argv,log,timeout=120):
            argv=[str(workspace/'tests/net_udp_owner_guest_user.c') if a=='tests/net_cpu_probe_user.c' else a for a in argv]
            return original(argv,log,timeout)
        baseline.fixture.run=run
        password=baseline.secrets.token_hex(16)
        try: disk=baseline.prepare(workspace,out,password)
        finally: baseline.fixture.run=original
        before=baseline.fixture.digest(disk); manifest['baseline_sha256']=before
        # Only the loopback port-53 helper needs bind privilege. Never run builds,
        # disk preparation, QEMU or filesystem audits with privilege.
        dns_argv=['sudo','-n','python3',str(ROOT/'scripts/network_smp_dns_peer.py')]
        manifest['dns_peer_argv']=dns_argv
        dns_log=(out/'dns-peer.stderr').open('wb')
        dns=subprocess.Popen(dns_argv,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=dns_log)
        assert dns.stdout.readline()==b'READY\n',(out/'dns-peer.stderr').read_text()
        echo=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
        echo.bind(('127.0.0.1',17777)); echo.settimeout(.1); stop=threading.Event(); echoes=[]
        def peer():
            while not stop.is_set():
                try: data,address=echo.recvfrom(1472)
                except socket.timeout: continue
                echoes.append(dict(address=list(address),data=data.hex())); echo.sendto(data,address)
        worker=threading.Thread(target=peer); worker.start()
        try:
            ledger_lock=threading.Lock()
            failed=threading.Event()
            def configuration(firmware,cpus):
                if failed.is_set(): return
                target=out/f'{firmware}-smp{cpus}.img'
                baseline.fixture.run(['cp','--sparse=always',str(disk),str(target)],out/f'{firmware}-smp{cpus}.copy.log')
                for restart in (0,1):
                    if failed.is_set(): return
                    try: case=boot(out,target,firmware,cpus,password,restart,args.nic)
                    except Exception as error:
                        failed.set()
                        with ledger_lock:
                            manifest.setdefault('failures',[]).append({'case':f'{firmware}-smp{cpus}-boot{restart}','failure':repr(error)})
                            (out/'manifest.json').write_text(json.dumps(manifest,indent=2))
                        print('FAIL',firmware,cpus,restart,repr(error),flush=True)
                        raise
                    with ledger_lock:
                        manifest['cases'].append(case)
                        (out/'manifest.json').write_text(json.dumps(manifest,indent=2))
            with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
                futures=[pool.submit(configuration,f,c) for f in args.firmware for c in args.cpus]
                try:
                    for future in concurrent.futures.as_completed(futures): future.result()
                except Exception:
                    for future in futures: future.cancel()
                    raise
        finally:
            stop.set(); worker.join(timeout=2); echo.close()
            dns.stdin.close()
            try: assert dns.wait(timeout=5)==0
            finally:
                if dns.poll() is None: dns.kill(); dns.wait(timeout=5)
                dns_log.close()
            (out/'echo-peer.json').write_text(json.dumps(echoes,indent=2))
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
    subprocess.run(['python3','scripts/audit_network_smp_phase2b.py',str(out)],check=True)

if __name__=='__main__': main()
