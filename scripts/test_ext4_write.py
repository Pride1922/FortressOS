"""BIOS/UEFI three-boot ext4 persistence and Ring 3 HTTP downloads.
Only new disposable GPT/NVMe regular files, never device paths. Exact argv is
preflighted at every boot. Each guest is reaped and evidence survives failures.
"""
import concurrent.futures
import hashlib
import http.server
import json
from pathlib import Path
import selectors
import shutil
import struct
import subprocess
import tempfile
import threading
import time
from create_ext4_fixtures import ROOT,FEATURES,run
from test_ext4_read import gpt
from test_net_pci import CODE,VARS

PATTERN=bytes((i*17+3)&255 for i in range(256))
DATA1=PATTERN*4096
DATA16=PATTERN*65536
class HTTP(http.server.BaseHTTPRequestHandler):
    def log_message(self,*args):pass
    def do_GET(self):
        body=DATA1 if self.path=='/1m' else DATA16 if self.path=='/16m' else None
        if body is None:self.send_error(404);return
        self.send_response(200);self.send_header('Content-Length',str(len(body)));self.send_header('Connection','close');self.end_headers();self.wfile.write(body)

def command(mode,disk,variables,phase,iso):
    cmd=['qemu-system-x86_64','-M','q35','-m','2G','-accel','tcg','-smp','1',
         '-display','none','-monitor','none','-no-reboot','-boot','d','-cdrom',str(iso),'-serial','stdio',
         '-netdev','user,id=n','-device','e1000,netdev=n',
         '-drive',f'file={disk},if=none,id=e4,format=raw,snapshot=off','-device','nvme,drive=e4,serial=EXT4WRITE',
         '-fw_cfg','name=opt/fortress/ext4_write_test,string=1']
    if phase>1:cmd+=['-fw_cfg',f'name=opt/fortress/ext4_write_{"verify" if phase==2 else "cleanup"},string=1']
    if mode=='uefi':cmd+=['-drive',f'if=pflash,format=raw,unit=0,readonly=on,file={CODE}',
                         '-drive',f'if=pflash,format=raw,unit=1,file={variables}']
    return cmd

def preflight(cmd,mode,disk,variables,phase,iso,out):
    assert mode in ('bios','uefi') and phase in (1,2,3)
    assert cmd==command(mode,disk,variables,phase,iso),'unexpected QEMU argv'
    for path in (disk,variables,iso):assert path.resolve().parent==out.resolve()
    assert disk.is_file() and iso.is_file() and not str(disk).startswith('/dev/')

def boot(mode,disk,variables,phase,iso,out,port,bs,production=False):
    label=f'{mode}-{bs}-boot{phase}';cmd=command(mode,disk,variables,phase,iso)
    preflight(cmd,mode,disk,variables,phase,iso,out)
    for extra in (['-drive','file=/dev/sda'],['-blockdev','driver=host_device'],['-device','usb-storage,drive=unsafe']):
        try:preflight(cmd+extra,mode,disk,variables,phase,iso,out)
        except AssertionError:pass
        else:raise AssertionError('extra storage accepted')
    (out/f'{label}-argv.json').write_text(json.dumps(cmd,indent=2)+'\n')
    transcript=bytearray()
    with (out/f'{label}-stderr.log').open('wb') as error:
        proc=subprocess.Popen(cmd,cwd=ROOT,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=error)
        selector=selectors.DefaultSelector();selector.register(proc.stdout,selectors.EVENT_READ)
        def wait(pred,timeout=180):
            deadline=time.monotonic()+timeout
            while time.monotonic()<deadline:
                for key,_ in selector.select(.1):
                    data=key.fileobj.read1(65536)
                    if data:
                        transcript.extend(data)
                        (out/f'{label}.log').write_bytes(transcript)
                text=transcript.decode(errors='replace')
                if 'PANIC' in text or '[FAIL]' in text:
                    for _ in range(10):
                        for key,_ in selector.select(.1):
                            data=key.fileobj.read1(65536)
                            if data:transcript.extend(data)
                    raise AssertionError(transcript.decode(errors='replace')[-5000:])
                if pred(text):return
                assert proc.poll() is None,f'QEMU exited: {label}: {text[-5000:]}'
            raise AssertionError(f'{label} timeout: {transcript[-5000:]!r}')
        def shell(command,timeout=180):
            start=len(transcript);proc.stdin.write((command+'\n').encode());proc.stdin.flush()
            wait(lambda t:'fortress:' in t[start:] and ' $ ' in t[start:],timeout)
            return transcript[start:].decode(errors='replace')
        try:
            wait(lambda t:'[BOOT] Interactive shell ready.' in t and 'fortress:' in t,1200 if production else 240)
            assert f'[EXT4 WRITE] PASS boot {phase};' in transcript.decode(errors='replace')
            if phase==1:
                # Retry only the explicit kernel TCP reboot-quiet response.
                deadline=time.monotonic()+150
                while True:
                    text=shell(f'wget -O /mnt/download-1m.bin http://10.0.2.2:{port}/1m')
                    if 'write error' in text:raise AssertionError(text)
                    if 'saved [' in text and '1048576' in text:break
                    assert 'quiet' in text and time.monotonic()<deadline,text
                    time.sleep(5)
                text=shell(f'wget -O /mnt/download-16m.bin http://10.0.2.2:{port}/16m',1800 if production else 600)
                assert 'saved [' in text and '16777216' in text and 'write error' not in text,text
            for name,payload in [('saved.bin',DATA16),('download-1m.bin',DATA1),('download-16m.bin',DATA16)]:
                digest=hashlib.sha256(payload).hexdigest();text=shell(f'sha256sum /mnt/{name}',900 if production else 180);assert digest in text,text
            if production:
                text=shell('sync')
                assert 'Filesystem synced.' in text,text
                # Mutation after SYS_SYNC proves that sync did not freeze the mount.
                text=shell('echo still-writable > /mnt/sync-after.txt')
                assert 'error' not in text.lower() and 'failed' not in text.lower(),text
            else:
                assert '[EXT4 WRITE] SYNC PASS' in shell('echo sync > /ext4-test-control')
                assert '[EXT4 WRITE] FREEZE PASS' in shell('echo freeze > /ext4-test-control')
            proc.stdin.write(b'poweroff\n');proc.stdin.flush()
            proc.wait(timeout=30);assert proc.returncode==0
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:proc.wait(timeout=3)
                except subprocess.TimeoutExpired:proc.kill();proc.wait()
            selector.close();(out/f'{label}.log').write_bytes(transcript)
    return {'phase':phase,'argv':cmd,'disk_sha256':hashlib.sha256(disk.read_bytes()).hexdigest()}

def audit(disk,out,label,phase):
    # Extract only the partition of the disposable GPT image while QEMU is gone.
    with disk.open('rb') as f:
        f.seek(1024+32);first,last=struct.unpack('<QQ',f.read(16));f.seek(first*512);data=f.read((last-first+1)*512)
    assert struct.unpack_from('<H',data,1024+58)[0]==1,'freeze did not mark filesystem clean'
    image=out/f'{label}-phase{phase}.ext4';image.write_bytes(data)
    log=run(['e2fsck','-fn',str(image)])
    for name,payload in [('saved.bin',DATA16),('download-1m.bin',DATA1),('download-16m.bin',DATA16)]:
        target=out/f'{label}-phase{phase}-{name}';log+=run(['debugfs','-R',f'dump /{name} {target}',str(image)])
        assert target.read_bytes()==payload,f'Linux byte mismatch {name}'
    (out/f'{label}-phase{phase}-linux.log').write_text(log)

def case(mode,bs,out,port,iso):
    label=f'{mode}-{bs}';source=out/f'{label}.ext4'
    with source.open('xb') as f:f.truncate(64*1024*1024)
    mkfs=['mke2fs','-q','-t','ext4','-b',str(bs),'-I','256','-O',FEATURES,'-E','lazy_itable_init=0','-m','0','-g','4096','-N','512',str(source)]
    (out/f'{label}-mkfs.log').write_text(run(mkfs)+run(['dumpe2fs','-h',str(source)]))
    disk=out/f'{label}.img';gpt(source,disk);variables=out/f'{label}-vars.fd'
    if mode=='uefi':shutil.copyfile(VARS,variables)
    records=[]
    for phase in (1,2,3):
        records.append(boot(mode,disk,variables,phase,iso,out,port,bs));audit(disk,out,label,phase)
        print(f'PASS {label} boot {phase}: kernel/VFS, Ring 3 hashes, Linux bytes/fsck',flush=True)
    return {'case':label,'mkfs':mkfs,'boots':records}

def main():
    parent=ROOT/'build/ext4-write';parent.mkdir(exist_ok=True);out=Path(tempfile.mkdtemp(prefix='run-',dir=parent))
    (out/'versions.txt').write_text(run(['mke2fs','-V'])+run(['qemu-system-x86_64','--version']))
    iso=out/'fixture.iso';shutil.copyfile(ROOT/'bin/fortress.iso',iso)
    server=http.server.ThreadingHTTPServer(('127.0.0.1',0),HTTP);threading.Thread(target=server.serve_forever,daemon=True).start()
    records=[];errors=[]
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
            pending=[pool.submit(case,mode,bs,out,server.server_port,iso) for bs in (1024,2048,4096) for mode in ('bios','uefi')]
            for future in concurrent.futures.as_completed(pending):
                try:records.append(future.result())
                except Exception as error:
                    errors.append(str(error));print(f'FAIL: {error}',flush=True)
    finally:
        server.shutdown();server.server_close();(out/'manifest.json').write_text(json.dumps({'cases':records,'errors':errors},indent=2)+'\n');print(f'Evidence: {out}',flush=True)
    assert not errors,errors
    print('EXT4 Phase 4 BIOS/UEFI 6/6 cases (18 boots) PASS; disposable NVMe, SMP=1; no USB/physical/journal claim.')
if __name__=='__main__':main()
