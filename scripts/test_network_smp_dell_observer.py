"""Validate the exact live Dell observer artifacts, including SMP1 negative proof.

Only delivery ISO/read-only USB images and disposable OVMF variables are allowed.
Controlled loopback HTTP/DNS peers; no installed/data disks or physical writes.
"""
import argparse,hashlib,http.server,json,os,re,shutil,socket,subprocess,tempfile,threading,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
NAMES=('/bin/ping','/bin/nslookup','/bin/wget','/sbin/dhcpc')
BODY=bytes((i*37+(i>>8))&255 for i in range(1024*1024))

def parse(text,cpus):
    rows={name:(int(a,16),int(n,16)) for name,a,n in re.findall(r'(/\S+) all=(0x[0-9a-f]+) net=(0x[0-9a-f]+) AP=(?:YES|NO)',text)}
    owners=re.findall(r'OWNER cpus=(0x[0-9a-f]+) steps=(0x[0-9a-f]+)',text)
    assert set(rows)==set(NAMES) and owners,text[-2500:]
    owner,steps=map(lambda n:int(n,16),owners[-1]);assert owner==1 and steps>0
    assert all(n and not (a & ~((1<<cpus)-1)) and not (n & ~a) for a,n in rows.values()),rows
    passed=all(n&~1 for a,n in rows.values())
    if cpus==1:assert not passed and all(a==n==1 for a,n in rows.values())
    return {'rows':rows,'owner_cpus':owner,'owner_steps':steps,'four_commands_ap':passed}

def guest(delivery,out,firmware,cpus,raw,http_port,manifest):
    label=f'{firmware}-smp{cpus}-'+('usb' if raw else 'iso');retained=out/label;retained.mkdir()
    with tempfile.TemporaryDirectory(prefix='fortress-dell-observer-') as tmp:
        native=Path(tmp);uart=native/'uart.sock';log=retained/'serial.log';chunks=bytearray();stop=threading.Event()
        argv=['qemu-system-x86_64','-M','q35','-m','2G','-accel','kvm','-cpu','host','-smp',str(cpus),'-display','none','-monitor','none','-no-reboot',
              '-netdev','user,id=observernet','-device','e1000e,netdev=observernet',
              '-chardev',f'socket,id=uart,path={uart},server=on,wait=off','-serial','chardev:uart']
        if raw:
            source=native/'source.img';shutil.copyfile(delivery/'fortress-dell-cpu-observer.img',source)
            argv+=['-drive',f'file={source},if=none,id=source,format=raw,readonly=on','-device','ich9-usb-ehci1,id=ehci','-device','usb-storage,bus=ehci.0,drive=source,bootindex=1']
        else:argv+=['-boot','d','-cdrom',str(delivery/'fortress-dell-cpu-observer.iso')]
        if firmware=='uefi':
            variables=native/'vars.fd';shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
            argv+=['-drive','if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd','-drive',f'if=pflash,format=raw,unit=1,file={variables}']
        assert argv.count('-drive')==(1 if raw else 0)+(2 if firmware=='uefi' else 0)
        assert argv.count('-device')==(3 if raw else 1)
        assert not any('/dev/' in x or 'nvme,' in x or 'virtio-blk' in x for x in argv)
        (retained/'argv.json').write_text(json.dumps(argv,indent=2)+'\n')
        manifest.setdefault('cases',[]).append({'label':label,'result':'FAIL','argv':argv})
        case=manifest['cases'][-1]
        proc=None;channel=None;reader=None
        with (retained/'stderr.log').open('wb') as err:
            try:
                proc=subprocess.Popen(argv,stdout=subprocess.DEVNULL,stderr=err)
                deadline=time.monotonic()+20
                while time.monotonic()<deadline:
                    try:
                        channel=socket.socket(socket.AF_UNIX);channel.connect(str(uart));break
                    except (FileNotFoundError,ConnectionRefusedError):
                        channel.close();channel=None;time.sleep(.05)
                assert channel is not None
                channel.settimeout(.2)
                def drain():
                    while not stop.is_set():
                        try:
                            b=channel.recv(65536)
                            if not b:return
                            chunks.extend(b)
                        except socket.timeout:pass
                        except OSError:return
                reader=threading.Thread(target=drain);reader.start()
                def text():
                    log.write_bytes(bytes(chunks))
                    return re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]','',bytes(chunks).decode(errors='replace')).replace('\r','')
                prompt=lambda s:re.search(r'(?:^|\n)[A-Za-z0-9_-]+@[A-Za-z0-9-]+:[^\n]*[#$] $',s)
                fault_baseline=None
                def wait(predicate,start=0,timeout=90):
                    end=time.monotonic()+timeout
                    while time.monotonic()<end:
                        t=text();assert not any(n in t for n in ('KERNEL PANIC','[FATAL]')),t[-5000:]
                        if fault_baseline is not None:assert t.count('[PROCESS FAULT]')==fault_baseline,t[-5000:]
                        if predicate(t[start:]):return t[start:]
                        assert proc.poll() is None,t[-5000:];time.sleep(.05)
                    raise TimeoutError(text()[-5000:])
                def send(cmd):channel.sendall(cmd.encode()+b'\n')
                def command(cmd,expected='',timeout=45):
                    start=len(text());case.setdefault('commands',[]).append(cmd);send(cmd)
                    return wait(lambda t:'\n' in t and expected in t and prompt(t),start,timeout)
                wait(lambda t:'FortressOS login: ' in t)
                # Normal live boot deliberately runs its existing fault-isolation
                # acceptance before login. Permit only that named, completed test.
                boot=text();fault_baseline=boot.count('[PROCESS FAULT]')
                assert fault_baseline==1 and '(faulty_user)' in boot and '[PASS] Fault isolation verified' in boot
                send('operator');wait(prompt)
                command('cat /run/dhcpc.lease','IP=10.0.2.15')
                assert ('Single-CPU system confirmed' if cpus==1 else f'All {cpus} CPU(s) accounted for') in text()
                command('net-cpu-probe cpu-invalid','OBSERVER bounds PASS')
                # Verify exact functional output before background UART writers
                # can interleave character streams. CPU masks are independent
                # monotonic kernel counters, checked separately below.
                command('ping -c 1 10.0.2.2',' bytes from 10.0.2.2')
                command('nslookup -s 10.0.2.2 service.test','Address: 10.0.2.2')
                command(f'wget -q -O - http://10.0.2.2:{http_port}/test.bin | sha256sum',hashlib.sha256(BODY).hexdigest())
                command('sudo dhcpc --dry-run','valid (dry-run)')
                results=None
                for attempt in range(8):
                    for cmd in ('ping -c 2 10.0.2.2 &','nslookup -s 10.0.2.2 service.test &',
                                f'wget -q -O - http://10.0.2.2:{http_port}/test.bin | sha256sum &','sudo dhcpc --dry-run &'):
                        command(cmd)
                    command('sleep 6',timeout=20)
                    report=command('net-cpu-probe cpu','OWNER cpus=')
                    results=parse(report,cpus)
                    if cpus==1 or results['four_commands_ap']:break
                assert results and (cpus==1 or results['four_commands_ap']),results
                command('net-cpu-probe cpu-detail','OWNER cpus=')
                case.update(results);case['result']='PASS'
                send('sudo shutdown');wait(lambda t:'[POWER] Shutdown initiated' in t);assert proc.wait(timeout=30)==0
            finally:
                stop.set()
                if channel:channel.close()
                if reader:reader.join(timeout=3)
                if proc and proc.poll() is None:
                    proc.terminate()
                    try:proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:proc.kill();proc.wait(timeout=5)
                log.write_bytes(bytes(chunks))
    print('PASS',label,case.get('rows'),flush=True)

def main():
    p=argparse.ArgumentParser();p.add_argument('--delivery',type=Path,required=True);args=p.parse_args();delivery=args.delivery.resolve()
    assert delivery.is_relative_to((ROOT/'build/network-smp-phase4').resolve())
    producer=json.loads((delivery/'manifest.json').read_text());assert producer['result']=='PASS'
    out=Path(tempfile.mkdtemp(prefix='guest-',dir=delivery));print(out,flush=True)
    result={'result':'FAIL','delivery':str(delivery),'cases':[]};dns=None;server=None;worker=None
    hashes={name:hashlib.sha256((delivery/name).read_bytes()).hexdigest() for name in ('fortress-dell-cpu-observer.iso','fortress-dell-cpu-observer.img')}
    class HTTP(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            self.send_response(200);self.send_header('Content-Length',str(len(BODY)));self.end_headers();self.wfile.write(BODY)
        def log_message(self,*args):pass
    try:
        dns=subprocess.Popen(['sudo','-n','python3',str(ROOT/'scripts/network_smp_dns_peer.py')],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=(out/'dns.stderr').open('wb'))
        import select
        assert select.select([dns.stdout],[],[],10)[0] and dns.stdout.readline()==b'READY\n','controlled DNS peer did not start'
        server=http.server.ThreadingHTTPServer(('127.0.0.1',0),HTTP);worker=threading.Thread(target=server.serve_forever);worker.start()
        for firmware,cpus,raw in (('bios',1,False),('bios',8,False),('uefi',1,False),('uefi',8,False),('uefi',8,True)):
            guest(delivery,out,firmware,cpus,raw,server.server_port,result)
        assert all(hashlib.sha256((delivery/name).read_bytes()).hexdigest()==h for name,h in hashes.items())
        result['result']='PASS';result['image_sha256']=hashes
    finally:
        if dns:
            dns.stdin.close()
            try:dns.wait(timeout=10)
            except subprocess.TimeoutExpired:dns.kill();dns.wait(timeout=5)
        if server:server.shutdown();server.server_close();worker.join(timeout=5)
        (out/'manifest.json').write_text(json.dumps(result,indent=2)+'\n')
    print(out,flush=True)
if __name__=='__main__':main()
