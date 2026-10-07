"""Explicit journaled USB fixture; isolated ISO, sole disposable BOT data disk."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tempfile
import time
import test_ext4_journal_mount as guest
import test_ext4_integration as integration
from test_ext4_read import gpt
from test_net_pci import REPO,CODE,VARS

SMP=1
INTEGRATION=False


def command(mode,disk,variables,phase,iso):
    cmd=['qemu-system-x86_64','-M','q35','-m','2G','-accel','tcg','-smp',str(SMP),
         '-display','none','-monitor','none','-no-reboot','-boot','d','-cdrom',str(iso),
         '-serial','stdio','-net','none','-device','qemu-xhci,id=xhci,p2=4,p3=0',
         '-drive',f'file={disk},if=none,id=e4,format=raw,cache=writeback',
         '-device','usb-storage,drive=e4,id=journalusb,bus=xhci.0',
         '-fw_cfg','name=opt/fortress/ext4_journal_usb_test,string=1']
    if phase==2:cmd+=['-fw_cfg','name=opt/fortress/ext4_journal_verify,string=1']
    if INTEGRATION:cmd+=['-fw_cfg','name=opt/fortress/ext4_journal_integration,string=1']
    if mode=='uefi':cmd+=['-drive',f'if=pflash,format=raw,unit=0,readonly=on,file={CODE}',
                         '-drive',f'if=pflash,format=raw,unit=1,file={variables}']
    return cmd


def main():
    global SMP,INTEGRATION
    parser=argparse.ArgumentParser();parser.add_argument('workspace',type=Path);parser.add_argument('--matrix',action='store_true');parser.add_argument('--integration',action='store_true');args=parser.parse_args()
    root=(REPO/'.codex-remote-attachments/ext4-phase9').resolve();workspace=args.workspace.resolve();assert workspace.is_relative_to(root)
    manifest=json.loads((workspace/'workspace-manifest.json').read_text())
    assert 'limine.conf' in manifest['source_overrides'],'prepare with --usb-journal'
    out=Path(tempfile.mkdtemp(prefix='guest-journal-usb-',dir=root));records=[];errors=[]
    iso=out/'fixture.iso';elf=out/'fortress.elf'
    shutil.copyfile(workspace/'bin/fortress.iso',iso);shutil.copyfile(workspace/'bin/fortress.elf',elf)
    original=guest.command;guest.command=command;INTEGRATION=args.integration
    try:
        configurations=[(mode,bs,smp) for mode in ('bios','uefi') for bs in (1024,2048,4096) for smp in (1,4)] if args.matrix else [('bios',1024,1)]
        for mode,bs,smp in configurations:
            SMP=smp;label=f'{mode}-{bs}-smp{smp}';disk=out/f'{label}.img'
            source=REPO/f'.codex-remote-attachments/ext4-phase8-5/host-frz8r6nu/{bs}-{"wrap" if bs==2048 else "normal"}-512-pending-seed.img';gpt(source,disk)
            variables=out/f'{label}-vars.fd'
            if mode=='uefi':shutil.copyfile(VARS,variables)
            boots=[];started=time.monotonic()
            for number,phase in enumerate((1,2,2),1):
                bootlabel=f'{label}-cycle{number}'
                removed=('journal-later.bin','ring-later.bin') if number==3 else ()
                boots.append(guest.boot(mode,disk,variables,phase,iso,out,bootlabel,removed=removed));guest.audit(disk,out,bootlabel,phase,removed=removed)
                text=(out/f'{bootlabel}-boot{phase}.log').read_text()
                assert '[ext4 usb journal] selected sda partuuid=11223344-5566-7788-99aa-bbccddeeff00, durability=sync-backed' in text.lower()
                assert '[USB 9G.2] PASS: Registered block device "sda"' in text
                if INTEGRATION:
                    integration.SMP=smp;integration.audit(disk,out,bootlabel,phase,base=False)
                    assert '[EXT4 INTEGRATION] namespace/truncate/pins/reuse PASS' in text
                    if smp==4:assert '[EXT4 INTEGRATION] SMP APPEND PASS' in text
                print(f'PASS USB journal {label} cycle {number}',flush=True)
            records.append({'label':label,'boots':boots,'seconds':time.monotonic()-started,'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest()})
    except Exception as error:errors.append(repr(error));raise
    finally:
        guest.command=original;(out/'manifest.json').write_text(json.dumps({'cases':records,'errors':errors,
            'workspace':str(workspace),'integration':INTEGRATION,'iso_sha256':hashlib.sha256(iso.read_bytes()).hexdigest(),
            'elf_sha256':hashlib.sha256(elf.read_bytes()).hexdigest()},indent=2)+'\n')
        print(f'Evidence: {out}',flush=True)


if __name__=='__main__':main()
