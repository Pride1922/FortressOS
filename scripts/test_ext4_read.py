"""Disposable ext4 read acceptance: BIOS/UEFI, actual NVMe/VFS and Ring 3 hashes."""
import hashlib
import json
from pathlib import Path
import selectors
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import uuid
import zlib
from test_net_pci import REPO, CODE, VARS


def gpt(image, target):
    payload=image.read_bytes()
    sectors=len(payload)//512+4096
    entries=bytearray(16384)
    struct.pack_into('<16s16sQQQ72s',entries,0,
        uuid.UUID('0fc63daf-8483-4772-8e79-3d69d8477de4').bytes_le,
        uuid.UUID('11223344-5566-7788-99aa-bbccddeeff00').bytes_le,
        2048,2048+len(payload)//512-1,0,b'')
    def header(current, alternate, array):
        h=bytearray(struct.pack('<8sIIIIQQQQ16sQIII',b'EFI PART',0x10000,92,0,0,
            current,alternate,34,sectors-34,uuid.UUID('a1b2c3d4-e5f6-7890-1234-56789abcdef0').bytes_le,
            array,128,128,zlib.crc32(entries)))
        struct.pack_into('<I',h,16,zlib.crc32(h))
        return h
    with target.open('xb') as f:
        f.truncate(sectors*512)
        mbr=bytearray(512)
        struct.pack_into('<B3sB3sII',mbr,446,0,b'\0\2\0',0xee,b'\xff'*3,1,sectors-1)
        mbr[510:]=b'\x55\xaa'
        for offset,data in [(0,mbr),(512,header(1,sectors-1,2)),(1024,entries),
                            (2048*512,payload),((sectors-33)*512,entries),
                            ((sectors-1)*512,header(sectors-1,1,sectors-33))]:
            f.seek(offset);f.write(data)


def command(mode, disk, variables):
    cmd=['qemu-system-x86_64','-M','q35','-m','2G','-accel','tcg','-smp','1',
         '-display','none','-monitor','none','-no-reboot','-boot','d',
         '-cdrom',str(REPO/'bin/fortress.iso'),'-serial','stdio','-net','none',
         '-drive',f'file={disk},if=none,id=e4,format=raw,readonly=on',
         '-device','nvme,drive=e4,serial=EXT4READ',
         '-fw_cfg','name=opt/fortress/ext4_read_test,string=1']
    if mode=='uefi':
        cmd+=['-drive',f'if=pflash,format=raw,unit=0,readonly=on,file={CODE}',
              '-drive',f'if=pflash,format=raw,unit=1,file={variables}']
    return cmd


def preflight(cmd,mode,disk,variables,out):
    assert mode in ('bios','uefi')
    assert cmd==command(mode,disk,variables), 'unexpected QEMU argv'
    assert disk.is_file() and disk.resolve().parent==out.resolve()
    assert variables.resolve().parent==out.resolve()


def run(mode,bs,fixture,out):
    label=f'{mode}-{bs}'
    disk=out/f'{label}.img';gpt(fixture/f'e4a-{bs}.img',disk)
    before=hashlib.sha256(disk.read_bytes()).hexdigest()
    variables=out/f'{label}-vars.fd'
    if mode=='uefi': shutil.copyfile(VARS,variables)
    cmd=command(mode,disk,variables)
    preflight(cmd,mode,disk,variables,out)
    for extra in (['-drive','file=/dev/sda'],['-blockdev','driver=host_device'],
                  ['-device','usb-storage,drive=unsafe']):
        try: preflight(cmd+extra,mode,disk,variables,out)
        except AssertionError: pass
        else: raise AssertionError('preflight accepted extra storage')
    (out/f'{label}-argv.json').write_text(json.dumps(cmd,indent=2)+'\n')
    transcript=bytearray()
    with (out/f'{label}-stderr.log').open('wb') as error:
        proc=subprocess.Popen(cmd,cwd=REPO,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=error)
        selector=selectors.DefaultSelector();selector.register(proc.stdout,selectors.EVENT_READ)
        def wait(predicate,timeout):
            deadline=time.monotonic()+timeout
            while time.monotonic()<deadline:
                assert proc.poll() is None, f'QEMU exited: {label}'
                for key,_ in selector.select(.1):
                    data=key.fileobj.read1(65536)
                    assert data, 'UART EOF'
                    transcript.extend(data)
                text=transcript.decode(errors='replace')
                assert 'PANIC' not in text and '[FAIL]' not in text, text[-4000:]
                if predicate(text): return
            raise AssertionError(f'{label} timeout: {transcript[-4000:]!r}')
        try:
            wait(lambda t:'[BOOT] Interactive shell ready.' in t and 'fortress:' in t,90)
            assert '[EXT4 READ] PASS exact bytes:' in transcript.decode(errors='replace')
            for name,expected in [('data.bin',hashlib.sha256(bytes(range(256))*4096).hexdigest()),
                                  ('fragmented.bin',hashlib.sha256((fixture/f'fragmented-{bs}.bin').read_bytes()).hexdigest()),
                                  ('unwritten.bin',hashlib.sha256(bytes(bs*8)).hexdigest())]:
                start=len(transcript)
                proc.stdin.write(f'sha256sum /mnt/{name}\n'.encode());proc.stdin.flush()
                wait(lambda t:expected in t[start:] and 'fortress:' in t[start:].split(expected)[-1],90)
            print(f'PASS {label}: full VFS byte audit, Ring 3 SHA-256 x3, sparse >4GiB, shell recovery',flush=True)
        finally:
            proc.terminate()
            try:proc.wait(timeout=3)
            except subprocess.TimeoutExpired:proc.kill();proc.wait()
            selector.close()
            (out/f'{label}.log').write_bytes(transcript)
    assert hashlib.sha256(disk.read_bytes()).hexdigest()==before, 'RO fixture changed'
    return {'case':label,'disk_sha256':before,'argv':cmd}


def main():
    result=subprocess.run([sys.executable,str(REPO/'scripts/create_ext4_fixtures.py')],
                          cwd=REPO,text=True,capture_output=True,check=True)
    print(result.stdout,end='',flush=True)
    fixture=Path(result.stdout.strip().split('Evidence: ')[-1])
    parent=REPO/'build/ext4-read';parent.mkdir(exist_ok=True)
    out=Path(tempfile.mkdtemp(prefix='run-',dir=parent))
    records=[]
    try:
        for bs in (1024,2048,4096):
            for mode in ('bios','uefi'):records.append(run(mode,bs,fixture,out))
    finally:
        (out/'manifest.json').write_text(json.dumps({'fixtures':str(fixture),'cases':records},indent=2)+'\n')
        print(f'Evidence: {out}',flush=True)
    print('EXT4 Phase 2 QEMU 6/6 PASS; disposable read-only NVMe, SMP=1; no USB/physical/write claim.')

if __name__=='__main__':main()
