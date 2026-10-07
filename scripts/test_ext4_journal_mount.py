"""Phase 8.5 disposable journaled NVMe VFS/SYS_SYNC/shutdown acceptance."""
import concurrent.futures
import hashlib
import json
from pathlib import Path
import selectors
import shutil
import struct
import subprocess
import tempfile
import time
from test_ext4_read import gpt
from test_net_pci import REPO, CODE, VARS
from create_ext4_fixtures import run
from test_jbd2_replay_host import blocks,be,crc
from test_ext4_mount_host import EVIDENCE

DATA=bytes((k*17+3)&255 for k in range(16384))

def snapshot_iso(source,target):
    # Another chat can rebuild this shared output. Take a stable, complete
    # ISO snapshot before starting any guest; never boot a changing bin file.
    deadline=time.monotonic()+30
    while time.monotonic()<deadline:
        try:
            before=source.stat();data=source.read_bytes();after=source.stat()
            pvd=data[16*2048:17*2048]
            if before.st_mtime_ns==after.st_mtime_ns and before.st_size==after.st_size==len(data) and \
                len(pvd)==2048 and pvd[1:6]==b'CD001' and len(data)>=struct.unpack_from('<I',pvd,80)[0]*2048:
                target.write_bytes(data);return
        except FileNotFoundError:pass
        time.sleep(.2)
    raise AssertionError('shared build ISO never became stable and complete')

def command(mode,disk,variables,phase,iso):
    cmd=['qemu-system-x86_64','-M','q35','-m','2G','-accel','tcg','-smp','1',
         '-display','none','-monitor','none','-no-reboot','-boot','d',
         '-cdrom',str(iso),'-serial','stdio','-net','none',
         '-drive',f'file={disk},if=none,id=e4,format=raw',
         '-device','nvme,drive=e4,serial=EXT4JOURNAL',
         '-fw_cfg','name=opt/fortress/ext4_journal_test,string=1']
    if phase==2:cmd+=['-fw_cfg','name=opt/fortress/ext4_journal_verify,string=1']
    if mode=='uefi':cmd+=['-drive',f'if=pflash,format=raw,unit=0,readonly=on,file={CODE}',
                          '-drive',f'if=pflash,format=raw,unit=1,file={variables}']
    return cmd

def preflight(cmd,mode,disk,variables,phase,iso,out):
    assert mode in ('bios','uefi') and phase in (1,2)
    assert cmd==command(mode,disk,variables,phase,iso),'unexpected QEMU argv'
    assert disk.is_file() and iso.is_file()
    assert all(p.resolve().parent==out.resolve() for p in (disk,variables,iso))

def boot(mode,disk,variables,phase,iso,out,label,removed=()):
    cmd=command(mode,disk,variables,phase,iso);preflight(cmd,mode,disk,variables,phase,iso,out)
    for extra in (['-drive','file=/dev/sda'],['-blockdev','driver=host_device'],['-snapshot']):
        try:preflight(cmd+extra,mode,disk,variables,phase,iso,out)
        except AssertionError:pass
        else:raise AssertionError('extra storage accepted')
    (out/f'{label}-boot{phase}-argv.json').write_text(json.dumps(cmd,indent=2)+'\n')
    transcript=bytearray()
    with (out/f'{label}-boot{phase}-stderr.log').open('wb') as error:
        proc=subprocess.Popen(cmd,cwd=REPO,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=error)
        selector=selectors.DefaultSelector();selector.register(proc.stdout,selectors.EVENT_READ)
        def wait(predicate,timeout=30):
            deadline=time.monotonic()+timeout
            while time.monotonic()<deadline:
                for key,_ in selector.select(.1):
                    data=key.fileobj.read1(65536)
                    if data:transcript.extend(data)
                text=transcript.decode(errors='replace')
                assert 'PANIC' not in text and '[FAIL]' not in text,text[-4000:]
                if predicate(text):return
                assert proc.poll() is None,f'QEMU exited: {label}: {text[-2000:]}'
            raise AssertionError(f'{label} timeout: {transcript[-4000:]!r}')
        def shell(text):
            start=len(transcript);proc.stdin.write((text+'\n').encode());proc.stdin.flush()
            wait(lambda t:('fortress:' in t[start:] and ' $ ' in t[start:]) or 'fortress> ' in t[start:])
            return transcript[start:].decode(errors='replace')
        try:
            wait(lambda t:'[BOOT] Interactive shell ready.' in t and ('fortress:' in t or 'fortress> ' in t),180)
            assert f'[EXT4 JOURNAL] PASS boot {phase};' in transcript.decode(errors='replace')
            assert 'Filesystem synced.' in shell('sync')
            text=shell('echo post-sync > /mnt/ring-later.bin')
            assert 'error' not in text.lower() and 'failed' not in text.lower(),text
            assert 'Filesystem synced.' in shell('sync')
            for name,payload in [('journal-persist.bin',DATA),('journal-later.bin',DATA[:17])]:
                assert hashlib.sha256(payload).hexdigest() in shell(f'sha256sum /mnt/{name}')
            for name in removed:
                assert name in ('journal-later.bin','ring-later.bin'),'undeclared cleanup'
                reply=shell(f'rm /mnt/{name}')
                assert 'error' not in reply.lower() and 'failed' not in reply.lower(),reply
            if removed:assert 'Filesystem synced.' in shell('sync')
            proc.stdin.write(b'shutdown\n');proc.stdin.flush()
            proc.wait(timeout=30);assert proc.returncode==0
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:proc.wait(timeout=3)
                except subprocess.TimeoutExpired:proc.kill();proc.wait()
            # Drain the final shutdown diagnostics after the process exits.
            transcript.extend(proc.stdout.read());selector.close()
            (out/f'{label}-boot{phase}.log').write_bytes(transcript)
    return {'phase':phase,'argv':cmd,'disk_sha256':hashlib.sha256(disk.read_bytes()).hexdigest()}

def audit(disk,out,label,phase,removed=()):
    with disk.open('rb') as f:
        f.seek(1056);first,last=struct.unpack('<QQ',f.read(16));f.seek(first*512);data=f.read((last-first+1)*512)
    assert struct.unpack_from('<H',data,1082)[0]==1
    assert struct.unpack_from('<I',data,1120)[0]==0x42
    assert struct.unpack_from('<I',data,1256)[0]==0
    image=out/f'{label}-boot{phase}.ext4';image.write_bytes(data)
    bs=1024<<struct.unpack_from('<I',data,1048)[0]
    assert struct.unpack_from('<I',data,2044)[0]==crc(0xffffffff,data[1024:2044])
    journal=blocks(image,f'<{struct.unpack_from("<I",data,1248)[0]}>')
    js=data[journal[0]*bs:journal[0]*bs+1024]
    assert not be(js,28),'clean shutdown left a nonempty journal'
    checked=bytearray(js);stored=be(js,252);struct.pack_into('>I',checked,252,0)
    assert stored==crc(0xffffffff,checked),'journal superblock checksum'
    log=run(['e2fsck','-fn',str(image)])
    for name,payload in [('journal-persist.bin',DATA),('journal-later.bin',DATA[:17]),('ring-later.bin',b'post-sync\n')]:
        if name in removed:
            result=run(['debugfs','-R',f'stat /{name}',str(image)]);assert 'File not found' in result,name;log+=result;continue
        target=out/f'{label}-boot{phase}-{name}'
        target.unlink(missing_ok=True)
        log+=run(['debugfs','-R',f'dump /{name} {target}',str(image)])
        assert target.read_bytes()==payload,f'Linux byte mismatch {name}'
    log+=run(['debugfs','-R','stat /target.bin',str(image)])
    assert 'File not found' in log, 'recovered orphan still reachable'
    (out/f'{label}-boot{phase}-linux.log').write_text(log)

def case(mode,bs,source,out,iso):
    assert source.is_file() and source.stat().st_size==32*1024*1024
    assert source.resolve().is_relative_to(EVIDENCE.resolve())
    label=f'{mode}-{bs}';disk=out/f'{label}.img';gpt(source,disk)
    variables=out/f'{label}-vars.fd'
    if mode=='uefi':shutil.copyfile(VARS,variables)
    records=[]
    for phase in (1,2):
        records.append(boot(mode,disk,variables,phase,iso,out,label));audit(disk,out,label,phase)
        print(f'PASS {label} boot {phase}: VFS/SYS_SYNC/later write/shutdown/Linux fsck and bytes',flush=True)
    return {'case':label,'source':str(source),'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'boots':records}

def main():
    parent=EVIDENCE
    candidates=sorted(parent.glob('host-*'),key=lambda p:p.stat().st_mtime,reverse=True)
    names=('1024-normal-512','2048-wrap-512','4096-normal-512')
    complete=next((p for p in candidates if all((p/f'{name}-pending-seed.img').is_file() and
        (p/f'{name}.log').is_file() and 'EXT4 mount host PASS' in (p/f'{name}.log').read_text()
        for name in names)),None)
    assert complete,'Run make test-ext4-mount-host first (selected complete disposable host profiles required)'
    out=Path(tempfile.mkdtemp(prefix='guest-',dir=parent));iso=out/'fixture.iso';snapshot_iso(REPO/'bin/fortress.iso',iso)
    records=[];errors=[]
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            pending=[]
            for bs in (1024,2048,4096):
                source=complete/f'{bs}-{"wrap" if bs==2048 else "normal"}-512-pending-seed.img'
                for mode in ('bios','uefi'):pending.append(pool.submit(case,mode,bs,source,out,iso))
            for future in concurrent.futures.as_completed(pending):
                try:records.append(future.result())
                except Exception as error:errors.append(str(error));print(f'FAIL {error}',flush=True)
    finally:
        (out/'manifest.json').write_text(json.dumps({'host_fixture_directory':str(complete),'iso_sha256':hashlib.sha256(iso.read_bytes()).hexdigest(),'cases':records,'errors':errors},indent=2)+'\n')
        print(f'Evidence: {out}',flush=True)
    assert not errors,errors
    print('EXT4 Phase 8.5 BIOS/UEFI 6/6 cases, 12 boots PASS; SMP=1, disposable NVMe; production journaled RW remains disabled.')
if __name__=='__main__':main()
