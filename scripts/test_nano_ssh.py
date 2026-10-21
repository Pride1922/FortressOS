"""Nano SSH viewport/resize/input/save regression using real OpenSSH and QEMU."""
import fcntl,hashlib,json,os,pty,re,select,selectors,shutil,struct,subprocess,sys,tempfile,termios,time,tty
from pathlib import Path
sys.path.insert(0,str(Path('scripts').resolve()))
import test_ssh as fixture
firmware=sys.argv[1] if len(sys.argv)>1 else 'bios'
assert firmware in ('bios','uefi')
parent=Path('build/nano-ssh').resolve();parent.mkdir(exist_ok=True)
out=Path(tempfile.mkdtemp(prefix=firmware+'-',dir=parent))
key=out/'client';subprocess.run(['ssh-keygen','-q','-t','ed25519','-N','','-f',str(key)],check=True)
disk=out/'fixture.img';fixture.create_nvme_fixture(disk,key.with_suffix('.pub').read_text())
iso=out/'fixture.iso'
fixture.build_test_iso(iso,fixture.TEST_PASSWORD)
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
def send(s):
    for byte in (s+'\n').encode():
        proc.stdin.write(bytes([byte]));proc.stdin.flush();time.sleep(.005)
client=None;master=None
remote=bytearray()
def remote_wait(predicate,timeout=20):
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        ready,_,_=select.select([master],[],[],.1)
        if ready:
            chunk=os.read(master,65536)
            if chunk:remote.extend(chunk)
        (out/'ssh.raw').write_bytes(remote)
        if predicate(bytes(remote)):return bytes(remote)
        assert client.poll() is None,remote[-1500:]
    raise TimeoutError(remote[-2500:])
def frame(data):
    start=data.rfind(b'\x1b[?25l\x1b[H')
    if start<0:return None
    end=data.find(b'\x1b[?25h',start)
    return data[start:end+6] if end>=0 else None
def check_frame(data,rows,cols,marker):
    latest=frame(data);assert latest is not None
    lines=latest.split(b'\r\n')
    assert len(lines)==rows,(len(lines),rows)
    plain=[re.sub(rb'\x1b\[[0-9;?]*[ -/]*[@-~]',b'',line) for line in lines]
    assert b'FortressOS Nano' in plain[0],plain[0]
    assert marker in plain[1],plain[1]
    assert b'\x1b[97m'+marker in lines[1],lines[1]
    assert b'^O Save' in plain[-1],plain[-1]
    assert all(len(line)<=cols for line in plain),[len(line) for line in plain]
    (out/f'screen-{cols}x{rows}.txt').write_bytes(b'\n'.join(plain))
try:
    wait(lambda s:s.endswith('FortressOS login: '));send('operator')
    wait(lambda s:s.endswith('Password: '));send(fixture.TEST_PASSWORD)
    wait(lambda s:s.endswith('$ '))

    import socket
    def is_port_listening(p):
        try:
            with socket.create_connection(('127.0.0.1', p), timeout=0.5):
                return True
        except Exception:
            return False

    if not is_port_listening(port):
        start=len(data);send('sudo -b /sbin/sshd');wait(lambda s:s.endswith('sudo password: '),start);send(fixture.TEST_PASSWORD)
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline and not is_port_listening(port):
            time.sleep(0.5)
        assert is_port_listening(port), "sshd failed to listen on port"
    master,slave=pty.openpty();tty.setraw(slave)
    fcntl.ioctl(slave,termios.TIOCSWINSZ,struct.pack('HHHH',30,100,0,0))
    def setup_client():
        os.setsid();fcntl.ioctl(slave,termios.TIOCSCTTY,0)
    ssh=['ssh','-tt','-q','-o','StrictHostKeyChecking=no','-o','UserKnownHostsFile=/dev/null','-o','PubkeyAuthentication=no','-o','ConnectTimeout=10','-p',str(port),'operator@127.0.0.1']
    (out/'ssh-argv.json').write_text(json.dumps(ssh,indent=2))
    client=subprocess.Popen(ssh,stdin=slave,stdout=slave,stderr=slave,preexec_fn=setup_client,close_fds=True);os.close(slave)
    remote_wait(lambda s:b'password: ' in s)
    os.write(master,(fixture.TEST_PASSWORD+'\n').encode())
    remote_wait(lambda s:b'$ ' in s)
    os.write(master,b'/bin/nano /run/user/1000/nano-ssh.txt\n')
    remote_wait(lambda s:frame(s) is not None)
    marker=b'NanoSSHVisible';os.write(master,marker)
    remote_wait(lambda s:frame(s) is not None and marker in frame(s))
    check_frame(bytes(remote),30,100,marker)
    fcntl.ioctl(master,termios.TIOCSWINSZ,struct.pack('HHHH',18,60,0,0))
    remote_wait(lambda s:frame(s) is not None and len(frame(s).split(b'\r\n'))==18)
    check_frame(bytes(remote),18,60,marker)
    os.write(master,b'\x0f');remote_wait(lambda s:b'File Name to Write:' in s)
    os.write(master,b'\n');remote_wait(lambda s:b'Wrote' in s)
    os.write(master,b'\x18')
    remote_wait(lambda s:re.sub(rb'\x1b\[[0-9;?]*[ -/]*[@-~]',b'',s).replace(b'\r',b'').endswith(b'$ '))
    saved_start=len(remote)
    os.write(master,b'sha256sum /run/user/1000/nano-ssh.txt\n')
    expected=hashlib.sha256(marker+b'\n').hexdigest().encode()
    remote_wait(lambda s:expected in s[saved_start:])
    (out/'saved-sha256.txt').write_bytes(expected+b'\n')
    print('PASS '+firmware+' SSH nano visible text, PTY resize, narrow frame and saved-file SHA-256',flush=True)
finally:
    if client is not None and client.poll() is None:
        client.terminate()
        try:client.wait(timeout=5)
        except subprocess.TimeoutExpired:client.kill();client.wait(timeout=5)
    if master is not None:os.close(master)
    if proc.poll() is None:
        proc.terminate()
        try:proc.wait(timeout=5)
        except subprocess.TimeoutExpired:proc.kill();proc.wait(timeout=5)
    data.extend(proc.stdout.read());sel.close();(out/'serial.log').write_bytes(data)
