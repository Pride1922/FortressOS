"""Exact production-image acceptance on retained disposable raw USB copies.
No physical devices, extra data disks, test login escape or raw-write probes.
"""
from pathlib import Path
import argparse,hashlib,json,re,selectors,shutil,struct,subprocess,tempfile,time,uuid
root=Path(__file__).resolve().parent.parent
p=argparse.ArgumentParser();p.add_argument('--image',type=Path,required=True)
a=p.parse_args();source=a.image.resolve();assert source.is_file() and source.is_relative_to(root)
out=Path(tempfile.mkdtemp(prefix='image-',dir=root/'build/permissions-phase5'))
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
before=sha(source);cases=[]
def run(mode,cpus,disk,number):
    label=f'{mode}-{cpus}-boot{number}';variables=out/(label+'-vars.fd')
    cmd=['qemu-system-x86_64','-M','q35','-m','2G','-accel','tcg','-smp',str(cpus),'-display','none','-monitor','none','-no-reboot','-serial','stdio','-net','none',
        '-drive',f'file={disk},if=none,id=usb,format=raw','-device','qemu-xhci,id=xhci,p2=4,p3=0','-device','usb-storage,drive=usb,bus=xhci.0,bootindex=1']
    if mode=='uefi':
        shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
        cmd+=['-drive','if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd','-drive',f'if=pflash,format=raw,unit=1,file={variables}']
    def preflight(actual):
        assert actual==cmd and disk.is_file() and disk.parent==out
        assert '-blockdev' not in actual and '-snapshot' not in actual and '-fw_cfg' not in actual
        assert actual.count('-drive')==(3 if mode=='uefi' else 1)
        assert not any('/dev/' in v or 'nvme' in v for v in actual)
    preflight(cmd)
    for extra in (['-drive','file=/dev/sda'],['-blockdev','driver=host_device'],['-snapshot']):
        try:preflight(cmd+extra)
        except AssertionError:pass
        else:raise AssertionError('extra storage accepted')
    (out/(label+'-argv.json')).write_text(json.dumps(cmd,indent=2))
    transcript=bytearray();err=(out/(label+'.stderr')).open('wb')
    proc=subprocess.Popen(cmd,cwd=root,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=err)
    selector=selectors.DefaultSelector();selector.register(proc.stdout,selectors.EVENT_READ)
    def clean(data):return re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]','',data.decode(errors='replace')).replace('\r','')
    def prompt(t):return re.search(r'(?:^|\n)[#$] $',t)
    def wait(pred,start=0,timeout=180):
        end=time.monotonic()+timeout
        while time.monotonic()<end:
            for key,_ in selector.select(.1):
                b=key.fileobj.read1(65536)
                if b:transcript.extend(b);(out/(label+'.log')).write_bytes(transcript)
            text=clean(transcript[start:])
            assert not any(s in clean(transcript) for s in ('CPU EXCEPTION KERNEL PANIC','[FATAL]','[FAIL]')),clean(transcript[-6000:])
            if pred(text):return text
            assert proc.poll() is None,clean(transcript[-6000:])
        raise TimeoutError(clean(transcript[-6000:]))
    def send(s):proc.stdin.write((s+'\n').encode());proc.stdin.flush()
    def execute(command,expected):
        start=len(transcript);send(command);return wait(lambda t:expected in t and prompt(t),start)
    def success(command,expected=''):
        text=execute(command,expected)
        assert '[PROCESS] Exit status' not in text,text
        return text
    try:
        wait(lambda t:'FortressOS login: ' in t);send('root')
        wait(lambda t:t.endswith('Password: '));send('locked-account-check')
        wait(lambda t:'Login incorrect' in t and t.endswith('FortressOS login: '))
        send('operator');wait(prompt)
        text=clean(transcript);assert 'WARNING:' in text and 'passwordless' in text
        assert 'read-write at /mnt' in text and 'Journal recovery' not in text
        success('id','uid=1000 gid=1000');success('env','HOME=/run/user/1000')
        execute('cat /etc/shadow','cat: permission denied: /etc/shadow')
        success('sudo id','uid=0 gid=0 groups=0')
        execute('/bin/dmesg','[PROCESS] Exit status 1');success('sudo dmesg','-->')
        if number==1:
            success('sudo /bin/shell','# ')
            success('id','uid=0 gid=0')
            success('mkdir /mnt/permissions-phase5')
            success('chown 1000:1000 /mnt/permissions-phase5')
            success('exit')
            success('echo phase5-owned-persistence > /mnt/permissions-phase5/owned.txt')
            success('chmod 0640 /mnt/permissions-phase5/owned.txt')
            success('sudo chown 4294967295:4294967294 /mnt/permissions-phase5/owned.txt')
            success('cat /bin/sudo > /mnt/permissions-phase5/nosuid-sudo')
            success('sudo chown 0:0 /mnt/permissions-phase5/nosuid-sudo')
            success('sudo chmod 4755 /mnt/permissions-phase5/nosuid-sudo')
        else:
            success('sudo cat /mnt/permissions-phase5/owned.txt','phase5-owned-persistence')
            success('ls -l /mnt/permissions-phase5/owned.txt','4294967295 4294967294')
        execute('cat /mnt/permissions-phase5/owned.txt','cat: permission denied:')
        execute('/mnt/permissions-phase5/nosuid-sudo id','sudo: privileged installation required')
        success('id','uid=1000 gid=1000')
        success('sync','Filesystem synced.')
        success('sudo /bin/shell','# ')
        start=len(transcript);send('shutdown')
        wait(lambda t:'Shutdown initiated' in t,start);assert proc.wait(timeout=30)==0
        cases.append({'firmware':mode,'smp':cpus,'boot':number,'result':'PASS','argv':label+'-argv.json','disk_sha256':sha(disk)})
        print('PASS',label,flush=True)
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:proc.wait(timeout=5)
            except subprocess.TimeoutExpired:proc.kill();proc.wait(timeout=5)
        selector.close();err.close();(out/(label+'.log')).write_bytes(transcript)
def audit(disk,label):
    part=out/(label+'.ext4')
    with disk.open('rb') as f:
        f.seek(133120*512);part.write_bytes(f.read(131072*512))
    log=[]
    for command in (['e2fsck','-fn',str(part)],['dumpe2fs','-h',str(part)]):
        r=subprocess.run(command,capture_output=True,text=True);log.append(json.dumps(command)+'\n'+r.stdout+r.stderr);assert r.returncode==0,log[-1]
    for name,uid,gid,mode in [('owned.txt',4294967295,4294967294,'0640'),('nosuid-sudo',0,0,'04755')]:
        r=subprocess.run(['debugfs','-R','stat /permissions-phase5/'+name,str(part)],capture_output=True,text=True,check=True)
        text=r.stdout+r.stderr;log.append(text)
        ids=re.search(r'User:\s+(-?\d+)\s+Group:\s+(-?\d+)',text)
        assert ids and tuple(int(v)&0xffffffff for v in ids.groups())==(uid,gid),text
        actual=re.search(r'Mode:\s+([0-7]+)',text);assert actual and int(actual[1],8)==int(mode,8),text
    dump=out/(label+'-owned.txt');subprocess.run(['debugfs','-R',f'dump /permissions-phase5/owned.txt {dump}',str(part)],check=True,capture_output=True)
    assert dump.read_bytes()==b'phase5-owned-persistence\n'
    (out/(label+'-linux.log')).write_text('\n'.join(log))
try:
    for mode in ('bios','uefi'):
        for cpus in (1,4):
            disk=out/f'{mode}-{cpus}.img';shutil.copyfile(source,disk)
            for number in (1,2):run(mode,cpus,disk,number);audit(disk,f'{mode}-{cpus}-boot{number}')
finally:
    assert sha(source)==before,'shipped image changed'
    (out/'manifest.json').write_text(json.dumps({'image':str(source),'image_sha256':before,'cases':cases,
        'artifact_sha256':{p.name:sha(p) for p in out.iterdir() if p.is_file()},
        'scope':'exact production image; 2 boots x BIOS/UEFI x SMP=1/4; USB nosuid, ownership/mode/bytes, independent Linux fsck/debugfs; no physical claim'},indent=2)+'\n')
    print('Evidence:',out,flush=True)
