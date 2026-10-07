"""Crash a durable open-unlink with live descriptors; fresh guest reclaims it."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
from test_ext4_guest_crash import stop_at_write,writer_offsets,partition,REPO
from test_ext4_read import gpt
from test_jbd2_replay_host import oracle
import test_ext4_journal_mount as guest
from test_net_pci import VARS


def check(disk,out,label,dirty=False):
    image=out/f'{label}.ext4';partition(disk,image)
    data=image.read_bytes()
    if dirty:assert struct.unpack_from('<I',data,1256)[0] or struct.unpack_from('<I',data,1120)[0]&4
    linux=out/f'{label}.linux.ext4';oracle(image,linux,out/f'{label}-linux.log')
    result=subprocess.run(['debugfs','-R','stat /pinned.bin',str(linux)],capture_output=True,check=True)
    assert b'File not found' in result.stdout+result.stderr
    dump=out/f'{label}.bin'
    result=subprocess.run(['debugfs','-R',f'dump /integration-dir/persist.bin {dump}',str(linux)],capture_output=True,check=True)
    assert dump.read_bytes()==guest.DATA[:1041]
    (out/f'{label}-namespace.log').write_bytes(result.stdout+result.stderr)


def main():
    parser=argparse.ArgumentParser();parser.add_argument('workspace',type=Path);parser.add_argument('--usb',action='store_true');args=parser.parse_args()
    allowed=(REPO/'.codex-remote-attachments').resolve();workspace=args.workspace.resolve();assert workspace.is_relative_to(allowed)
    out=Path(tempfile.mkdtemp(prefix='guest-usb-orphan-crash-' if args.usb else 'guest-orphan-crash-',dir=allowed/'ext4-phase9'))
    iso=out/'fixture.iso';elf=out/'fortress.elf';shutil.copyfile(workspace/'bin/fortress.iso',iso);shutil.copyfile(workspace/'bin/fortress.elf',elf)
    records=[];errors=[];original=guest.command
    base_command=original
    if args.usb:
        from test_ext4_journal_usb import command as base_command
    try:
        offsets=writer_offsets(elf,out)
        configurations=[(mode,bs,smp) for mode in ('bios','uefi') for bs in (1024,2048,4096) for smp in ((1,4) if args.usb else (1,))]
        for mode,bs,smp in configurations:
                label=f'{mode}-{bs}'+(f'-smp{smp}' if args.usb else '');disk=out/f'{label}.img'
                source=allowed/f'ext4-phase8-5/host-frz8r6nu/{bs}-normal-512-pending-seed.img';gpt(source,disk)
                cut=stop_at_write(out,iso,elf,disk,offsets,'jbd2_writer_checkpoint',label+'-cut',3,mode,bs,smp,orphan=True,usb=args.usb)
                check(disk,out,label+'-crash',True)
                stop_at_write(out,iso,elf,disk,offsets,'vfs_lookup',label+'-recovered',0,mode,bs,smp,recover_mount=True,usb=args.usb)
                check(disk,out,label+'-guest-replay')
                variables=out/f'{label}-final-vars.fd'
                if mode=='uefi':shutil.copyfile(VARS,variables)
                def command(bootmode,bootdisk,bootvars,phase,bootiso):
                    cmd=base_command(bootmode,bootdisk,bootvars,phase,bootiso);cmd[cmd.index('-smp')+1]=str(smp);return cmd
                guest.command=command
                guest.boot(mode,disk,variables,2,iso,out,label+'-final');guest.audit(disk,out,label+'-final',2)
                check(disk,out,label+'-final')
                records.append({'label':label,'milestone':cut});print(f'PASS orphan crash/replay {label}',flush=True)
    except Exception as error:errors.append(repr(error));raise
    finally:
        guest.command=original
        (out/'manifest.json').write_text(json.dumps({'cases':records,'errors':errors,'workspace':str(workspace),
            'iso_sha256':hashlib.sha256(iso.read_bytes()).hexdigest(),'elf_sha256':hashlib.sha256(elf.read_bytes()).hexdigest()},indent=2)+'\n')
        print(f'Evidence: {out}',flush=True)


if __name__=='__main__':main()
