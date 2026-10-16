#!/usr/bin/env python3
"""Strict disposable installed-root TCP listener/lifecycle acceptance.

Reuse Phase 2C snapshot-only AP launch, CPU provenance and exact pool observers.
The controlled listener peers, checkpoints, capture audit and argv are Phase 2D.
"""
import argparse, hashlib, json, re, selectors, shutil, socket, subprocess, tempfile, threading, time
from pathlib import Path
import test_net_installed as baseline
import test_network_smp_phase2c as streams
from audit_network_smp_phase2b import audit
from net_tcp_wire_audit import parse as audited_packet, records
ROOT=baseline.fixture.REPO
QMP_DIRECTORY=None

def critical_source(name):
    return streams.critical_source(name) or name in {
        'tests/net_tcp_listener_guest_user.c','scripts/test_network_smp_phase2d.py'}

def instrument(workspace):
    streams.instrument(workspace)
    owner=workspace/'src/net/net_udp_owner.c'; text=owner.read_text()
    text=text.replace('(values[i].kind==NET_OWNER_TCP_RECV ||','(values[i].kind==NET_OWNER_TCP_ACCEPT || values[i].kind==NET_OWNER_TCP_RECV ||')
    text=text.replace('values[i].file || values[i].pin','values[i].file || values[i].accepted || values[i].pin')
    owner.write_text(text)
    syscall=workspace/'src/kernel/syscall.c'; text=syscall.read_text()
    text=text.replace('    switch (syscall_nr) {', '    switch (syscall_nr) {\n        case 0x7ff4: { extern int net_tcp_fixture_accept_arm(unsigned); result=net_tcp_fixture_accept_arm((unsigned)frame->rdi); break; }')
    syscall.write_text(text)
    entry=workspace/'src/net/net_tcp_syscall.c'; text=entry.read_text()
    text='void net_tcp_fixture_accept_hook(unsigned);\n'+text
    start=text.index('static int64_t accept_socket('); end=text.index('int64_t net_tcp_syscall(',start)
    body=text[start:end]
    body=body.replace('        if (process_signal_interrupt()) {', '        if (net_udp_ready(&t)) net_tcp_fixture_accept_hook(1);\n        if (process_signal_interrupt()) {')
    body=body.replace('            if (accepted) fd=fd_alloc(caller,accepted);', '            if (accepted) fd=fd_alloc(caller,accepted);\n            net_tcp_fixture_accept_hook(2);')
    body=body.replace('            decision=true; continue;', '            net_tcp_fixture_accept_hook(error ? 4 : 3);\n            decision=true; continue;')
    text=text[:start]+body+text[end:]
    text+=r'''
#include "serial.h"
static uint64_t fixture_accept_actor;
static unsigned fixture_accept_stage, fixture_accept_signal;
int net_tcp_fixture_accept_arm(unsigned stage) {
    bool killed=stage>16; if (killed) stage-=16;
    if (stage<1 || stage>4) return -1;
    __atomic_store_n(&fixture_accept_signal,killed ? SIGKILL : SIGSTOP,__ATOMIC_RELEASE);
    __atomic_store_n(&fixture_accept_actor,thread_current()->tid,__ATOMIC_RELEASE);
    __atomic_store_n(&fixture_accept_stage,stage,__ATOMIC_RELEASE); return 0;
}
void net_tcp_fixture_accept_hook(unsigned stage) {
    if (__atomic_load_n(&fixture_accept_actor,__ATOMIC_ACQUIRE)!=thread_current()->tid) return;
    unsigned expected=stage;
    if (!__atomic_compare_exchange_n(&fixture_accept_stage,&expected,0,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) return;
    serial_puts("[NET SMP2D SIGNAL PUBLISHED] stage="); serial_print_hex(stage); serial_puts("\n");
    if (process_signal_send_kernel(thread_current()->tid,(int64_t)thread_current()->tid,__atomic_load_n(&fixture_accept_signal,__ATOMIC_ACQUIRE))) __builtin_trap();
}
'''
    entry.write_text(text)
    (workspace/'network-smp2d-overrides.json').write_text(json.dumps({str(p.relative_to(workspace)):baseline.fixture.digest(p) for d in ('src','user') for p in (workspace/d).rglob('*') if p.is_file()},indent=2))

def listener_argv(argv,out,label,nic):
    if QMP_DIRECTORY is not None:
        endpoint=QMP_DIRECTORY/(label+'.sock')
        assert argv[-2:]==['-qmp',f'unix:{endpoint},server=on,wait=off']
        argv=argv[:-2].copy()
        assert argv.count(f'{nic},netdev=net0,id=nic0')==1
        argv[argv.index(f'{nic},netdev=net0,id=nic0')]=f'{nic},netdev=net0'

    if nic=='none':
        assert argv.count('-net')==1
        index=argv.index('-net'); assert argv[index+1]=='none'
        normalized=argv[:index]+['-netdev','user,id=net0','-device','e1000,netdev=net0',
            '-object',f'filter-dump,id=capture,netdev=net0,file={out/(label+".pcap")}']+argv[index+2:]
        return audit(normalized,out,label,'e1000')
    expected='user,id=net0,hostfwd=tcp:127.0.0.1:19000-:9000'
    assert argv.count(expected)==1
    normalized=['user,id=net0' if value==expected else value for value in argv]
    return audit(normalized,out,label,nic)

def link_set(out,label,up):
    endpoint=QMP_DIRECTORY/(label+'.sock')
    assert endpoint.resolve().parent==QMP_DIRECTORY.resolve() and len(str(endpoint).encode())<104
    exchanges=[]
    with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as peer:
        peer.settimeout(5); peer.connect(str(endpoint))
        with peer.makefile('rwb') as stream:
            exchanges.append(json.loads(stream.readline()))
            for identity,command in enumerate(({'execute':'qmp_capabilities'},
                    {'execute':'set_link','arguments':{'name':'nic0','up':up}}),1):
                command['id']=identity; exchanges.append(command)
                stream.write((json.dumps(command)+'\n').encode()); stream.flush()
                while True:
                    response=json.loads(stream.readline()); exchanges.append(response)
                    if response.get('id')==identity:
                        assert 'return' in response,response
                        break
    path=out/(label+'.qmp.json'); saved=json.loads(path.read_text()) if path.is_file() else []
    saved.append({'up':up,'exchanges':exchanges})
    path.write_text(json.dumps(saved,indent=2))

def listener_capture(path,peers):
    flows={}; bases={}
    for _,raw in records(path):
        packet=audited_packet(raw)
        if not packet or 9000 not in (packet['source'],packet['dest']): continue
        outbound=packet['source']==9000
        port=packet['dest'] if outbound else packet['source']
        key=(port,outbound)
        if packet['flags']&2:
            base=(packet['seq']+1)&0xffffffff
            assert key not in bases or bases[key]==base
            bases[key]=base
        flow=flows.setdefault(port,[{},{}]); stream=flow[int(outbound)]
        if packet['data']:
            assert key in bases
            for i,value in enumerate(packet['data']):
                offset=(packet['seq']+i-bases[key])&0xffffffff
                assert offset<256
                assert offset not in stream or stream[offset]==value
                stream[offset]=value
    digests=[]
    for streams in flows.values():
        if not any(streams): continue
        data=[bytes(stream[k] for k in sorted(stream)) for stream in streams]
        assert len(data[0])==256 and (data[0]==data[1] or not data[1])
        digests.append((hashlib.sha256(data[0]).hexdigest(),bool(data[1])))
    assert sorted(digests)==sorted((peer['sha256'],peer['echo']) for peer in peers)
    path.with_suffix('.listener-audit.json').write_text(json.dumps({'result':'PASS','streams':len(digests),'sha256':digests},indent=2))


def boot(out,disk,firmware,cpus,password,restart,nic='e1000'):
    label=f'{firmware}-smp{cpus}-boot{restart}'
    assert disk.is_file() and disk.resolve().parent==out.resolve()
    argv=['qemu-system-x86_64','-accel','tcg','-M','q35','-m','512M','-smp',str(cpus),
          '-display','none','-monitor','none','-no-reboot','-serial','stdio',
          '-drive',f'file={disk},if=none,id=target,format=raw','-device','nvme,drive=target,serial=NETINSTALLED,bootindex=1',
          '-netdev','user,id=net0,hostfwd=tcp:127.0.0.1:19000-:9000','-device',f'{nic},netdev=net0',
          '-object',f'filter-dump,id=capture,netdev=net0,file={out/(label+".pcap")}']
    if nic=='none': argv=argv[:argv.index('-netdev')]+['-net','none']
    if firmware=='uefi':
        variables=out/(label+'.vars.fd'); shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
        argv+=['-drive','if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
               '-drive',f'if=pflash,format=raw,unit=1,file={variables}']
    if QMP_DIRECTORY is not None:
        argv[argv.index(f'{nic},netdev=net0')]=f'{nic},netdev=net0,id=nic0'
        argv+=['-qmp',f'unix:{QMP_DIRECTORY/(label+".sock")},server=on,wait=off']
    assert listener_argv(argv,out,label,nic)
    (out/(label+'.argv.json')).write_text(json.dumps(argv,indent=2))
    transcript=bytearray(); commands=[]
    err=(out/(label+'.stderr')).open('wb')
    proc=subprocess.Popen(argv,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=err,
)
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
        if nic=='none':
            command(probe('N'))
            start=len(transcript); send('sudo shutdown')
            wait(lambda t:t.endswith('sudo password: '),start); send(password)
            wait(lambda t:'[POWER] Shutdown initiated' in t,start)
            assert proc.wait(timeout=30)==0
            root=out/(label+'.root.img'); baseline.extract_root(disk,root)
            baseline.fixture.run(['e2fsck','-fn',str(root)],out/(label+'.fsck.log'))
            print('PASS',label,'absent NIC',flush=True)
            return {'case':label,'result':'PASS','absent_nic':True}
        cpulist=list(range(restart*4,(restart+1)*4)) if cpus==8 else list(range(cpus))
        for action in (('LINK','STOP','KILL','INT') if QMP_DIRECTORY is not None else ('STOP','KILL','INT')):
            start=len(transcript)
            launched=command(probe('E')+' &','')
            wait(lambda t:'NET SMP2D LISTENER READY' in t,start)
            job=int(re.findall(r'\[(\d+)\]\s+\d+',launched)[-1])
            command(probe('m'))
            command('/sbin/ifconfig','inet 10.0.2.15/24')
            command('ping -c 1 10.0.2.2','bytes from 10.0.2.2')
            if action=='LINK':
                link_set(out,label,False)
                deadline=time.monotonic()+30
                while 'link DOWN' not in command('/sbin/ifconfig',''):
                    assert time.monotonic()<deadline
                while re.search(r'\b(?:Running|Stopped)\b',command('jobs','')):
                    assert time.monotonic()<deadline
                command(probe('g'))
                link_set(out,label,True)
                deadline=time.monotonic()+30
                while 'link UP' not in command('/sbin/ifconfig',''):
                    assert time.monotonic()<deadline
                command('ping -c 1 10.0.2.2','bytes from 10.0.2.2')
                continue
            command(f'kill %{job} {action}','')
            if action=='STOP':
                command('jobs','Stopped')
                command('nslookup -s 10.0.2.2 tcp.test','10.0.2.2')
                command(f'kill %{job} CONT','')
                command(f'kill %{job} KILL','')
            command(probe('g'))
        peer_results=[]
        def listener(cmd,count):
            killed=cmd[-1] in ('V','W','X','Y')
            echoed=not killed and cmd[-1]!='D'
            saturated=cmd[-1]=='Q'
            before_synacks=set()
            if saturated:
                before_synacks={p['dest'] for _,r in records(out/(label+'.pcap')) if (p:=audited_packet(r)) and p['source']==9000 and p['flags']&2}
            start=len(transcript); send(cmd+' &' if saturated else cmd)
            wait(lambda t:'NET SMP2D LISTENER READY' in t,start)
            if saturated:
                launched=wait(prompt,start)
                job=int(re.findall(r'\[(\d+)\]\s+\d+',launched)[-1])
                command(f'kill %{job} STOP','')
                command('jobs','Stopped')
            results=[]; threads=[]; result_lock=threading.Lock()
            peer_path=out/(label+'.peer-attempts.json')
            attempts=json.loads(peer_path.read_text()) if peer_path.is_file() else []
            attempt={'command':cmd,'expected':count,'results':results}; attempts.append(attempt)
            def peer(tag):
                try:
                    payload=bytes((i+tag)&255 for i in range(256))
                    with socket.create_connection(('127.0.0.1',19000),timeout=20) as conn:
                        conn.settimeout(90); conn.sendall(payload); conn.shutdown(socket.SHUT_WR)
                        data=bytearray()
                        while True:
                            chunk=conn.recv(1024)
                            if not chunk: break
                            data.extend(chunk)
                        assert bytes(data)==(payload if echoed else b''),(len(data),tag)
                        results.append({'tag':tag,'sha256':hashlib.sha256(payload).hexdigest(),'echo':echoed})
                except Exception as error: results.append({'failure':repr(error),'tag':tag})
                finally:
                    with result_lock: peer_path.write_text(json.dumps(attempts,indent=2))
            for i in range(count):
                t=threading.Thread(target=peer,args=(1+len(peer_results)+i,)); threads.append(t); t.start()
            if saturated:
                command('/sbin/ifconfig','inet 10.0.2.15/24')
                command('ping -c 1 10.0.2.2','bytes from 10.0.2.2')
                command('nslookup -s 10.0.2.2 tcp.test','10.0.2.2')
                deadline=time.monotonic()+20
                while True:
                    try:
                        seen={p['dest'] for _,r in records(out/(label+'.pcap')) if (p:=audited_packet(r)) and p['source']==9000 and p['flags']&2}
                    except AssertionError:
                        assert time.monotonic()<deadline; continue
                    if len(seen-before_synacks)==4: break
                    assert time.monotonic()<deadline,(seen,before_synacks)
                    command('/sbin/ifconfig','inet 10.0.2.15/24')
                assert not results,results  # no accept/echo while caller stopped
                command('jobs','Stopped')
                command(f'fg %{job}')
            stopped=wait(prompt,start)
            if cmd[-1] in ('S','P','C','B'):
                assert 'Stopped' in stopped,stopped
                jobs=command('jobs','Stopped')
                job=int(re.findall(r'\[(\d+)\].*Stopped',jobs)[-1])
                command('/sbin/ifconfig','inet 10.0.2.15/24')
                command('ping -c 1 10.0.2.2','bytes from 10.0.2.2')
                command(f'fg %{job}')
            for t in threads: t.join(timeout=35); assert not t.is_alive()
            assert len(results)==count and all('failure' not in r for r in results),results
            peer_results.extend(results)
            command('echo $?', '\n137\n' if killed else '\n0\n')
            command(probe('g'))
        for cpu in cpulist:
            listener(f'net-cpu-probe --worker {cpu} L',1)
        listener(probe('H'),4)
        listener(probe('F'),1)
        listener(probe('D'),1)
        listener(probe('Q'),4)
        for mode in ('S','P','C','B','V','W','X','Y'): listener(probe(mode),1)
        command('nslookup -s 10.0.2.2 tcp.test','10.0.2.2')
        command('/sbin/ifconfig','inet 10.0.2.15/24')
        command('ping -c 1 10.0.2.2','bytes from 10.0.2.2')
        command(probe('g'))
        (out/(label+'.peers.json')).write_text(json.dumps(peer_results,indent=2))
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
        if QMP_DIRECTORY is not None:
            endpoint=QMP_DIRECTORY/(label+'.sock')
            if endpoint.exists(): endpoint.unlink()
        (out/(label+'.termination.json')).write_text(json.dumps({'pid':proc.pid,'returncode':proc.returncode,'reaped':proc.poll() is not None}))
        (out/(label+'.serial.log')).write_bytes(transcript)
        (out/(label+'.commands.json')).write_text(json.dumps(commands,indent=2))
    log=text()
    entries=re.findall(r'\[NET SMP2C MASK\] nr=(0x[0-9A-Fa-f]+) cpus=(0x[0-9A-Fa-f]+)',log)
    masks={int(nr,16):int(mask,16) for nr,mask in entries}
    for nr in (38,39,44,45,46,47,48):
        expected=sum(1<<cpu for cpu in cpulist)
        assert masks.get(nr,0)&expected==expected,(nr,masks)
    assert {int(v,16) for v in re.findall(r'\[NET SMP0 OWNER\] cpu=(0x[0-9A-Fa-f]+)',log)}=={0}
    assert '[NET SMP2C CLEAN]' in log
    stages=[int(v,16) for v in re.findall(r'\[NET SMP2D SIGNAL PUBLISHED\] stage=(0x[0-9A-Fa-f]+)',log)]
    assert sorted(stages)==[1,1,2,2,3,3,4,4],stages
    assert all(int(status) in (0,130,137) for status in re.findall(r'Done\((\d+)\)',log))
    owner_masks=re.findall(r'\[NET SMP2C OWNER\] cpus=(0x[0-9A-Fa-f]+) steps=(0x[0-9A-Fa-f]+)',log)
    assert owner_masks and int(owner_masks[-1][0],16)==1 and int(owner_masks[-1][1],16)>0
    listener_capture(out/(label+'.pcap'),peer_results)
    root=out/(label+'.root.img'); baseline.extract_root(disk,root)
    baseline.fixture.run(['e2fsck','-fn',str(root)],out/(label+'.fsck.log'))
    print('PASS',label,flush=True)
    for path,expected in [('/etc/network/network.conf',config.encode()),('/etc/resolv.conf',b'nameserver 1.1.1.1\nnameserver 8.8.8.8\n')]:
        target=out/(label+'.'+Path(path).name)
        baseline.fixture.run(['debugfs','-R',f'dump {path} {target}',str(root)],out/(label+'.audit.log'))
        assert target.read_bytes()==expected,path
        metadata=baseline.fixture.run(['debugfs','-R',f'stat {path}',str(root)],out/(label+'.audit.log'))
        assert 'User:     0' in metadata and 'Group:     0' in metadata and '0644' in metadata,metadata
    negatives=0
    for extra in (['-drive','file=/dev/sda,format=raw'],['-blockdev','driver=host_device,filename=/dev/sda'],['-cdrom','/tmp/shared.iso'],['-device','usb-storage,drive=shared']):
        try: listener_argv(argv+extra,out,label,nic)
        except AssertionError: negatives+=1
        else: raise AssertionError('unauthorized storage accepted')
    return {'case':label,'result':'PASS','syscall_cpu_masks':masks,'stop_stages':stages,'listener_streams':len(peer_results),'negative_storage_exclusions':negatives}

def main():
    global QMP_DIRECTORY
    parser=argparse.ArgumentParser()
    parser.add_argument('--cpus',type=int,nargs='+',choices=[1,4,8],default=[1,4,8])
    parser.add_argument('--firmware',nargs='+',choices=['bios','uefi'],default=['bios','uefi'])
    parser.add_argument('--workspace',type=Path)
    parser.add_argument('--nic',choices=['e1000','e1000e','none'],default='e1000e')
    parser.add_argument('--link',action='store_true')
    args=parser.parse_args()
    if args.link:
        assert args.nic!='none'
        QMP_DIRECTORY=Path(tempfile.mkdtemp(prefix='smp2d-qmp-'))
    parent=ROOT/'build/network-smp-phase2d'; parent.mkdir(exist_ok=True,parents=True)
    out=Path(tempfile.mkdtemp(prefix='gate-',dir=parent)); print(out,flush=True)
    manifest={'result':'FAIL','cases':[],'nic':args.nic,'carrier_loss':args.link}
    try:
        workspace=args.workspace.resolve() if args.workspace else Path(subprocess.check_output(['python3','scripts/create_ext4_guest_workspace.py'],text=True).strip())
        manifest['workspace']=str(workspace)
        shutil.copyfile(workspace/'workspace-manifest.json',out/'sources.json')
        if args.workspace:
            assert workspace.is_relative_to(ROOT/'.codex-remote-attachments') and (workspace/'network-smp2d-overrides.json').is_file()
        else: instrument(workspace)
        with (out/'build.log').open('w') as log:
            subprocess.run(['make','-j4','bin/fortress.elf','bin/initramfs.tar'],cwd=workspace,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=600)
        manifest['build_sha256']={n:baseline.fixture.digest(workspace/n) for n in ('bin/fortress.elf','bin/initramfs.tar')}
        original=baseline.fixture.run
        def run(argv,log,timeout=120):
            return original([str(workspace/'tests/net_tcp_listener_guest_user.c') if a=='tests/net_cpu_probe_user.c' else a for a in argv],log,timeout)
        baseline.fixture.run=run
        password=baseline.secrets.token_hex(16)
        try: disk=baseline.prepare(workspace,out,password)
        finally: baseline.fixture.run=original
        before=baseline.fixture.digest(disk); manifest['baseline_sha256']=before
        manifest['probe_sha256']=baseline.fixture.digest(out/'tree/bin/net-cpu-probe')
        if args.nic!='none':
            dns_log=(out/'dns.stderr').open('wb')
            dns=subprocess.Popen(['sudo','-n','python3',str(workspace/'scripts/network_smp_dns_peer.py')],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=dns_log)
            assert dns.stdout.readline()==b'READY\n'
        for firmware in args.firmware:
            for cpus in args.cpus:
                target=out/f'{firmware}-smp{cpus}.img'
                baseline.fixture.run(['cp','--sparse=always',str(disk),str(target)],out/'copy.log')
                for restart in (0,1):
                    case=boot(out,target,firmware,cpus,password,restart,args.nic)
                    manifest['cases'].append(case)
                    (out/'manifest.json').write_text(json.dumps(manifest,indent=2))
        assert baseline.fixture.digest(disk)==before
        sources=json.loads((out/'sources.json').read_text())['sources']
        current=subprocess.check_output(['git','ls-files','--cached','--others','--exclude-standard','-z'],cwd=ROOT,text=True).split('\0')
        changed=[n for n,sha in sources.items() if not (ROOT/n).is_file() or baseline.fixture.digest(ROOT/n)!=sha]
        added=[n for n in current if n and n not in sources and critical_source(n)]
        manifest['concurrent_source_changes']=changed
        manifest['added_executable_or_fixture_sources']=added
        critical=[n for n in changed if critical_source(n)]+added
        manifest['changed_executable_or_fixture_sources']=critical
        assert not critical,critical
        manifest['source_preservation']='CONCURRENT_NONPRODUCTION_CHANGES' if changed else 'PASS'
        manifest['result']='PASS'
    except BaseException as error:
        manifest['failure']=repr(error); raise
    finally:
        if 'dns' in locals():
            dns.stdin.close()
            try: dns.wait(timeout=5)
            except subprocess.TimeoutExpired: dns.kill(); dns.wait(timeout=5)
            dns_log.close()
            manifest['dns_returncode']=dns.returncode
            if dns.returncode: manifest['result']='FAIL'
        if QMP_DIRECTORY is not None: QMP_DIRECTORY.rmdir()
        (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    if manifest['result']!='PASS':
        raise RuntimeError(manifest.get('failure','DNS peer failed; see retained manifest'))
if __name__=='__main__': main()
