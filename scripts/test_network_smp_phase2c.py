#!/usr/bin/env python3
"""Disposable installed TCP owner client/stream Ring 3 acceptance."""
import os, argparse, json, re, selectors, shutil, subprocess, tempfile, time, socket, threading, struct, hashlib, http.server
from pathlib import Path
import test_net_installed as baseline
import test_network_smp_phase0 as phase0
from audit_network_smp_phase2b import audit
from net_tcp_wire_audit import parse as audited_packet, records
ROOT=baseline.fixture.REPO

def critical_source(name):
    return name.startswith(('src/','user/','scripts/test_net_','scripts/test_network_smp_',
        'scripts/net_','scripts/network_smp_')) or name in {
        'Makefile','linker.ld','limine.conf','tests/net_tcp_owner_guest_user.c','tests/net_tcp_peer_socket_shim.c',
        'scripts/audit_network_smp_phase2b.py','scripts/create_ext4_guest_workspace.py',
        'scripts/test_root_phase0.py','scripts/test_root_phase2.py','scripts/create_initramfs.py'}

def instrument(workspace):
    phase0.instrument(workspace)
    thread=workspace/'src/kernel/thread.c'; text=thread.read_text()
    text=text.replace('!strcmp(p->name, "/bin/net-cpu-probe") && p->cpu_affinity > 0',
        '(!strcmp(p->name, "/bin/net-cpu-probe") || !strcmp(p->name, "/bin/wget")) && p->cpu_affinity > 0')
    text=text.replace('    SPAWN_ADD(profile, phase[SP_FILE], phase_begin);\n    tcb_t *child',
        '    if (!strcmp(path,"/bin/wget") && g_total_sched_cpus>1) { target_cpu=1; affinity=1; }\n    SPAWN_ADD(profile, phase[SP_FILE], phase_begin);\n    tcb_t *child')
    thread.write_text(text)
    tcp=workspace/'src/net/net_tcp.c'; source=tcp.read_text()
    source=source.replace('static endpoint_t endpoints[NET_SOCKET_MAX];',
        'static unsigned fixture_tcp_live, fixture_tcp_zero; static char fixture_tcp_channel;\nstatic endpoint_t endpoints[NET_SOCKET_MAX];')
    source=source.replace('int64_t net_tcp_create(unsigned slot) {',
        'int64_t net_tcp_create(unsigned slot) {\n    __atomic_store_n(&fixture_tcp_live,1,__ATOMIC_RELEASE);')
    source=source.replace('    cursor=(cursor+1)%NET_SOCKET_MAX;',
        '    cursor=(cursor+1)%NET_SOCKET_MAX;\n    unsigned live=0;\n    for (unsigned i=0;i<NET_SOCKET_MAX;++i) live+=endpoints[i].used || endpoints[i].detach || endpoints[i].cancel;\n    for (unsigned i=0;i<TCP_CB_MAX;++i) live+=pool.used[i];\n    __atomic_store_n(&fixture_tcp_live,live,__ATOMIC_RELEASE);\n    unsigned zero=0;\n    for (unsigned i=0;i<TCP_CB_MAX;++i) if (pool.used[i] && pool.blocks[i].tuple.remote_port==17779 && !pool.blocks[i].snd_wnd && pool.blocks[i].tx_count==TCP_TXBUF_MAX) ++zero;\n    __atomic_store_n(&fixture_tcp_zero,zero,__ATOMIC_RELEASE);\n    if (!live || zero) sched_wake_all(&fixture_tcp_channel);')
    source+='''\nstatic bool net_tcp_fixture_drained(void *unused) {
    (void)unused; return !__atomic_load_n(&fixture_tcp_live,__ATOMIC_ACQUIRE);
}
static bool net_tcp_fixture_zero(void *unused) {
    (void)unused; return __atomic_load_n(&fixture_tcp_zero,__ATOMIC_ACQUIRE)!=0;
}
void net_tcp_fixture_zero_wait(void) { sched_wait_until(&fixture_tcp_channel,net_tcp_fixture_zero,NULL); }
void net_tcp_fixture_wait(void) {
    sched_wait_until(&fixture_tcp_channel,net_tcp_fixture_drained,NULL);
}
'''
    tcp.write_text(source)
    sockets=workspace/'src/net/net_socket.c'; source=sockets.read_text()
    source=source.replace('static socket_t s_slots[NET_SOCKET_MAX];',
        'static bool fixture_socket_idle; static char fixture_socket_channel;\nstatic socket_t s_slots[NET_SOCKET_MAX];')
    for signature in ('static int64_t create(file_t **out, bool stream, bool staged)',
        'int64_t net_socket_activate(file_t *file)','int64_t net_socket_activate_stream(file_t *file)'):
        source=source.replace(signature+' {',signature+' {\n    __atomic_store_n(&fixture_socket_idle,false,__ATOMIC_RELEASE);')
    source=source.replace('    s_cursor=(s_cursor+1)%NET_SOCKET_MAX;',
        '    s_cursor=(s_cursor+1)%NET_SOCKET_MAX;\n    uint64_t fixture_flags=spin_lock_irqsave(&s_lock); bool clean=true;\n    for (unsigned i=0;i<NET_SOCKET_MAX;++i) if (s_slots[i].used || s_slots[i].detaching) clean=false;\n    __atomic_store_n(&fixture_socket_idle,clean,__ATOMIC_RELEASE);\n    spin_unlock_irqrestore(&s_lock,fixture_flags);\n    if (clean) sched_wake_all(&fixture_socket_channel);')
    source+='''\nstatic bool net_socket_fixture_idle(void *unused) {
    (void)unused; return __atomic_load_n(&fixture_socket_idle,__ATOMIC_ACQUIRE);
}
void net_socket_fixture_wait(void) { sched_wait_until(&fixture_socket_channel,net_socket_fixture_idle,NULL); }
'''
    sockets.write_text(source)
    with (workspace/'src/kernel/handoff.c').open('a') as f:
        f.write('''\n#include "apic.h"
#include "serial.h"
static bool net_tcp_fixture_empty(void *deadline) {
    if (apic_timer_get_bsp_ticks()>=*(uint64_t *)deadline) return true;
    for (unsigned i=0;i<HANDOFF_SLOTS;++i) {
        unsigned st=state(&slots[i]);
        if (st!=HANDOFF_FREE && st!=HANDOFF_RETIRED) return false;
    }
    return true;
}
int net_tcp_fixture_pool_wait(void) {
    uint64_t deadline=apic_timer_get_bsp_ticks()+5*apic_timer_get_frequency();
    sched_wait_until(&handoff_admission_channel,net_tcp_fixture_empty,&deadline);
    handoff_stats_t st=handoff_stats();
    if (!st.live) return 0;
    for (unsigned i=0;i<HANDOFF_SLOTS;++i) {
        uint64_t f=spin_lock_irqsave(&lock); request_slot_t s=slots[i]; spin_unlock_irqrestore(&lock,f);
        if (s.state==HANDOFF_FREE || s.state==HANDOFF_RETIRED) continue;
        serial_puts("[NET SMP2C UNRECLAIMED] slot="); serial_print_hex(i);
        serial_puts(" actor="); serial_print_hex(s.request.actor);
        serial_puts(" state="); serial_print_hex(s.state);
        serial_puts(" kind="); serial_print_hex(s.request.kind);
        serial_puts(" caller="); serial_print_hex(s.caller);
        serial_puts(" cleaned="); serial_print_hex(s.cleaned); serial_puts("\\n");
    }
    return -1;
}
''')
    p=workspace/'src/kernel/syscall.c'; s=p.read_text()
    a=s.index('    /* Disposable Phase 0 observation'); b=s.index('    uint64_t syscall_nr = frame->rax;',a)
    s=s[:a]+r'''    if ((frame->cs&3)==3 &&
        (!strcmp(thread_current()->name,"/bin/net-cpu-probe") || !strcmp(thread_current()->name,"/bin/wget")) &&
        ((frame->rax>=SYS_SOCKET && frame->rax<=SYS_CONNECT_UNTIL) || frame->rax==SYS_READ || frame->rax==SYS_WRITE)) {
        extern void net_tcp_fixture_entry(unsigned,bool);
        net_tcp_fixture_entry((unsigned)frame->rax,!strcmp(thread_current()->name,"/bin/wget"));
    }
'''+s[b:]
    s=s.replace('    switch (syscall_nr) {',r'''    switch (syscall_nr) {
        case 0x7ff3: {
            thread_yield();
            if (frame->rdi==6) { extern void net_tcp_fixture_zero_wait(void); net_tcp_fixture_zero_wait(); result=0; break; }
            if (frame->rdi==3) { extern int net_tcp_fixture_arm(unsigned); result=net_tcp_fixture_arm((unsigned)frame->rsi); break; }
            if (frame->rdi==4) { extern int net_tcp_fixture_gate_wait(void); result=net_tcp_fixture_gate_wait(); break; }
            if (frame->rdi==5) { extern void net_tcp_fixture_release(void); net_tcp_fixture_release(); result=0; break; }
            if (frame->rdi==2) { result=cpu_current()->id; break; }
            if (frame->rdi) {
                extern bool net_tcp_fixture_pending(void);
                result=net_tcp_fixture_pending();
                break;
            }
            extern int net_tcp_fixture_pool_wait(void);
            result=net_tcp_fixture_pool_wait();
            if (!result) {
                extern bool net_tcp_fixture_clean(void);
                if (!net_tcp_fixture_clean()) result=-1;
                else {
                    extern void net_tcp_fixture_wait(void);
                    extern void net_socket_fixture_wait(void);
                    net_tcp_fixture_wait(); net_socket_fixture_wait();
                    if (!result) {
                        extern void net_tcp_fixture_dump(void);
                        net_tcp_fixture_dump();
                        serial_puts("[NET SMP2C CLEAN]\n");
                    }
                }
            }
            break;
        }
''')
    p.write_text(s)
    owner=workspace/'src/net/net_udp_owner.c'; source=owner.read_text()
    source=source.replace('    if (r.kind>=NET_OWNER_TCP_CREATE) { tcp_step(t,r); return; }',
        '    extern void net_tcp_fixture_step(void);\n    extern bool net_tcp_fixture_gate(handoff_token_t);\n    if (r.kind>=NET_OWNER_TCP_CREATE) net_tcp_fixture_step();\n    if (r.kind==NET_OWNER_TCP_SEND && !handoff_cancelled(t) && net_tcp_fixture_gate(t)) { handoff_finish(t,(handoff_reply_t){0},true); return; }\n    if (r.kind>=NET_OWNER_TCP_CREATE) { tcp_step(t,r); return; }')
    owner.write_text(source)
    with (workspace/'src/net/net_udp_owner.c').open('a') as f:
        f.write('''
#include "percpu.h"
#include "serial.h"
static uint64_t fixture_masks[52], fixture_wget_masks[52], fixture_owner_mask, fixture_steps;
static uint64_t fixture_gate_seen[HANDOFF_SLOTS];
static unsigned fixture_gate_target, fixture_gate_count;
static bool fixture_gate_hold;
static char fixture_gate_channel;
void net_tcp_fixture_entry(unsigned nr,bool wget) {
    if (nr>=52) __builtin_trap();
    __atomic_fetch_or(wget ? &fixture_wget_masks[nr] : &fixture_masks[nr],1ULL<<cpu_current()->id,__ATOMIC_RELEASE);
}
void net_tcp_fixture_step(void) {
    net_control_assert_owner();
    __atomic_fetch_or(&fixture_owner_mask,1ULL<<cpu_current()->id,__ATOMIC_RELEASE);
    __atomic_add_fetch(&fixture_steps,1,__ATOMIC_RELEASE);
}
void net_tcp_fixture_dump(void) {
    for (unsigned nr=0;nr<52;++nr) {
        uint64_t mask=__atomic_load_n(&fixture_masks[nr],__ATOMIC_ACQUIRE);
        if (mask) { serial_puts("[NET SMP2C MASK] nr="); serial_print_hex(nr); serial_puts(" cpus="); serial_print_hex(mask); serial_puts("\\n"); }
        mask=__atomic_load_n(&fixture_wget_masks[nr],__ATOMIC_ACQUIRE);
        if (mask) { serial_puts("[NET SMP2C WGET MASK] nr="); serial_print_hex(nr); serial_puts(" cpus="); serial_print_hex(mask); serial_puts("\\n"); }
    }
    serial_puts("[NET SMP2C OWNER] cpus="); serial_print_hex(__atomic_load_n(&fixture_owner_mask,__ATOMIC_ACQUIRE));
    serial_puts(" steps="); serial_print_hex(__atomic_load_n(&fixture_steps,__ATOMIC_ACQUIRE)); serial_puts("\\n");
}
int net_tcp_fixture_arm(unsigned n) {
    if (!n || n>4) return -1;
    for (unsigned i=0;i<HANDOFF_SLOTS;++i) fixture_gate_seen[i]=0;
    __atomic_store_n(&fixture_gate_count,0,__ATOMIC_RELEASE);
    __atomic_store_n(&fixture_gate_target,n,__ATOMIC_RELEASE);
    __atomic_store_n(&fixture_gate_hold,true,__ATOMIC_RELEASE); return 0;
}
bool net_tcp_fixture_gate(handoff_token_t t) {
    if (!__atomic_load_n(&fixture_gate_hold,__ATOMIC_ACQUIRE)) return false;
    if (fixture_gate_seen[t.slot]!=t.generation) {
        fixture_gate_seen[t.slot]=t.generation;
        __atomic_add_fetch(&fixture_gate_count,1,__ATOMIC_RELEASE);
        sched_wake_all(&fixture_gate_channel);
    }
    return true;
}
static bool fixture_gate_ready(void *unused) {
    (void)unused; return __atomic_load_n(&fixture_gate_count,__ATOMIC_ACQUIRE)>=__atomic_load_n(&fixture_gate_target,__ATOMIC_ACQUIRE);
}
int net_tcp_fixture_gate_wait(void) {
    sched_wait_until(&fixture_gate_channel,fixture_gate_ready,NULL);
    serial_puts("[NET SMP2C CONCURRENT] held="); serial_print_hex(__atomic_load_n(&fixture_gate_count,__ATOMIC_ACQUIRE)); serial_puts("\\n");
    return fixture_gate_ready(NULL) ? 0 : SYSCALL_EINTR;
}
void net_tcp_fixture_release(void) {
    __atomic_store_n(&fixture_gate_hold,false,__ATOMIC_RELEASE); handoff_notify();
}
bool net_tcp_fixture_pending(void) {
    bool pending=false; uint64_t f=spin_lock_irqsave(&lock);
    for (unsigned i=0;i<HANDOFF_SLOTS;++i)
        if (values[i].tcp_pending && (values[i].kind==NET_OWNER_TCP_RECV || values[i].kind==NET_OWNER_TCP_SEND || values[i].kind==NET_OWNER_TCP_CONNECT) && values[i].phase==UDP_IO) pending=true;
    spin_unlock_irqrestore(&lock,f); return pending;
}
bool net_tcp_fixture_clean(void) {
    for (unsigned i=0;i<HANDOFF_SLOTS;++i)
        if (values[i].file || values[i].pin || values[i].active || values[i].reserved ||
            (values[i].generation && values[i].slab>=0)) return false;
    return !copy_decisions && !slab_waiters && free_slabs==8;
}
''')
    (workspace/'network-smp2c-overrides.json').write_text(json.dumps({str(p.relative_to(workspace)):baseline.fixture.digest(p) for d in ('src','user') for p in (workspace/d).rglob('*') if p.is_file()},indent=2))

def checksum(data):
    if len(data)%2: data+=b'\0'
    value=sum(struct.unpack('!%dH'%(len(data)//2),data))
    while value>>16: value=(value&65535)+(value>>16)
    return (~value)&65535

def audit_capture(path,cpus):
    flows={}; http_flows={}; zero_window=False; unanswered_arp=False
    bases={}; reset_ports=set()
    for _,raw in records(path):
        if raw[12:14]==b'\x08\x06' and raw[20:22]==b'\x00\x01' and raw[38:42]==bytes([10,0,2,123]): unanswered_arp=True
        packet=audited_packet(raw)
        if not packet: continue
        src,dst,seq,data=packet['source'],packet['dest'],packet['seq'],packet['data']
        if src==17779 and packet['window']==0: zero_window=True
        server=17778 if 17778 in (src,dst) else 17780 if 17780 in (src,dst) else 0
        if not server: continue
        port=src if dst==server else dst; direction=0 if dst==server else 1
        key=(server,port,direction)
        if packet['flags']&2:
            base=(seq+1)&0xffffffff
            assert key not in bases or bases[key]==base
            bases[key]=base
        if src==17778 and packet['flags']&4: reset_ports.add(port)
        streams=(flows if server==17778 else http_flows).setdefault(port,({},{})); stream=streams[direction]
        if data:
            assert key in bases
            for i,value in enumerate(data):
                position=(seq+i-bases[key])&0xffffffff
                assert position<65536+8192
                assert position not in stream or stream[position]==value
                stream[position]=value
    sizes=[]; reset_streams=0
    for port,(sent,received) in flows.items():
        a=bytes(sent[k] for k in sorted(sent)); b=bytes(received[k] for k in sorted(received))
        if not a and not b: continue
        if a==b'R':
            assert b==b'reset-before-error' and port in reset_ports; reset_streams+=1; continue
        assert len(a) in (16384,65536) and a==b
        assert a==bytes((i*37+(i>>8))&255 for i in range(len(a)))
        sizes.append(len(a))
    assert sorted(sizes)==sorted([65536,16384]*cpus+[65536,16384]),sizes
    assert unanswered_arp,'CONNECT had no controlled unanswered ARP attempt'
    assert reset_streams==1,reset_streams
    assert zero_window,'controlled peer never advertised zero window'
    assert len(http_flows)==1
    for request,response in http_flows.values():
        request=bytes(request[k] for k in sorted(request)); response=bytes(response[k] for k in sorted(response))
        assert request.startswith(b'GET /body HTTP/1.')
        header,body=response.split(b'\r\n\r\n',1)
        assert b'Content-Length: 65536' in header
        assert body==bytes((i*13+7)&255 for i in range(65536))
    path.with_suffix('.tcp-audit.json').write_text(json.dumps({'result':'PASS','exact_stream_lengths':sizes}))

def boot(out,disk,firmware,cpus,password,restart,nic='e1000',release_zero=lambda:None):
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
    proc=subprocess.Popen(argv,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=err,
        env=dict(os.environ,LD_PRELOAD=str(out/'peer-socket-shim.so')))
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
                if not re.search(r'\[\d+\].*\b(?:Running|Stopped)\b',jobs): break
                assert time.monotonic()<end,jobs
            release_zero()
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
        return wait(lambda t: (len(cmd)>50 or cmd+'\n' in t) and expected in t and prompt(t),start)
    caller=' --worker 1' if cpus>1 else ''
    probe=lambda mode: 'net-cpu-probe'+caller+' '+mode
    try:
        wait(lambda t:'FortressOS login: ' in t)
        send('operator'); wait(lambda t:t.endswith('Password: ')); send(password); wait(prompt)
        command('net-cpu-probe h')
        command(probe('g'))
        for action in ('STOP','KILL','INT'):
            launched=command(probe('w')+' &','')
            job=int(re.findall(r'\[(\d+)\]\s+\d+',launched)[-1])
            command(probe('m'))
            command('/sbin/ifconfig','inet 10.0.2.15/24')
            command('ping -c 1 10.0.2.2','bytes from 10.0.2.2')
            command(f'kill %{job} {action}','')
            if action=='STOP':
                command('jobs','Stopped')
                # Stop past the original 20-second absolute receive deadline.
                until=time.monotonic()+22
                wait(lambda t: time.monotonic()>=until,timeout=25)
                command(f'kill %{job} CONT','')
            command(probe('g'))
        launched=command(probe('c')+' &','')
        command(probe('m'))
        command('/sbin/ifconfig','inet 10.0.2.15/24')
        command('ping -c 1 10.0.2.2','bytes from 10.0.2.2')
        command(probe('t'))
        command(probe('g'))
        # Four calls per CPU retain TIME_WAIT records. Respect the existing
        # 16-entry bound; SMP=8 covers CPUs 0..3 and 4..7 across two boots.
        cpulist=range(restart*4,(restart+1)*4) if cpus==8 else range(cpus)
        command(probe('b'))
        for cpu in cpulist: command(f'net-cpu-probe --worker {cpu} u &','')
        command(probe('j'))
        command('/sbin/ifconfig','inet 10.0.2.15/24')
        command('ping -c 1 10.0.2.2','bytes from 10.0.2.2')
        command(probe('v'))
        command(probe('g'))
        for cpu in cpulist:
            command(f'net-cpu-probe --worker {cpu} t')
            command(probe('g'))
        command(probe('x'))
        command(probe('g'))
        command(probe('d'))
        command(probe('g'))
        launched=command(probe('z')+' &','')
        command(probe('m'))
        command(probe('o'))
        command('/sbin/ifconfig','inet 10.0.2.15/24')
        command('ping -c 1 10.0.2.2','bytes from 10.0.2.2')
        expected=hashlib.sha256(bytes((i*13+7)&255 for i in range(65536))).hexdigest()
        command('wget -s 10.0.2.2 -q -O - http://service.test:17780/body | sha256sum',expected)
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
    entries=re.findall(r'\[NET SMP2C MASK\] nr=(0x[0-9A-Fa-f]+) cpus=(0x[0-9A-Fa-f]+)',log)
    masks={int(nr,16):int(mask,16) for nr,mask in entries}
    for nr in (38,39,43,46,47,48,49,50,51,1,4):
        assert masks.get(nr,0)&sum(1<<cpu for cpu in cpulist)==sum(1<<cpu for cpu in cpulist),(nr,masks)
    assert {int(v,16) for v in re.findall(r'\[NET SMP0 OWNER\] cpu=(0x[0-9A-Fa-f]+)',log)}=={0}
    assert '[NET SMP2C CLEAN]' in log
    assert all(int(status) in (0,130,137) for status in re.findall(r'Done\((\d+)\)',log))
    wget_entries=re.findall(r'\[NET SMP2C WGET MASK\] nr=(0x[0-9A-Fa-f]+) cpus=(0x[0-9A-Fa-f]+)',log)
    wget_masks={int(nr,16):int(mask,16) for nr,mask in wget_entries}
    for nr in (38,43,1,4,48): assert wget_masks.get(nr)==(2 if cpus>1 else 1),(nr,wget_masks)
    held=[int(v,16) for v in re.findall(r'\[NET SMP2C CONCURRENT\] held=(0x[0-9A-Fa-f]+)',log)]
    assert held and held[-1]==len(cpulist),held
    owner_masks=re.findall(r'\[NET SMP2C OWNER\] cpus=(0x[0-9A-Fa-f]+) steps=(0x[0-9A-Fa-f]+)',log)
    assert owner_masks and int(owner_masks[-1][0],16)==1 and int(owner_masks[-1][1],16)>0
    audit_capture(out/(label+'.pcap'),len(cpulist))
    root=out/(label+'.root.img'); baseline.extract_root(disk,root)
    baseline.fixture.run(['e2fsck','-fn',str(root)],out/(label+'.fsck.log'))
    print('PASS',label,flush=True)
    for path,expected in [('/etc/network/network.conf',config.encode()),('/etc/resolv.conf',b'nameserver 1.1.1.1\nnameserver 8.8.8.8\n')]:
        target=out/(label+'.'+Path(path).name)
        baseline.fixture.run(['debugfs','-R',f'dump {path} {target}',str(root)],out/(label+'.audit.log'))
        assert target.read_bytes()==expected,path
        metadata=baseline.fixture.run(['debugfs','-R',f'stat {path}',str(root)],out/(label+'.audit.log'))
        assert 'User:     0' in metadata and 'Group:     0' in metadata and '0644' in metadata,metadata
    return {'case':label,'result':'PASS','syscall_cpu_masks':masks}

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--cpus',type=int,nargs='+',default=[1,4,8])
    parser.add_argument('--firmware',nargs='+',default=['bios','uefi'])
    parser.add_argument('--workspace',type=Path)
    parser.add_argument('--nic',choices=['e1000','e1000e'],default='e1000e')
    args=parser.parse_args()
    parent=ROOT/'build/network-smp-phase2c'; parent.mkdir(exist_ok=True,parents=True)
    out=Path(tempfile.mkdtemp(prefix='gate-',dir=parent)); print(out,flush=True)
    manifest={'result':'FAIL','cases':[],'nic':args.nic}
    try:
        workspace=args.workspace.resolve() if args.workspace else Path(subprocess.check_output(['python3','scripts/create_ext4_guest_workspace.py'],text=True).strip())
        manifest['workspace']=str(workspace)
        shutil.copyfile(workspace/'workspace-manifest.json',out/'sources.json')
        if args.workspace:
            assert workspace.is_relative_to(ROOT/'.codex-remote-attachments') and (workspace/'network-smp2c-overrides.json').is_file()
        else: instrument(workspace)
        with (out/'build.log').open('w') as log:
            subprocess.run(['make','-j4','bin/fortress.elf','bin/initramfs.tar'],cwd=workspace,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=600)
        manifest['build_sha256']={n:baseline.fixture.digest(workspace/n) for n in ('bin/fortress.elf','bin/initramfs.tar')}
        shim=out/'peer-socket-shim.so'
        shim_argv=['gcc','-std=c11','-O2','-fPIC','-shared','-Wall','-Wextra','-Werror',str(workspace/'tests/net_tcp_peer_socket_shim.c'),'-ldl','-o',str(shim)]
        with (out/'peer-shim-build.log').open('w') as log: subprocess.run(shim_argv,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=60)
        manifest['peer_shim']={'build_argv':shim_argv,'sha256':baseline.fixture.digest(shim),'LD_PRELOAD':str(shim)}
        original=baseline.fixture.run
        def run(argv,log,timeout=120):
            return original([str(workspace/'tests/net_tcp_owner_guest_user.c') if a=='tests/net_cpu_probe_user.c' else a for a in argv],log,timeout)
        baseline.fixture.run=run
        password=baseline.secrets.token_hex(16)
        try: disk=baseline.prepare(workspace,out,password)
        finally: baseline.fixture.run=original
        before=baseline.fixture.digest(disk); manifest['baseline_sha256']=before
        dns_log=(out/'dns.stderr').open('wb')
        dns=subprocess.Popen(['sudo','-n','python3',str(ROOT/'scripts/network_smp_dns_peer.py')],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=dns_log)
        assert dns.stdout.readline()==b'READY\n'
        class HTTP(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                data=bytes((i*13+7)&255 for i in range(65536))
                self.send_response(200); self.send_header('Content-Length',str(len(data))); self.end_headers(); self.wfile.write(data)
            def log_message(self,*args): pass
        http_server=http.server.ThreadingHTTPServer(('127.0.0.1',17780),HTTP)
        http_thread=threading.Thread(target=http_server.serve_forever); http_thread.start()
        zero=socket.socket(); zero.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
        zero.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,1024)
        zero.bind(('127.0.0.1',17779)); zero.listen(4); zero.settimeout(.2)
        zero_stop=threading.Event(); zero_workers=[]; zero_events=[]
        def stall(conn,release):
            try: release.wait(60)
            finally:
                conn.setsockopt(socket.SOL_SOCKET,socket.SO_LINGER,struct.pack('ii',1,0)); conn.close()
        def zero_accept():
            while not zero_stop.is_set():
                try: conn,_=zero.accept()
                except socket.timeout: continue
                release=threading.Event(); zero_events.append(release)
                worker=threading.Thread(target=stall,args=(conn,release)); zero_workers.append(worker); worker.start()
        zero_thread=threading.Thread(target=zero_accept); zero_thread.start()
        server=socket.socket(); server.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
        server.bind(('127.0.0.1',17778)); server.listen(16); server.settimeout(.2)
        stop=threading.Event(); records=[]; workers=[]
        def connection(conn):
            try:
                conn.settimeout(45); data=bytearray()
                while True:
                    chunk=conn.recv(8192)
                    if not chunk: break
                    data.extend(chunk)
                    if data==b'R':
                        conn.sendall(b'reset-before-error'); time.sleep(.1)
                        conn.setsockopt(socket.SOL_SOCKET,socket.SO_LINGER,struct.pack('ii',1,0))
                        records.append({'reset':True}); return
                    assert len(data)<=65536
                if not data:
                    conn.setsockopt(socket.SOL_SOCKET,socket.SO_LINGER,struct.pack("ii",1,0))
                    records.append({"empty":True}); return
                assert len(data) in (16384,65536)
                assert data==bytes((i*37+(i>>8))&255 for i in range(len(data)))
                conn.sendall(data); conn.shutdown(socket.SHUT_WR)
                records.append({'length':len(data),'sha256':hashlib.sha256(data).hexdigest()})
            except Exception as error: records.append({'failure':repr(error)})
            finally: conn.close()
        def accept():
            while not stop.is_set():
                try: conn,_=server.accept()
                except socket.timeout: continue
                worker=threading.Thread(target=connection,args=(conn,)); workers.append(worker); worker.start()
        listener=threading.Thread(target=accept); listener.start()
        try:
            for firmware in args.firmware:
                for cpus in args.cpus:
                    target=out/f'{firmware}-smp{cpus}.img'
                    baseline.fixture.run(['cp','--sparse=always',str(disk),str(target)],out/'copy.log')
                    for restart in (0,1):
                        case=boot(out,target,firmware,cpus,password,restart,args.nic,lambda:[release.set() for release in zero_events])
                        manifest['cases'].append(case)
                        (out/'manifest.json').write_text(json.dumps(manifest,indent=2))
        finally:
            stop.set(); listener.join(timeout=2); server.close()
            zero_stop.set()
            for release in zero_events: release.set()
            zero_thread.join(timeout=2); zero.close()
            for worker in zero_workers: worker.join(timeout=2)
            http_server.shutdown(); http_server.server_close(); http_thread.join(timeout=2)
            for worker in workers: worker.join(timeout=50)
            dns.stdin.close()
            try: dns.wait(timeout=5)
            finally:
                if dns.poll() is None: dns.kill(); dns.wait(timeout=5)
                dns_log.close()
            (out/'peer.json').write_text(json.dumps(records,indent=2))
        assert dns.returncode==0,(out/'dns.stderr').read_text()
        assert not any('failure' in r for r in records),records
        assert baseline.fixture.digest(disk)==before
        sources=json.loads((out/'sources.json').read_text())['sources']
        changed=[n for n,sha in sources.items() if baseline.fixture.digest(ROOT/n)!=sha]
        manifest['concurrent_source_changes']=changed
        critical=[n for n in changed if critical_source(n)]
        manifest['changed_executable_or_fixture_sources']=critical
        assert not critical,critical
        manifest['source_preservation']='CONCURRENT_NONPRODUCTION_CHANGES' if changed else 'PASS'
        manifest['result']='PASS'
    except Exception as error:
        manifest['failure']=repr(error); raise
    finally:
        if 'dns' in locals():
            dns.stdin.close()
            try: dns.wait(timeout=5)
            except subprocess.TimeoutExpired: dns.kill(); dns.wait(timeout=5)
            dns_log.close()
        (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
if __name__=='__main__': main()
