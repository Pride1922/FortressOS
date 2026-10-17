#!/usr/bin/env python3
"""Disk-free isolated fstat-probe acceptance. Never stage it in release media."""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import hashlib
import json
import re
import shutil
import socket
import subprocess
import tempfile
import threading
import time
import tarfile
from test_getrandom import CODE,VARS,PROMPT

ROOT=Path(__file__).resolve().parent.parent
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def case(out,work,firmware,smp):
    dest=out/(firmware+'-smp'+str(smp));dest.mkdir()
    with tempfile.TemporaryDirectory(prefix='fortress-fstat-') as tmp:
        sockpath=Path(tmp)/'uart'
        cpu='max'
        cmd=['qemu-system-x86_64','-M','q35','-accel','tcg','-cpu',cpu,'-m','512M','-smp',str(smp),
             '-display','none','-net','none','-no-reboot','-boot','d','-cdrom',str(work/'bin/fortress.iso'),
             '-chardev',f'socket,id=uart,path={sockpath},server=on,wait=off,logfile={dest/"serial.log"}',
             '-serial','chardev:uart']
        if firmware=='uefi':
            assert CODE.is_file() and VARS.is_file()
            shutil.copyfile(VARS,dest/'vars.fd')
            cmd+=['-drive',f'if=pflash,format=raw,unit=0,readonly=on,file={CODE}',
                  '-drive',f'if=pflash,format=raw,unit=1,file={dest/"vars.fd"}']
        # Final command is constructed here; only optical media and pflash exist.
        assert not any(p in cmd for p in ('-hda','-hdb','-blockdev','-readconfig'))
        for i,c in enumerate(cmd):
            if c=='-drive':assert cmd[i+1].startswith('if=pflash,format=raw,unit=')
        (dest/'argv.json').write_text(json.dumps(cmd,indent=2))
        with (dest/'qemu.stderr').open('w') as err:
            p=subprocess.Popen(cmd,cwd=work,stderr=err)
            uart=socket.socket(socket.AF_UNIX);stop=threading.Event();thread=None
            def text():
                path=dest/'serial.log'
                s=path.read_text(errors='replace') if path.exists() else ''
                return re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]','',s).replace('\r','')
            def wait(pred,timeout=90):
                deadline=time.monotonic()+timeout
                while time.monotonic()<deadline:
                    s=text()
                    if pred(s):return s
                    assert p.poll() is None,('QEMU exited',p.returncode)
                    time.sleep(.05)
                raise AssertionError('timeout: '+text()[-3000:])
            try:
                deadline=time.monotonic()+10
                while True:
                    try:uart.connect(str(sockpath));break
                    except (FileNotFoundError,ConnectionRefusedError):
                        assert time.monotonic()<deadline;time.sleep(.05)
                uart.settimeout(.2)
                def drain():
                    while not stop.is_set():
                        try:
                            if not uart.recv(65536):return
                        except socket.timeout:pass
                        except OSError:return
                thread=threading.Thread(target=drain,daemon=True);thread.start()
                wait(lambda s:'FortressOS login: ' in s or PROMPT.search(s))
                if not PROMPT.search(text()):uart.sendall(b'operator\n')
                wait(lambda s:PROMPT.search(s))
                def command(c,marker):
                    pos=len(text());uart.sendall(c.encode()+b'\n')
                    return wait(lambda s:marker in s[pos:] and PROMPT.search(s[pos:]))[pos:]
                for path,kind in (('/etc/motd',1),('/bin/hello',1),('/bin',2)):
                    s=command('/bin/fstat-probe '+path,'FSTAT probe PASS')
                    assert 'FSTAT FAIL' not in s
                    match=re.search(r'FSTAT type=(\d+) size=(\d+) mode=(0[0-7]+) uid=(\d+) gid=(\d+)',s)
                    assert match and int(match[1])==kind,s
                    assert int(match[4])==0 and int(match[5])==0,s
                    if kind==1:
                        with tarfile.open(work/'bin/initramfs.tar') as archive:
                            member=next(m for m in archive.getmembers() if m.name.lstrip('./')==path.lstrip('/'))
                            assert int(match[2])==member.size,s
                            assert int(match[3],8)==(member.mode|0o100000),s
                command('/bin/fstat-probe /tmp/does-not-exist','FSTAT open failed')
                s=command('/bin/fstat-probe --selftest','FSTAT selftest PASS')
                assert 'FSTAT FAIL' not in s,s
                s=command('/bin/fstat-probe --workers','FSTAT workers PASS')
                assert s.count('FSTAT selftest PASS')==8 and 'FSTAT FAIL' not in s,s
                cpus=set(re.findall(r'cpu=(\d+)',s))
                assert smp==1 or len(cpus)>1,('workers did not show multiple CPUs',cpus)
                s=command('ls -l /etc/motd','-rw-r--r-- root root ')
                assert re.search(r'(?m)^-rw-r--r-- root root [0-9]+ /etc/motd$',s),s
                uart.sendall(b'sudo shutdown\n');p.wait(timeout=20);assert p.returncode==0
                result={'case':dest.name,'status':'PASS'}
                (dest/'result.json').write_text(json.dumps(result));print(json.dumps(result),flush=True)
                return result
            except BaseException as exc:
                (dest/'failure.txt').write_text(repr(exc));raise
            finally:
                stop.set();uart.close()
                if thread:thread.join(timeout=1)
                if p.poll() is None:
                    p.terminate()
                    try:p.wait(timeout=5)
                    except subprocess.TimeoutExpired:p.kill();p.wait(timeout=5)
def main():
    out=ROOT/'.codex-remote-attachments'/('fstat-qemu-'+time.strftime('%Y%m%d-%H%M%S'));out.mkdir(parents=True)
    work=out/'workspace'
    ignore=shutil.ignore_patterns('.git','.codex-remote-attachments','build','bin','dist','.VSCodeCounter','__pycache__','*.pyc','*.iso','*.img')
    shutil.copytree(ROOT,work,ignore=ignore)
    # Runtime dependencies are copied, not symlinked, so the isolated builder
    # cannot write through a symlink into the user's source/dependency tree.
    assert (work/'limine/limine').is_file()
    sources={str(p.relative_to(ROOT)):sha(p) for prefix in ('src','user','tests') for p in (ROOT/prefix).rglob('*') if p.is_file()}
    media={str(p.relative_to(ROOT)):sha(p) for p in (ROOT/'bin').glob('*') if p.is_file()}
    manifest={'source_sha256':sources,'release_media_sha256':media}
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2))
    try:
        with (out/'build.log').open('w') as log:
            subprocess.run(['make','-j8','iso','FSTAT_TEST_PROBE=1'],cwd=work,stdout=log,stderr=subprocess.STDOUT,check=True)
        with tarfile.open(work/'bin/initramfs.tar') as t:
            assert any(m.name.endswith('/bin/fstat-probe') or m.name=='bin/fstat-probe' for m in t.getmembers())
        manifest['test_iso_sha256']=sha(work/'bin/fortress.iso')
        results=[]
        with ThreadPoolExecutor(max_workers=2) as pool:
            futures=[pool.submit(case,out,work,f,a) for f in ('bios','uefi') for a in (1,4,8)]
            for future in futures:results.append(future.result())
        assert all(sha(ROOT/p)==h for p,h in sources.items()),'source changed during campaign'
        assert all(sha(ROOT/p)==h for p,h in media.items()),'release media changed during isolated test'
        (out/'summary.json').write_text(json.dumps(results,indent=2))
        (out/'manifest.json').write_text(json.dumps(manifest,indent=2))
        print(f'PASS 6/6 isolated fstat boots; source/release preservation; artifacts: {out}',flush=True)
    finally:print(f'Artifacts retained: {out}',flush=True)
if __name__=='__main__':main()
