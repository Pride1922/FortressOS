"""Phase 8.6 combined journaled VFS lifecycle; disposable NVMe, BIOS/UEFI.
Reuse Phase-8.5 bounded UART, argv preflight, immutable ISO and PID cleanup.
Each invocation has a fixed SMP count; no shared mutable worker configuration.
"""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import test_ext4_journal_mount as guest
from create_ext4_fixtures import ROOT,run

SMP=int(os.environ.get('FORTRESS_EXT4_INTEGRATION_SMP','1'))
assert SMP in (1,4)
EVIDENCE=Path(os.environ.get('FORTRESS_EXT4_INTEGRATION_EVIDENCE',
    ROOT/'.codex-remote-attachments/ext4-phase8-6'))
original_command=guest.command
original_boot=guest.boot
original_audit=guest.audit

def command(mode,disk,variables,phase,iso):
    cmd=original_command(mode,disk,variables,phase,iso)
    cmd[cmd.index('-smp')+1]=str(SMP)
    return cmd+['-fw_cfg','name=opt/fortress/ext4_journal_integration,string=1']

def boot(mode,disk,variables,phase,iso,out,label):
    record=original_boot(mode,disk,variables,phase,iso,out,label)
    text=(out/f'{label}-boot{phase}.log').read_text(errors='replace')
    assert '[EXT4 INTEGRATION] namespace/truncate/pins/reuse PASS' in text
    if SMP==4:assert '[EXT4 INTEGRATION] SMP APPEND PASS' in text
    return record

def audit(disk,out,label,phase,base=True):
    if base:original_audit(disk,out,label,phase)
    image=out/f'{label}-boot{phase}.ext4';log=''
    for name,payload in [('integration-dir/persist.bin',guest.DATA[:1041]),('reuse.bin',guest.DATA[:8192])]:
        target=out/f'{label}-boot{phase}-{name.replace("/","-")}'
        log+=run(['debugfs','-R',f'dump /{name} {target}',str(image)])
        assert target.read_bytes()==payload,name
    for name in ('pinned.bin','integration.bin','empty-dir'):
        result=run(['debugfs','-R',f'stat /{name}',str(image)])
        assert 'File not found' in result;log+=result
    if SMP==4:
        expected={f'W{wid:02d}:{seq:04d}:APPEND\n'.encode() for wid in range(2) for seq in range(100)}
        for name in ('journal-app-independent.txt','journal-app-shared.txt'):
            target=out/f'{label}-boot{phase}-{name}'
            log+=run(['debugfs','-R',f'dump /{name} {target}',str(image)])
            data=target.read_bytes();assert len(data)==3200
            assert {data[k:k+16] for k in range(0,len(data),16)}==expected
    (out/f'{label}-boot{phase}-integration-linux.log').write_text(log)

def main():
    assert len(sys.argv) in (2,4),'usage: test_ext4_integration.py fixture-directory [firmware block-size]'
    fixtures=Path(sys.argv[1]).resolve()
    assert fixtures.is_relative_to((ROOT/'.codex-remote-attachments').resolve())
    guest.command=command;guest.boot=boot;guest.audit=audit
    guest.EVIDENCE=ROOT/'.codex-remote-attachments'
    EVIDENCE.mkdir(parents=True,exist_ok=True)
    out=Path(tempfile.mkdtemp(prefix=f'guest-smp{SMP}-',dir=EVIDENCE))
    iso=out/'fixture.iso';guest.snapshot_iso(ROOT/'bin/fortress.iso',iso)
    configurations=[(mode,bs) for bs in (1024,2048,4096) for mode in ('bios','uefi')]
    if len(sys.argv)==4:
        requested=(sys.argv[2],int(sys.argv[3]));assert requested in configurations
        configurations=[requested]
    records=[];errors=[]
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        pending=[pool.submit(guest.case,mode,bs,
            fixtures/f'{bs}-{"wrap" if bs==2048 else "normal"}-512-pending-seed.img',out,iso)
            for mode,bs in configurations]
        for future in concurrent.futures.as_completed(pending):
            try:records.append(future.result())
            except Exception as error:errors.append(str(error));print(f'FAIL {error}',flush=True)
    (out/'manifest.json').write_text(json.dumps({'smp':SMP,'configurations':configurations,'cases':records,'errors':errors,
        'iso_sha256':hashlib.sha256(iso.read_bytes()).hexdigest()},indent=2)+'\n')
    assert not errors,errors
    print(f'EXT4 Phase 8.6 QEMU PASS {len(records)}/{len(configurations)}, {2*len(records)} boots, SMP={SMP}; evidence: {out}')

if __name__=='__main__':main()
