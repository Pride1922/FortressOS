#!/usr/bin/env python3
"""Phase 5 production USB dispatch, SYS_SYNC, shutdown and SMP persistence.
Only disposable regular copies; no NVMe/device paths or physical acceptance.
"""
import concurrent.futures
import argparse
import hashlib
import http.server
import json
from pathlib import Path
import shutil
import struct
import tempfile
import threading
import test_ext4_write as writer
from create_ext4_fixtures import ROOT,run
from test_net_pci import CODE,VARS

SOURCE=ROOT/'bin/fortress-ext4-test.img'

def command(mode,disk,variables,phase,iso):
    cpus=4 if 'smp4' in disk.name else 1
    cmd=['qemu-system-x86_64','-M','q35','-m','2G','-accel','tcg','-smp',str(cpus),
         '-display','none','-monitor','none','-no-reboot','-boot','c','-serial','stdio',
         '-netdev','user,id=n','-device','e1000,netdev=n',
         '-device','qemu-xhci,id=xhci,p2=4,p3=0',
         '-drive',f'file={disk},if=none,id=e4,format=raw,snapshot=off',
         '-device','usb-storage,drive=e4,bootindex=1',
         '-fw_cfg','name=opt/fortress/ext4_usb_test,string=1']
    if phase>1:cmd+=['-fw_cfg',f'name=opt/fortress/ext4_write_{"verify" if phase==2 else "cleanup"},string=1']
    if cpus==4 and phase==1:cmd+=['-fw_cfg','name=opt/fortress/ext4_usb_append,string=1']
    if mode=='uefi':cmd+=['-drive',f'if=pflash,format=raw,unit=0,readonly=on,file={CODE}',
                         '-drive',f'if=pflash,format=raw,unit=1,file={variables}']
    return cmd

def preflight(cmd,mode,disk,variables,phase,iso,out):
    assert cmd==command(mode,disk,variables,phase,iso),'unexpected storage/QEMU argv'
    assert mode in ('bios','uefi') and phase in (1,2,3)
    for path in (disk,variables,iso):assert path.resolve().parent==out.resolve()
    assert disk.is_file() and iso.is_file() and not str(disk).startswith('/dev/')

def audit(disk,out,label,phase):
    with disk.open('rb') as f:
        f.seek(1024+128+32);first,last=struct.unpack('<QQ',f.read(16))
        f.seek(first*512);data=f.read((last-first+1)*512)
    assert struct.unpack_from('<H',data,1082)[0]==1,'shutdown did not mark clean'
    image=out/f'{label}-phase{phase}.ext4';image.write_bytes(data)
    log=run(['e2fsck','-fn',str(image)])
    for name,payload in [('saved.bin',writer.DATA16),('download-1m.bin',writer.DATA1),
                         ('download-16m.bin',writer.DATA16),('sync-after.txt',b'still-writable\n')]:
        target=out/f'{label}-phase{phase}-{name}'
        log+=run(['debugfs','-R',f'dump /{name} {target}',str(image)])
        assert target.read_bytes()==payload,name
    if 'smp4' in label:
        for name in ('smp_app_indep.txt','smp_app_shared.txt'):
            target=out/f'{label}-phase{phase}-{name}'
            log+=run(['debugfs','-R',f'dump /{name} {target}',str(image)])
            data=target.read_bytes();assert len(data)==3200
            records=[data[i:i+16] for i in range(0,len(data),16)]
            expected={f'W{w:02d}:{n:04d}:APPEND\n'.encode() for w in (0,1) for n in range(100)}
            assert len(set(records))==200 and set(records)==expected,name
            for w in (0,1):
                own=[int(r[4:8]) for r in records if r[1:3]==f'{w:02d}'.encode()]
                assert own==list(range(100)),name
    (out/f'{label}-phase{phase}-linux.log').write_text(log)

def case(mode,cpus,out,port,iso):
    label=f'{mode}-smp{cpus}';disk=out/f'{label}.img';shutil.copyfile(SOURCE,disk)
    variables=out/f'{label}-vars.fd'
    if mode=='uefi':shutil.copyfile(VARS,variables)
    records=[]
    for phase in (1,2,3):
        records.append(writer.boot(mode,disk,variables,phase,iso,out,port,label,production=True))
        text=(out/f'{mode}-{label}-boot{phase}.log').read_text(errors='replace')
        assert '[USB E4-A] Selected filesystem: ext4' in text
        assert 'durability=sync-backed' in text and 'Mount mode: read-write' in text
        if cpus==4:
            assert 'All 4 CPU(s) accounted for (1 BSP + 3 AP(s)), matching MADT' in text
            if phase==1:assert '[EXT4 USB] SMP APPEND PASS' in text
        else:assert 'Single-CPU system confirmed by both MADT and Limine' in text
        audit(disk,out,label,phase)
        print(f'PASS {label} boot {phase}: production USB, SYS_SYNC/shutdown, hashes/fsck',flush=True)
    return {'case':label,'boots':records}

def readonly(mode,out,degraded=False):
    from test_usb_persistence import configure_disposable_img_mode,run_qemu_session,send_command
    label=f'{"degraded-ro" if degraded else "ro"}-{mode}'
    disk=out/f'{label}.img';shutil.copyfile(SOURCE,disk)
    configure_disposable_img_mode(disk,writable=degraded)
    if degraded:
        with disk.open('r+b') as f:f.truncate(disk.stat().st_size+1024*1024)
    def partition_hash():
        with disk.open('rb') as f:
            f.seek(133120*512);return hashlib.sha256(f.read(64*1024*1024)).hexdigest()
    before=partition_hash();log=out/f'{label}.log'
    def action(qmp,child,path):
        text=path.read_text(errors='replace');assert '[USB E4-A] Selected filesystem: ext4' in text
        if degraded:assert 'GPT policy not strictly consistent; RW not eligible' in text
        reply=send_command(qmp,child,path,'cat /mnt/README.txt\n','fortress:/ $ ')
        assert 'Filesystem: ext4' in reply
        reply=send_command(qmp,child,path,'mkdir /mnt/forbidden\n','fortress:/ $ ')
        assert 'cannot create directory' in reply.lower(),reply
        reply=send_command(qmp,child,path,'ls /mnt\n','fortress:/ $ ')
        assert 'forbidden' not in reply,reply
        send_command(qmp,child,path,'poweroff\n',None)
    run_qemu_session(mode,disk,log,action,f'ext4-{label}',writable=False)
    assert partition_hash()==before,'RO filesystem changed'
    print(f'PASS EXT4 USB {label}: immutable partition, reads, write denial and shutdown',flush=True)
    return {'mode':mode,'degraded':degraded,'partition_sha256':before}

def smoke(mode,out):
    from test_usb_persistence import run_qemu_session,send_command
    disk=out/f'smoke-{mode}.img';shutil.copyfile(SOURCE,disk);log=out/f'smoke-{mode}.log'
    def action(qmp,child,path):
        assert '[USB E4-A] Selected filesystem: ext4' in path.read_text(errors='replace')
        send_command(qmp,child,path,'edit /mnt/smoke.txt\n','edit> ')
        send_command(qmp,child,path,'a\n','> ')
        send_command(qmp,child,path,'phase5 final image\n','> ')
        send_command(qmp,child,path,'.\n','edit> ')
        send_command(qmp,child,path,'w\n','Saved')
        send_command(qmp,child,path,'q\n','fortress:/ $ ')
        reply=send_command(qmp,child,path,'sync\n','fortress:/ $ ')
        assert 'Filesystem synced.' in reply
        reply=send_command(qmp,child,path,'mkdir /mnt/after-sync\n','fortress:/ $ ')
        assert 'cannot' not in reply.lower(),reply
        send_command(qmp,child,path,'poweroff\n',None)
    run_qemu_session(mode,disk,log,action,f'ext4-smoke-{mode}')
    with disk.open('rb') as f:f.seek(133120*512);data=f.read(64*1024*1024)
    assert struct.unpack_from('<H',data,1082)[0]==1
    image=out/f'smoke-{mode}.ext4';image.write_bytes(data)
    result=run(['e2fsck','-fn',str(image)])
    target=out/f'smoke-{mode}.txt';result+=run(['debugfs','-R',f'dump /smoke.txt {target}',str(image)])
    assert target.read_bytes()==b'phase5 final image\n'
    (out/f'smoke-{mode}-linux.log').write_text(result)
    print(f'PASS final-image smoke {mode}: actual EXT4 write, SYS_SYNC, later mutation, clean shutdown, Linux bytes/fsck',flush=True)
    return {'mode':mode,'disk_sha256':hashlib.sha256(disk.read_bytes()).hexdigest()}

def main():
    global SOURCE
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--smoke-only',action='store_true',help='verify the delivered test image with small real writes/sync/shutdown')
    args=parser.parse_args()
    parent=ROOT/'build/ext4-usb';parent.mkdir(exist_ok=True)
    evidence=Path(tempfile.mkdtemp(prefix='run-',dir=parent))
    out=Path(tempfile.mkdtemp(prefix='fortress-ext4-usb-'))
    print(f'Native workbench: {out}; retained evidence: {evidence}',flush=True)
    SOURCE=out/'source.img'
    if args.smoke_only:shutil.copyfile(ROOT/'bin/fortress-ext4-test.img',SOURCE)
    else:run(['python3','scripts/create_boot_img.py',str(SOURCE),'--filesystem','ext4'])
    iso=SOURCE
    (out/'versions.txt').write_text(run(['mke2fs','-V'])+run(['qemu-system-x86_64','--version']))
    (out/'source-sha256.txt').write_text(hashlib.sha256(SOURCE.read_bytes()).hexdigest()+'\n')
    if args.smoke_only:
        records=[]
        try:
            for mode in ('bios','uefi'):records.append(smoke(mode,out))
        finally:
            (out/'smoke-manifest.json').write_text(json.dumps(records,indent=2)+'\n')
            shutil.copytree(out,evidence,dirs_exist_ok=True);print(f'Evidence: {evidence}',flush=True)
        assert len(records)==2
        return
    writer.command=command;writer.preflight=preflight
    server=http.server.ThreadingHTTPServer(('127.0.0.1',0),writer.HTTP)
    threading.Thread(target=server.serve_forever,daemon=True).start();records=[];errors=[];ro=[]
    try:
        for degraded in (False,True):
            for mode in ('bios','uefi'):ro.append(readonly(mode,out,degraded))
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            pending=[pool.submit(case,mode,cpus,out,server.server_port,iso) for cpus in (1,4) for mode in ('bios','uefi')]
            for future in concurrent.futures.as_completed(pending):
                try:records.append(future.result())
                except Exception as error:errors.append(str(error));print(f'FAIL {error}',flush=True)
    except Exception as error:
        errors.append(str(error));raise
    finally:
        server.shutdown();server.server_close()
        (out/'manifest.json').write_text(json.dumps({'cases':records,'read_only':ro,'errors':errors},indent=2)+'\n')
        shutil.copytree(out,evidence,dirs_exist_ok=True)
        print(f'Evidence: {evidence}',flush=True)
    assert not errors,errors
    assert len(records)==4 and len(ro)==4
    print('EXT4 production USB BIOS/UEFI SMP=1/4: 4/4 cases, 12 boots PASS; no physical/journal claim.')
if __name__=='__main__':main()
