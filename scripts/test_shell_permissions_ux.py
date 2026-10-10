"""Finite BIOS/UEFI normal-login shell UX gate; ISO only, no data disks."""
from pathlib import Path
import hashlib,json,re,selectors,shutil,subprocess,time
root=Path(__file__).resolve().parent.parent
out=root/'build/shell-ux';workspace=Path((out/'simple-workspace.txt').read_text().strip())
results=[]
for mode in ('bios','uefi'):
    cmd=['qemu-system-x86_64','-M','q35','-m','2G','-smp','1','-accel','tcg',
         '-display','none','-monitor','none','-no-reboot','-serial','stdio','-net','none',
         '-cdrom',str(workspace/'bin/fortress.iso')]
    if mode=='uefi':
        variables=out/'ux-vars.fd';shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
        cmd+=['-drive','if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
              '-drive',f'if=pflash,format=raw,unit=1,file={variables}']
    assert not any('/dev/' in v or 'nvme' in v for v in cmd)
    (out/f'ux-{mode}-argv.json').write_text(json.dumps(cmd,indent=2))
    transcript=bytearray();stderr=(out/f'ux-{mode}.stderr').open('wb')
    proc=subprocess.Popen(cmd,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=stderr)
    selector=selectors.DefaultSelector();selector.register(proc.stdout,selectors.EVENT_READ)
    def clean(data):return re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]','',data.decode(errors='replace')).replace('\r','')
    def wait(predicate,start=0):
        deadline=time.monotonic()+90
        while time.monotonic()<deadline:
            for key,_ in selector.select(.1):
                data=key.fileobj.read1(65536)
                if data:transcript.extend(data)
            text=clean(transcript[start:])
            if predicate(text):return text
            assert proc.poll() is None,text[-2000:]
        raise TimeoutError(clean(transcript[-3000:]))
    def send(command):proc.stdin.write((command+'\n').encode());proc.stdin.flush()
    def execute(command,expected,role='$'):
        start=len(transcript);send(command)
        return wait(lambda t:expected in t and t.endswith(role+' '),start)
    try:
        wait(lambda t:'FortressOS login: ' in t);start=len(transcript);send('operator')
        wait(lambda t:t.endswith('$ '),start)
        assert b'\x1b[2J\x1b[H' in transcript[start:]
        execute('shutdown','Permission denied.')
        execute('/bin/mkdir /run/sudo-ux','permission denied:')
        execute('sudo mkdir /run/sudo-ux','WARNING:')
        execute('sudo rm /run/sudo-ux','WARNING:')
        execute('sudo /bin/shell','# ',role='#')
        execute('id','uid=0 gid=0',role='#')
        execute('exit','$ ')
        execute('id','uid=1000 gid=1000')
        execute('cat /etc/shadow','permission denied:')
        send('sudo shutdown');proc.wait(timeout=90);assert proc.returncode==0
        results.append({'firmware':mode,'result':'PASS'})
        print('PASS',mode,'user/root prompt, exit restoration, denial, direct sudo mkdir/rm/shutdown',flush=True)
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:proc.wait(timeout=5)
            except subprocess.TimeoutExpired:proc.kill();proc.wait(timeout=5)
        selector.close();stderr.close();(out/f'ux-{mode}.log').write_bytes(transcript)
        (out/'ux-manifest.json').write_text(json.dumps({'cases':results,'workspace':str(workspace),
            'iso_sha256':hashlib.sha256((workspace/'bin/fortress.iso').read_bytes()).hexdigest(),
            'scope':'normal login ISO only; no data disks or physical I/O'},indent=2))
