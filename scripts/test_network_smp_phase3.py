"""Ordinary unpinned commands, snapshot observation only, disposable roots."""
import argparse, hashlib, http.server, json, os, re, selectors, shutil, socket, socketserver, struct, subprocess, tempfile, threading, time
from pathlib import Path
import test_net_installed as baseline
from audit_network_smp_phase2b import audit
from net_tcp_wire_audit import parse as tcp_packet, records
from test_net_udp import decode as udp_packet
from test_network_smp_phase2d import listener_argv
ROOT=baseline.fixture.REPO
BODY=bytes((i*37+(i>>8))&255 for i in range(65536))
COMMANDS=['/bin/shell','/bin/sudo','/sbin/ifconfig','/sbin/ifup','/bin/ping','/bin/nslookup','/bin/wget','/bin/nc','/bin/cat','/bin/head','/bin/sha256sum','/bin/time','/bin/udptest','/bin/smpbench','/bin/net-cpu-probe']

def wire_audit(path,http_port,link=False):
    flows={}; bases={}; udp=[];zero_window=False;unanswered=False
    for _,raw in records(path):
        if raw[12:14]==b'\x08\x06' and raw[20:22]==b'\x00\x01' and raw[38:42]==bytes([10,0,2,99]): unanswered=True
        if len(raw)>42 and raw[12:14]==b'\x08\x00' and raw[23]==17:
            _,_,src,dst,data=udp_packet(raw)
            if 17777 in (src,dst): udp.append((src==17777,data))
        packet=tcp_packet(raw)
        if not packet: continue
        src,dst=packet['source'],packet['dest']
        if src==17779 and packet['window']==0: zero_window=True
        server=next((p for p in (http_port,17778,9000) if p in (src,dst)),None)
        if server is None: continue
        outbound=src==server; client=dst if outbound else src
        key=(server,client,outbound)
        if packet['flags']&2:
            base=(packet['seq']+1)&0xffffffff
            assert key not in bases or bases[key]==base
            bases[key]=base
        stream=flows.setdefault((server,client),[{},{}])[int(outbound)]
        for i,value in enumerate(packet['data']):
            assert key in bases
            at=(packet['seq']+i-bases[key])&0xffffffff
            assert at<73728 and (at not in stream or stream[at]==value)
            stream[at]=value
    http=echo=listened=shared_echo=shared_read=0
    for (server,_),streams in flows.items():
        data=[]
        for stream in streams:
            assert sorted(stream)==list(range(len(stream)))
            data.append(bytes(stream[i] for i in range(len(stream))))
        if not any(data): continue
        if server==http_port:
            assert data[0].startswith(b'GET /proof HTTP/1.')
            header,body=data[1].split(b'\r\n\r\n',1)
            assert b'Content-Length: 65536' in header and body==BODY
            http+=1
        elif server==17778:
            assert data==[b'ordinary-stream\n',b'ordinary-stream\n']; echo+=1
        else:
            if data[0]==b'ordinary-listener\n':
                assert not data[1];listened+=1
            else:
                assert len(data[0])==256 and data[0]==bytes((i+data[0][0])&255 for i in range(256))
                if data[1]: assert data[1]==data[0];shared_echo+=1
                else: shared_read+=1
    assert (http,echo,listened)==(9 if link else 8,1,1),(http,echo,listened)
    assert zero_window and unanswered,(zero_window,unanswered)
    assert (shared_echo,shared_read)==(3,1),(shared_echo,shared_read)
    expected=[(direction,data) for direction in (False,True) for data in [b'ordinary-udp']*4+[b'inherited-udp-proof']]
    assert sorted(udp)==sorted(expected)
    result={'result':'PASS','exact_http_bodies':http,'sha256':hashlib.sha256(BODY).hexdigest(),'echo_streams':echo,'listener_streams':listened,'udp_datagrams':len(udp)}
    path.with_suffix('.wire-audit.json').write_text(json.dumps(result,indent=2))
    return result

def instrument(workspace):
    path=workspace/'src/kernel/syscall.c'; text=path.read_text()
    anchor='    uint64_t syscall_nr = frame->rax;'
    assert text.count(anchor)==1
    text=text.replace(anchor,'    if ((frame->cs&3)==3) phase3_entry((unsigned)frame->rax);\n'+anchor)
    text=text.replace('    switch (syscall_nr) {','    switch (syscall_nr) {\n        case 0x7ff5: result=phase3_dump(); break;')
    text='static void phase3_entry(unsigned);\nstatic int phase3_dump(void);\n'+text
    text+='\n#include "handoff.h"\n'
    text+='static const char *const phase3_names[]={'+','.join(json.dumps(n) for n in COMMANDS)+'};\n'
    text+='enum { PHASE3_COMMANDS='+str(len(COMMANDS))+' };\n'
    text+=r'''
static uint64_t phase3_masks[PHASE3_COMMANDS][80],phase3_owner_cpus,phase3_owner_steps,phase3_bench_cpus;
static void phase3_entry(unsigned nr) {
    tcb_t *self=thread_current();
    if (!self || nr>=80) return;
    if (!strcmp(self->name,"/bin/smpbench") && self->cpu_affinity>=0) {
        if ((size_t)self->cpu_affinity!=cpu_current()->id || self->cpus_allowed!=CPU_MASK_ONE(cpu_current()->id)) __builtin_trap();
        __atomic_fetch_or(&phase3_bench_cpus,1ULL<<cpu_current()->id,__ATOMIC_RELEASE);return;
    }
    for (unsigned i=0;i<PHASE3_COMMANDS;i++) if (!strcmp(self->name,phase3_names[i])) {
        /* Observation asserts ordinary policy, never changes placement. */
        if (self->cpu_affinity!=-1 || self->cpus_allowed!=CPU_MASK_ALL) __builtin_trap();
        __atomic_fetch_or(&phase3_masks[i][nr],1ULL<<cpu_current()->id,__ATOMIC_RELEASE);
    }
}
void phase3_owner_observe(void) {
    if (cpu_current()->id) __builtin_trap();
    __atomic_fetch_or(&phase3_owner_cpus,1ULL<<cpu_current()->id,__ATOMIC_RELEASE);
    __atomic_add_fetch(&phase3_owner_steps,1,__ATOMIC_RELEASE);
}
static int phase3_dump(void) {
    for (unsigned i=0;i<PHASE3_COMMANDS;i++) for (unsigned nr=0;nr<80;nr++) {
        uint64_t mask=__atomic_load_n(&phase3_masks[i][nr],__ATOMIC_ACQUIRE);
        if (!mask) continue;
        serial_puts("[SMP3 ENTRY] name="); serial_puts(phase3_names[i]);
        serial_puts(" nr="); serial_print_hex(nr); serial_puts(" cpus="); serial_print_hex(mask); serial_puts("\n");
    }
    serial_puts("[SMP3 OWNER] cpus="); serial_print_hex(__atomic_load_n(&phase3_owner_cpus,__ATOMIC_ACQUIRE));
    serial_puts(" steps="); serial_print_hex(__atomic_load_n(&phase3_owner_steps,__ATOMIC_ACQUIRE)); serial_puts("\n");
    serial_puts("[SMP3 BENCH] cpus="); serial_print_hex(__atomic_load_n(&phase3_bench_cpus,__ATOMIC_ACQUIRE)); serial_puts("\n");
    handoff_stats_t st=handoff_stats();
    serial_puts("[SMP3 LIVE] count="); serial_print_hex(st.live); serial_puts("\n");
    extern uint64_t phase3_pending_snapshot(unsigned *);
    unsigned pending_count=0;uint64_t kinds=phase3_pending_snapshot(&pending_count);
    serial_puts("[SMP3 PENDING] kinds="); serial_print_hex(kinds);serial_puts(" count=");serial_print_hex(pending_count);serial_puts("\n");
    extern unsigned phase3_tcp_snapshot(void),phase3_socket_snapshot(void),phase3_timewait_snapshot(void);
    unsigned tcp=phase3_tcp_snapshot(),sockets=phase3_socket_snapshot();
    extern bool phase3_typed_clean(void);bool typed=phase3_typed_clean();
    serial_puts("[SMP3 RESOURCES] tcp=");serial_print_hex(tcp);serial_puts(" sockets=");serial_print_hex(sockets);serial_puts("\n");
    serial_puts("[SMP3 TYPED] clean=");serial_print_hex(typed);serial_puts("\n");
    serial_puts("[SMP3 TIMEWAIT] retained=");serial_print_hex(phase3_timewait_snapshot());serial_puts("\n");
    return st.live || tcp || sockets || !typed ? 1 : 0;
}
'''
    path.write_text(text)
    for name in ('net_control.c','net_udp_owner.c'):
        path=workspace/'src/net'/name; text=path.read_text()
        assert '    net_control_assert_owner();' in text
        text='void phase3_owner_observe(void);\n'+text.replace('    net_control_assert_owner();','    net_control_assert_owner();\n    phase3_owner_observe();')
        path.write_text(text)
    path=workspace/'src/net/net_udp_owner.c'
    with path.open('a') as f:
        f.write(r'''
uint64_t phase3_pending_snapshot(unsigned *count) {
    uint64_t mask=0,flags=spin_lock_irqsave(&lock);
    for (unsigned i=0;i<HANDOFF_SLOTS;i++)
        if (values[i].tcp_pending && values[i].phase==UDP_IO) { mask|=1ULL<<values[i].kind; ++*count; }
    spin_unlock_irqrestore(&lock,flags);return mask;
}
bool phase3_typed_clean(void) {
    uint64_t flags=spin_lock_irqsave(&lock);bool clean=true;
    for (unsigned i=0;i<HANDOFF_SLOTS;i++) {
        udp_request_t *v=&values[i];
        if (v->file || v->accepted || v->pin || v->active || v->reserved ||
                (v->generation && v->slab>=0)) clean=false;
    }
    clean=clean && !copy_decisions && !slab_waiters && free_slabs==8;
    spin_unlock_irqrestore(&lock,flags);return clean;
}
''')
    path=workspace/'src/net/net_tcp.c';text=path.read_text()
    anchor='    cursor=(cursor+1)%NET_SOCKET_MAX;'
    assert text.count(anchor)==1
    text='static unsigned phase3_tcp_live,phase3_tw_live;\n'+text.replace(anchor,anchor+r'''
    unsigned observed=0;
    for (unsigned i=0;i<NET_SOCKET_MAX;i++) observed+=endpoints[i].used || endpoints[i].detach || endpoints[i].cancel;
    for (unsigned i=0;i<TCP_CB_MAX;i++) observed+=pool.used[i];
    __atomic_store_n(&phase3_tcp_live,observed,__ATOMIC_RELEASE);
    unsigned tw=0;for (unsigned i=0;i<TCP_TIMEWAIT_MAX;i++) tw+=pool.timewait[i].used;
    __atomic_store_n(&phase3_tw_live,tw,__ATOMIC_RELEASE);
''')
    text=text.replace('int64_t net_tcp_create(unsigned slot) {','int64_t net_tcp_create(unsigned slot) {\n    __atomic_store_n(&phase3_tcp_live,1,__ATOMIC_RELEASE);')
    text+='\nunsigned phase3_tcp_snapshot(void) { return __atomic_load_n(&phase3_tcp_live,__ATOMIC_ACQUIRE); }\n'
    text+='unsigned phase3_timewait_snapshot(void) { return __atomic_load_n(&phase3_tw_live,__ATOMIC_ACQUIRE); }\n'
    path.write_text(text)
    path=workspace/'src/net/net_socket.c'
    with path.open('a') as f:
        f.write(r'''
unsigned phase3_socket_snapshot(void) {
    uint64_t flags=spin_lock_irqsave(&s_lock);unsigned live=0;
    for (unsigned i=0;i<NET_SOCKET_MAX;i++) live+=s_slots[i].used || s_slots[i].detaching;
    spin_unlock_irqrestore(&s_lock,flags);return live;
}
''')
    (workspace/'network-smp3-overrides.json').write_text(json.dumps({str(p.relative_to(workspace)):baseline.fixture.digest(p) for p in workspace.rglob('*') if p.is_file() and p.parts[-1] in ('syscall.c','net_control.c','net_udp_owner.c','net_tcp.c','net_socket.c')},indent=2))

def boot(out,disk,firmware,cpus,restart,password,nic,port,qmp_directory=None,release_zero=lambda:None):
    label=f'{firmware}-smp{cpus}-boot{restart}'
    assert disk.is_file() and disk.resolve().parent==out.resolve()
    argv=['qemu-system-x86_64','-accel','tcg','-M','q35','-m','512M','-smp',str(cpus),'-display','none','-monitor','none','-no-reboot','-serial','stdio',
        '-drive',f'file={disk},if=none,id=target,format=raw','-device','nvme,drive=target,serial=NETINSTALLED,bootindex=1',
        '-netdev','user,id=net0,hostfwd=tcp:127.0.0.1:19000-10.0.2.50:9000','-device',f'{nic},netdev=net0',
        '-object',f'filter-dump,id=capture,netdev=net0,file={out/(label+".pcap")}']
    if nic=='none': argv=argv[:argv.index('-netdev')]+['-net','none']
    if firmware=='uefi':
        variables=out/(label+'.vars.fd'); shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
        argv+=['-drive','if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd','-drive',f'if=pflash,format=raw,unit=1,file={variables}']
    if qmp_directory:
        assert nic!='none'
        argv[argv.index(f'{nic},netdev=net0')]=f'{nic},netdev=net0,id=nic0'
        argv+=['-qmp',f'unix:{qmp_directory/(label+".sock")},server=on,wait=off']
    normalized=['user,id=net0' if v.startswith('user,id=net0,hostfwd=') else v for v in argv]
    if qmp_directory:
        normalized=normalized[:-2]
        normalized[normalized.index(f'{nic},netdev=net0,id=nic0')]=f'{nic},netdev=net0'
    check=lambda candidate:listener_argv(candidate,out,label,nic) if nic=='none' else audit(candidate,out,label,nic)
    assert check(normalized)
    for extra in (['-drive','file=/dev/sda'],['-blockdev','driver=host_device,filename=/dev/sda'],['-cdrom','shared.iso'],['-device','usb-storage,drive=shared']):
        try: check(normalized+extra)
        except AssertionError: pass
        else: raise AssertionError('storage exclusion failed')
    (out/(label+'.argv.json')).write_text(json.dumps(argv,indent=2))
    transcript=bytearray(); commands=[]; peers=[]
    err=(out/(label+'.stderr')).open('wb')
    proc=subprocess.Popen(argv,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=err,
        env={**os.environ,'LD_PRELOAD':str(out/'peer-socket-shim.so')})
    sel=selectors.DefaultSelector(); sel.register(proc.stdout,selectors.EVENT_READ)
    def text(start=0): return re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]','',transcript[start:].decode(errors='replace')).replace('\r','')
    def wait(predicate,start=0,timeout=120):
        end=time.monotonic()+timeout
        while time.monotonic()<end:
            for key,_ in sel.select(.1): transcript.extend(key.fileobj.read1(65536))
            (out/(label+'.serial.log')).write_bytes(transcript)
            assert not any(s in text() for s in ('KERNEL PANIC','[FATAL]','[PROCESS FAULT]')),text()[-5000:]
            if predicate(text(start)): return text(start)
            assert proc.poll() is None,text()[-5000:]
        raise TimeoutError(text()[-5000:])
    prompt=lambda t:re.search(r'(?:^|\n)operator@[^\n]+\$ $',t)
    def send(cmd):
        commands.append('<password>' if cmd==password else cmd)
        proc.stdin.write((cmd+'\n').encode()); proc.stdin.flush()
    def command(cmd,expected=None,sudo=False):
        start=len(transcript); send(cmd)
        if sudo:
            wait(lambda t:t.endswith('sudo password: '),start); send(password)
        # The terminal scrolls long input; its final suffix remains visible.
        # Requiring that suffix plus newline prevents an idle redraw or an
        # unrelated Done prompt from acknowledging a command still being typed.
        suffix=cmd[-25:]+'\n'
        result=wait(lambda t:suffix in t and prompt(t) and (expected is None or expected in t)
            and (not cmd.endswith('&') or re.search(r'\[\d+\]\s+\d+',t)),start)
        return result
    def status(cmd,expected='0'):
        command(cmd); result=command('echo $?')
        assert re.search(r'(?:^|\n)'+re.escape(expected)+r'\n',result),result
    def drain():
        end=time.monotonic()+90
        while re.search(r'\b(?:Running|Stopped)\b',command('jobs')):
            assert time.monotonic()<end
        while True:
            snapshot=command('net-cpu-probe')
            live=re.findall(r'\[SMP3 LIVE\] count=(0x[\da-fA-F]+)',snapshot)
            resources=re.findall(r'\[SMP3 RESOURCES\] tcp=(0x[\da-fA-F]+) sockets=(0x[\da-fA-F]+)',snapshot)
            typed=re.findall(r'\[SMP3 TYPED\] clean=(0x[\da-fA-F]+)',snapshot)
            if live and int(live[-1],16)==0 and resources and all(int(n,16)==0 for n in resources[-1]) and typed and int(typed[-1],16)==1: break
            assert time.monotonic()<end,(live,resources)
            command('sleep 0.1')
    def link_set(up):
        endpoint=qmp_directory/(label+'.sock')
        assert endpoint.parent==qmp_directory and len(str(endpoint).encode())<104
        exchanges=[]
        with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as peer:
            peer.settimeout(5);peer.connect(str(endpoint))
            with peer.makefile('rwb') as stream:
                exchanges.append(json.loads(stream.readline()))
                for identity,request in enumerate(({'execute':'qmp_capabilities'},{'execute':'set_link','arguments':{'name':'nic0','up':up}}),1):
                    request['id']=identity;exchanges.append(request);stream.write((json.dumps(request)+'\n').encode());stream.flush()
                    while True:
                        reply=json.loads(stream.readline());exchanges.append(reply)
                        if reply.get('id')==identity:
                            assert 'return' in reply,reply
                            break
        path=out/(label+'.qmp.json');saved=json.loads(path.read_text()) if path.is_file() else []
        saved.append({'up':up,'exchanges':exchanges});path.write_text(json.dumps(saved,indent=2))
    config='address 10.0.2.50/24\ngateway 10.0.2.2\ndns 1.1.1.1\ndns2 8.8.8.8\n'
    try:
        wait(lambda t:'FortressOS login: ' in t)
        assert '[ROOT] Installed root admitted by PARTUUID' in text()
        send('operator'); wait(lambda t:t.endswith('Password: ')); send(password); wait(prompt)
        if nic=='none':
            for cmd in ('/sbin/ifconfig','ping -c 1 10.0.2.2','udptest 10.0.2.2 17777 absent',
                    'wget -q -O /home/operator/absent http://10.0.2.2:17780/absent',
                    'nslookup -s 10.0.2.2 service.test'):
                status(cmd,'1')
            command('sudo ifup 10.0.2.50/24 10.0.2.2',sudo=True)
            result=command('echo $?');assert re.search(r'(?:^|\n)1\n',result),result
            status(f'smpbench -c -n {cpus} -r 1 -i 100 -w cpu_scale')
            snapshot=command('net-cpu-probe')
            entries=re.findall(r'\[SMP3 ENTRY\] name=(\S+) nr=(0x[\da-fA-F]+) cpus=(0x[\da-fA-F]+)',snapshot)
            observed={}
            for name,nr,mask in entries: observed.setdefault(name,{})[int(nr,16)]=int(mask,16)
            if cpus>1: assert any(int(mask,16)&~1 for _,_,mask in entries)
            owner=re.findall(r'\[SMP3 OWNER\] cpus=(0x[\da-fA-F]+) steps=(0x[\da-fA-F]+)',snapshot)
            assert owner and tuple(int(n,16) for n in owner[-1])==(0,0)
            start=len(transcript);send('sudo shutdown');wait(lambda t:t.endswith('sudo password: '),start);send(password)
            wait(lambda t:'[POWER] Shutdown initiated' in t,start);assert proc.wait(timeout=30)==0
            root=out/(label+'.root.img');baseline.extract_root(disk,root)
            baseline.fixture.run(['e2fsck','-fn',str(root)],out/(label+'.fsck.log'))
            print('PASS',label,'absent NIC',flush=True)
            return {'case':label,'result':'PASS','absent_nic':True,'entry_masks':observed,'owner':owner[-1]}
        command('/sbin/ifconfig','inet 10.0.2.15/24')
        if not restart:
            command('/sbin/ifup 10.0.2.50/24 10.0.2.2','permission denied')
            command('sudo ifup 10.0.2.50/24 10.0.2.2 1.1.1.1 8.8.8.8','applied',True)
        else: command('sudo ifup','applied',True)
        command('cat /etc/network/network.conf',config)
        waiting=[]
        for n in range(6):
            launch=command(f'nc -l {9100+n} > /tmp/wait{n} &')
            waiting.append(int(re.findall(r'\[(\d+)\]\s+\d+',launch)[-1]))
        deadline=time.monotonic()+30
        while True:
            snapshot=command('net-cpu-probe')
            if any(int(n,16)>=6 for n in re.findall(r'\[SMP3 PENDING\] kinds=0x[\da-fA-F]+ count=(0x[\da-fA-F]+)',snapshot)): break
            assert time.monotonic()<deadline,snapshot
        status('/sbin/ifconfig'); command('ping -c 1 10.0.2.2','bytes from 10.0.2.2')
        command('nslookup -s 10.0.2.2 service.test','10.0.2.2')
        for job in waiting: command(f'kill %{job} KILL')
        drain()
        status('net-cpu-probe udp-parent');drain()
        for mode,count in (('shared-listener',3),('shared-stream',1)):
            start=len(transcript);cmd='net-cpu-probe '+mode;send(cmd)
            wait(lambda t:'PRIVATE LISTENER READY' in t,start)
            results=[];threads=[];peer_lock=threading.Lock()
            attempt_path=out/(label+'.peer-attempts.json')
            attempts=json.loads(attempt_path.read_text()) if attempt_path.is_file() else []
            attempts.append({'mode':mode,'expected':count,'results':results})
            attempt_path.write_text(json.dumps(attempts,indent=2))
            def peer_exchange(tag):
                record={'mode':mode,'tag':tag}
                try:
                    data=bytes((i+tag)&255 for i in range(256))
                    with socket.create_connection(('127.0.0.1',19000),timeout=20) as peer:
                        peer.settimeout(30);peer.sendall(data);peer.shutdown(socket.SHUT_WR);received=bytearray()
                        while chunk:=peer.recv(4096):received.extend(chunk)
                        assert bytes(received)==(data if mode=='shared-listener' else b'')
                        record.update(result='PASS',sha256=hashlib.sha256(data).hexdigest())
                except Exception as error:record.update(result='FAIL',failure=repr(error))
                finally:
                    with peer_lock:
                        results.append(record);attempt_path.write_text(json.dumps(attempts,indent=2))
            for tag in range(1,count+1):
                thread=threading.Thread(target=peer_exchange,args=(tag,));threads.append(thread);thread.start()
            wait(lambda t:prompt(t) and 'PRIVATE LISTENER READY' in t,start)
            for thread in threads:thread.join(timeout=35);assert not thread.is_alive()
            peers.extend(results);(out/(label+'.peers.json')).write_text(json.dumps(peers,indent=2))
            assert len(results)==count and all(r['result']=='PASS' for r in results),results
            result=command('echo $?');assert re.search(r'(?:^|\n)0\n',result),result
            drain()
        for iteration in range(4):
            command('/sbin/ifconfig | head -n 2','inet 10.0.2.50/24')
            command('ping -c 1 10.0.2.2','bytes from 10.0.2.2')
            command('nslookup -s 10.0.2.2 tcp.test','10.0.2.2')
            command('udptest 10.0.2.2 17777 ordinary-udp','UDP echo PASS')
            command(f'wget -q -O /home/operator/proof http://10.0.2.2:{port}/proof')
            command('sha256sum /home/operator/proof',hashlib.sha256(BODY).hexdigest())
        # Concurrent production commands retain normal arguments and spawn paths.
        for n in range(4): command(f'wget -q -O /home/operator/proof{n} http://10.0.2.2:{port}/proof &')
        status('/sbin/ifconfig'); drain()
        for n in range(4): command(f'sha256sum /home/operator/proof{n}',hashlib.sha256(BODY).hexdigest())
        command('echo ordinary-stream | nc 10.0.2.2 17778 > /tmp/echo')
        command('cat /tmp/echo','ordinary-stream')
        # Real production listener and stream, no placement parameters.
        launch=command('nc -l 9000 > /tmp/listened &')
        time.sleep(.3)
        with socket.create_connection(('127.0.0.1',19000),timeout=20) as peer:
            payload=b'ordinary-listener\n'; peer.sendall(payload); peer.shutdown(socket.SHUT_WR)
            while peer.recv(4096): pass
            peers.append({'sha256':hashlib.sha256(payload).hexdigest()})
        drain(); command('sha256sum /tmp/listened',hashlib.sha256(payload).hexdigest())
        for action in ('STOP','KILL','INT'):
            launch=command('nc -l 9000 > /tmp/empty &')
            job=int(re.findall(r'\[(\d+)\]\s+\d+',launch)[-1])
            command(f'kill %{job} {action}')
            if action=='STOP':
                command('jobs','Stopped'); command('/sbin/ifconfig','inet 10.0.2.50/24')
                command(f'kill %{job} CONT'); command(f'kill %{job} KILL')
            drain()
        # Ordinary blocked callers: no private launch, placement, or protocol hook.
        launch=command('nc 10.0.2.99 80 < /dev/null > /tmp/unanswered &')
        job=int(re.findall(r'\[(\d+)\]\s+\d+',launch)[-1])
        deadline=time.monotonic()+30
        while True:
            snapshot=command('net-cpu-probe')
            pending=re.findall(r'\[SMP3 PENDING\] kinds=(0x[\da-fA-F]+)',snapshot)
            if pending and int(pending[-1],16)&(1<<22): break
            assert time.monotonic()<deadline,pending
        status('/sbin/ifconfig'); command(f'kill %{job} KILL'); drain()
        launch=command('cat /home/operator/proof /home/operator/proof /home/operator/proof /home/operator/proof | nc 10.0.2.2 17779 > /tmp/zero &')
        job=int(re.findall(r'\[(\d+)\]\s+\d+',launch)[-1])
        deadline=time.monotonic()+60
        while True:
            snapshot=command('net-cpu-probe')
            pending=re.findall(r'\[SMP3 PENDING\] kinds=(0x[\da-fA-F]+)',snapshot)
            if pending and int(pending[-1],16)&(1<<23): break
            assert time.monotonic()<deadline,pending
        command(f'kill %{job} STOP'); command('jobs','Stopped')
        status('/sbin/ifconfig'); command('ping -c 1 10.0.2.2','bytes from 10.0.2.2')
        command('nslookup -s 10.0.2.2 service.test','10.0.2.2')
        command(f'kill %{job} CONT'); command(f'kill %{job} KILL'); release_zero(); drain()
        if qmp_directory:
            command('nc -l 9200 > /tmp/link &');command('net-cpu-probe')
            link_set(False);deadline=time.monotonic()+30
            while 'link DOWN' not in command('/sbin/ifconfig'): assert time.monotonic()<deadline
            drain()
            link_set(True);deadline=time.monotonic()+30
            while 'link UP' not in command('/sbin/ifconfig'): assert time.monotonic()<deadline
            command('ping -c 1 10.0.2.2','bytes from 10.0.2.2')
            command(f'wget -q -O /home/operator/link-proof http://10.0.2.2:{port}/proof')
            command('sha256sum /home/operator/link-proof',hashlib.sha256(BODY).hexdigest());drain()
        command('time -p sleep 0.1','real')
        status(f'smpbench -c -n {cpus} -r 1 -i 100 -w cpu_scale')
        drain()
        start=len(transcript); send('sudo shutdown'); wait(lambda t:t.endswith('sudo password: '),start); send(password)
        wait(lambda t:'[POWER] Shutdown initiated' in t,start); assert proc.wait(timeout=30)==0
    finally:
        if proc.poll() is None: proc.kill(); proc.wait(timeout=20)
        sel.close(); err.close()
        (out/(label+'.termination.json')).write_text(json.dumps({'pid':proc.pid,'returncode':proc.returncode,'reaped':True}))
        (out/(label+'.commands.json')).write_text(json.dumps(commands,indent=2))
        (out/(label+'.serial.log')).write_bytes(transcript)
        if qmp_directory:
            endpoint=qmp_directory/(label+'.sock')
            if endpoint.exists():endpoint.unlink()
    masks={}
    for name,nr,mask in re.findall(r'\[SMP3 ENTRY\] name=(\S+) nr=(0x[\da-fA-F]+) cpus=(0x[\da-fA-F]+)',text()): masks.setdefault(name,{})[int(nr,16)]=int(mask,16)
    for name,nr in (('/sbin/ifconfig',42),('/sbin/ifup',42),('/bin/ping',42),('/bin/nslookup',38),('/bin/wget',38),('/bin/nc',38)):
        assert masks.get(name,{}).get(nr,0),(name,nr,masks)
    if cpus>1:
        # Prove production AP network entry separately from owner execution.
        assert all(masks[name][nr]&~1 for name,nr in (('/sbin/ifconfig',42),('/bin/ping',42),('/bin/wget',38))),masks
        assert all(masks.get('/bin/net-cpu-probe',{}).get(nr,0)&~1 for nr in (45,47)),masks
    owners=re.findall(r'\[SMP3 OWNER\] cpus=(0x[\da-fA-F]+) steps=(0x[\da-fA-F]+)',text())
    assert owners and int(owners[-1][0],16)==1 and int(owners[-1][1],16)>0
    bench=re.findall(r'\[SMP3 BENCH\] cpus=(0x[\da-fA-F]+)',text())
    assert bench and int(bench[-1],16)==(1<<cpus)-1,bench
    live=re.findall(r'\[SMP3 LIVE\] count=(0x[\da-fA-F]+)',text())
    assert live and int(live[-1],16)==0,live
    root=out/(label+'.root.img'); baseline.extract_root(disk,root)
    baseline.fixture.run(['e2fsck','-fn',str(root)],out/(label+'.fsck.log'))
    for path,expected in (('/etc/network/network.conf',config.encode()),('/etc/network/resolv.conf',b'nameserver 1.1.1.1\nnameserver 8.8.8.8\n')):
        target=out/(label+'.'+Path(path).name)
        baseline.fixture.run(['debugfs','-R',f'dump {path} {target}',str(root)],out/(label+'.audit.log'))
        assert target.read_bytes()==expected
        metadata=baseline.fixture.run(['debugfs','-R',f'stat {path}',str(root)],out/(label+'.audit.log'))
        assert 'User:     0' in metadata and 'Group:     0' in metadata and '0644' in metadata
    print('PASS',label,flush=True)
    return {'case':label,'result':'PASS','entry_masks':masks,'owner':owners[-1],'listener':peers,'wire':wire_audit(out/(label+'.pcap'),port,qmp_directory is not None)}

def main():
    parser=argparse.ArgumentParser(); parser.add_argument('--cpus',type=int,nargs='+',choices=[1,4,8],default=[1,4,8]); parser.add_argument('--firmware',nargs='+',choices=['bios','uefi'],default=['bios','uefi']); parser.add_argument('--nic',choices=['e1000','e1000e','none'],default='e1000e'); parser.add_argument('--link',action='store_true');args=parser.parse_args()
    assert not args.link or args.nic!='none'
    qmp_directory=Path(tempfile.mkdtemp(prefix='smp3-qmp-')) if args.link else None
    parent=ROOT/'build/network-smp-phase3'; parent.mkdir(exist_ok=True,parents=True)
    out=Path(tempfile.mkdtemp(prefix='gate-',dir=parent)); print(out,flush=True)
    result={'result':'FAIL','cases':[],'nic':args.nic,'carrier_loss':args.link}
    (out/'manifest.json').write_text(json.dumps(result,indent=2))
    try:
        workspace=Path(subprocess.check_output(['python3','scripts/create_ext4_guest_workspace.py'],cwd=ROOT,text=True).strip()); result['workspace']=str(workspace)
        shutil.copyfile(workspace/'workspace-manifest.json',out/'sources.json'); instrument(workspace)
        with (out/'build.log').open('w') as log: subprocess.run(['make','-j4','bin/fortress.elf','bin/initramfs.tar'],cwd=workspace,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=600)
        result['build_sha256']={n:baseline.fixture.digest(workspace/n) for n in ('bin/fortress.elf','bin/initramfs.tar')}
        shim=['gcc','-std=c11','-O2','-fPIC','-shared','-Wall','-Wextra','-Werror',str(workspace/'tests/net_tcp_peer_socket_shim.c'),'-ldl','-o',str(out/'peer-socket-shim.so')]
        with (out/'shim-build.log').open('w') as log: subprocess.run(shim,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=60)
        result['peer_shim']={'argv':shim,'sha256':baseline.fixture.digest(out/'peer-socket-shim.so')}
        original=baseline.fixture.run
        def run(argv,log,timeout=120): return original([str(workspace/'tests/net_ordinary_observer.c') if a=='tests/net_cpu_probe_user.c' else a for a in argv],log,timeout)
        baseline.fixture.run=run; password=baseline.secrets.token_hex(16)
        try: disk=baseline.prepare(workspace,out,password)
        finally: baseline.fixture.run=original
        before=baseline.fixture.digest(disk); result['baseline_sha256']=before
        class HTTP(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                self.send_response(200); self.send_header('Content-Length',str(len(BODY))); self.end_headers(); self.wfile.write(BODY)
            def log_message(self,*args): pass
        http_peer=http.server.ThreadingHTTPServer(('127.0.0.1',0),HTTP); worker=threading.Thread(target=http_peer.serve_forever); worker.start()
        dnslog=(out/'dns.stderr').open('wb'); dns=subprocess.Popen(['sudo','-n','python3',str(workspace/'scripts/network_smp_dns_peer.py')],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=dnslog)
        assert dns.stdout.readline()==b'READY\n'
        class UDP(socketserver.BaseRequestHandler):
            def handle(self):
                data,peer=self.request; peer.sendto(data,self.client_address)
        udp=socketserver.UDPServer(('127.0.0.1',17777),UDP)
        udpworker=threading.Thread(target=udp.serve_forever); udpworker.start()
        zero=socket.socket();zero.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);zero.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,1024)
        zero.bind(('127.0.0.1',17779));zero.listen(4);zero.settimeout(.2)
        zero_stop=threading.Event();zero_connections=[];zero_lock=threading.Lock()
        def release_zero():
            with zero_lock:
                for conn in zero_connections:
                    conn.setsockopt(socket.SOL_SOCKET,socket.SO_LINGER,struct.pack('ii',1,0));conn.close()
                zero_connections.clear()
        def zero_run():
            while not zero_stop.is_set():
                try: conn,_=zero.accept()
                except socket.timeout: continue
                with zero_lock:zero_connections.append(conn)
        zeroworker=threading.Thread(target=zero_run);zeroworker.start()
        echo=socket.socket(); echo.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1); echo.bind(('127.0.0.1',17778)); echo.listen(8); echo.settimeout(.2); stop=threading.Event(); echo_results=[]
        def echo_run():
            while not stop.is_set():
                try: conn,_=echo.accept()
                except socket.timeout: continue
                with conn:
                    conn.settimeout(20); data=bytearray()
                    while chunk:=conn.recv(4096): data.extend(chunk)
                    conn.sendall(data); echo_results.append(hashlib.sha256(data).hexdigest())
        echoworker=threading.Thread(target=echo_run); echoworker.start()
        try:
            for firmware in args.firmware:
                for cpus in args.cpus:
                    target=out/f'{firmware}-smp{cpus}.img'; baseline.fixture.run(['cp','--sparse=always',str(disk),str(target)],out/'copy.log')
                    for restart in (0,1):
                        result['cases'].append(boot(out,target,firmware,cpus,restart,password,args.nic,http_peer.server_port,qmp_directory,release_zero))
                        (out/'manifest.json').write_text(json.dumps(result,indent=2))
        finally:
            stop.set(); echoworker.join(timeout=25); echo.close(); assert not echoworker.is_alive()
            http_peer.shutdown(); http_peer.server_close(); worker.join(timeout=5)
            udp.shutdown(); udp.server_close(); udpworker.join(timeout=5)
            zero_stop.set();zeroworker.join(timeout=5);zero.close()
            release_zero()
            dns.stdin.close()
            try: dns.wait(timeout=5)
            except subprocess.TimeoutExpired: dns.kill(); dns.wait(timeout=5)
            dnslog.close(); result['dns_returncode']=dns.returncode
            (out/'echo-results.json').write_text(json.dumps(echo_results))
        assert dns.returncode==0
        assert len(echo_results)==(0 if args.nic=='none' else len(result['cases']))
        assert baseline.fixture.digest(disk)==before
        sources=json.loads((out/'sources.json').read_text())['sources']
        changed=[n for n,sha in sources.items() if not (ROOT/n).is_file() or baseline.fixture.digest(ROOT/n)!=sha]
        current=subprocess.check_output(['git','ls-files','--cached','--others','--exclude-standard','-z'],cwd=ROOT,text=True).split('\0')
        added=[n for n in current if n and n not in sources]
        critical=lambda n:n.startswith(('src/','user/','tests/','scripts/')) or n in ('Makefile','linker.ld','limine.conf')
        result['concurrent_changes']=changed; result['added_sources']=added
        assert not [n for n in changed+added if critical(n)],changed+added
        result['source_preservation']='PASS' if not changed else 'CONCURRENT_DOCUMENTATION_CHANGES'
        result['result']='PASS'
    except BaseException as error: result['failure']=repr(error); raise
    finally:
        if qmp_directory:qmp_directory.rmdir()
        (out/'manifest.json').write_text(json.dumps(result,indent=2)+'\n')
if __name__=='__main__': main()
