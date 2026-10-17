"""Direct sudo/sshd and authenticated detached launch; disposable QEMU fixtures."""
import json,re,selectors,shutil,subprocess,sys,tempfile,time
from pathlib import Path
sys.path.insert(0,str(Path('scripts').resolve()))
import test_ssh as fixture
if len(sys.argv)==2 and sys.argv[1]=='--all':
    cases=[('foreground','bios'),('background','bios'),('passwordless-background','bios')]
    cases += [(mode,firmware) for firmware in ('bios','uefi') for mode in ('detached','detached-root')]
    for mode,firmware in cases:
        subprocess.run([sys.executable,__file__,mode,firmware],check=True,timeout=240)
    print('PASS sudo/sshd launch matrix: 7 boots, BIOS/UEFI detached launch and shell-exit survival',flush=True)
    sys.exit(0)
mode=sys.argv[1] if len(sys.argv)>1 else 'foreground'
assert mode in ('foreground','background','passwordless-background','detached','detached-root')
firmware=sys.argv[2] if len(sys.argv)>2 else 'bios'
assert firmware in ('bios','uefi')
parent=Path('build/sshd-sudo').resolve();parent.mkdir(exist_ok=True)
out=Path(tempfile.mkdtemp(prefix=firmware+'-'+mode+'-',dir=parent))
key=out/'client';subprocess.run(['ssh-keygen','-q','-t','ed25519','-N','','-f',str(key)],check=True)
disk=out/'fixture.img';fixture.create_nvme_fixture(disk,key.with_suffix('.pub').read_text())
iso=out/'fixture.iso'
if mode=='passwordless-background':shutil.copyfile('bin/fortress.iso',iso)
else:fixture.build_test_iso(iso,fixture.TEST_PASSWORD)
port=fixture.find_free_port()
argv=['qemu-system-x86_64','-M','q35','-m','2G','-smp','1','-cpu','max,+rdrand,+rdseed','-accel','tcg','-display','none','-monitor','none','-no-reboot','-serial','stdio','-boot','d','-cdrom',str(iso),'-drive',f'file={disk},if=none,id=target,format=raw','-device','nvme,drive=target,serial=SUDOSSH','-fw_cfg','name=opt/fortress/write_test,string=1','-netdev',f'user,id=net0,hostfwd=tcp:127.0.0.1:{port}-:22','-device','e1000,netdev=net0']
if firmware=='uefi':
    variables=out/'vars.fd';shutil.copyfile(fixture.VARS,variables)
    argv+=['-drive',f'if=pflash,format=raw,unit=0,readonly=on,file={fixture.CODE}','-drive',f'if=pflash,format=raw,unit=1,file={variables}']
assert disk.is_file() and disk.parent==out and not any('/dev/' in x for x in argv)
(out/'argv.json').write_text(json.dumps(argv,indent=2));print(out,flush=True)
proc=subprocess.Popen(argv,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=(out/'qemu.stderr').open('wb'))
sel=selectors.DefaultSelector();sel.register(proc.stdout,selectors.EVENT_READ);data=bytearray()
def wait(predicate,start=0,timeout=120):
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        for k,_ in sel.select(.1):
            chunk=k.fileobj.read1(65536)
            if chunk:data.extend(chunk)
        text=re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]','',data[start:].decode(errors='replace')).replace('\r','')
        (out/'serial.log').write_bytes(data)
        if predicate(text):return text
        assert proc.poll() is None,text[-2000:]
    raise TimeoutError(text[-2500:])
def send(s):proc.stdin.write((s+'\n').encode());proc.stdin.flush()
try:
    wait(lambda s:s.endswith('FortressOS login: '));send('operator')
    if mode!='passwordless-background':wait(lambda s:s.endswith('Password: '));send(fixture.TEST_PASSWORD)
    wait(lambda s:s.endswith('$ '))
    if mode=='detached-root':
        start=len(data);send('sudo -s');wait(lambda s:s.endswith('sudo password: '),start);send(fixture.TEST_PASSWORD)
        wait(lambda s:s.endswith('# '),start)
    if mode=='detached':
        start=len(data);send('sudo -b /sbin/sshd');wait(lambda s:s.endswith('sudo password: '),start);send('wrong-daemon-password')
        text=wait(lambda s:'authentication failed' in s and s.endswith('$ '),start)
        assert 'Listening on port' not in text and 'started background pid' not in text
    detached=mode.startswith('detached')
    start=len(data);send('sudo -b /sbin/sshd' if detached else 'sudo /sbin/sshd'+(' &' if mode!='foreground' else ''))
    if mode not in ('passwordless-background','detached-root'):
        wait(lambda s:'sudo password: ' in s,start)
        if mode in ('foreground','detached'):send(fixture.TEST_PASSWORD)
        else:
            text=wait(lambda s:'Stopped' in s or 'stopped' in s,start,15)
            print('OBSERVED background sudo stopped for terminal password input',flush=True)
            sys.exit(0)
    wait(lambda s:'Listening on port 22' in s,start,30)
    if detached:
        wait(lambda s:'started background pid' in s and s.endswith('# ' if mode=='detached-root' else '$ '),start)
    if mode=='detached-root':
        start=len(data);send('exit');wait(lambda s:s.endswith('$ '),start)
    scan=subprocess.run(['ssh-keyscan','-T','10','-t','ed25519','-p',str(port),'127.0.0.1'],capture_output=True,text=True,timeout=15)
    (out/'keyscan.stdout').write_text(scan.stdout);(out/'keyscan.stderr').write_text(scan.stderr)
    assert scan.returncode==0 and 'ssh-ed25519' in scan.stdout,scan.stderr
    print('PASS '+firmware+' '+mode+' direct sudo sshd launch and OpenSSH key exchange',flush=True)
finally:
    if proc.poll() is None:
        proc.terminate()
        try:proc.wait(timeout=5)
        except subprocess.TimeoutExpired:proc.kill();proc.wait(timeout=5)
    data.extend(proc.stdout.read());sel.close();(out/'serial.log').write_bytes(data)
