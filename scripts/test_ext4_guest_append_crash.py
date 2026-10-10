"""Actual AP independent/shared append crashes, recovery before truncation."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import test_ext4_journal_mount as guest
from test_ext4_guest_crash import stop_at_write,writer_offsets,partition,REPO,fixture_source
from test_ext4_read import gpt
from test_jbd2_replay_host import oracle
from test_net_pci import VARS


def check_final(out,label):
    image=out/f'{label}-final-boot2.ext4'
    expected={f'W{wid:02d}:{seq:04d}:APPEND\n'.encode() for wid in range(2) for seq in range(100)}
    logs=[]
    for name in ('journal-app-independent.txt','journal-app-shared.txt'):
        dump=out/f'{label}-final-{name}'
        result=subprocess.run(['debugfs','-R',f'dump /{name} {dump}',str(image)],capture_output=True,check=True)
        logs.append((result.stdout+result.stderr).decode(errors='replace'))
        data=dump.read_bytes()
        assert len(data)==3200 and {data[k:k+16] for k in range(0,len(data),16)}==expected,name
    (out/f'{label}-final-append-linux.log').write_text(''.join(logs))


def check(disk,out,label,kind,record):
    image=out/f'{label}.ext4';partition(disk,image);linux=out/f'{label}.linux.ext4'
    oracle(image,linux,out/f'{label}-linux.log')
    name='journal-app-shared.txt' if kind else 'journal-app-independent.txt';dump=out/f'{label}.bin'
    subprocess.run(['debugfs','-R',f'dump /{name} {dump}',str(linux)],capture_output=True,check=True)
    assert dump.read_bytes()==record,'committed AP record mismatch'


def main():
    parser=argparse.ArgumentParser();parser.add_argument('workspace',type=Path);parser.add_argument('--usb',action='store_true');args=parser.parse_args()
    allowed=(REPO/'.codex-remote-attachments').resolve();workspace=args.workspace.resolve();assert workspace.is_relative_to(allowed)
    out=Path(tempfile.mkdtemp(prefix='guest-usb-append-crash-' if args.usb else 'guest-append-crash-',dir=allowed/'ext4-phase9'))
    iso=out/'fixture.iso';elf=out/'fortress.elf';shutil.copyfile(workspace/'bin/fortress.iso',iso);shutil.copyfile(workspace/'bin/fortress.elf',elf)
    records=[];errors=[];original=guest.command
    base_command=original
    if args.usb:
        from test_ext4_journal_usb import command as base_command
    try:
        offsets=writer_offsets(elf,out)
        for mode in ('bios','uefi'):
            for bs in (1024,2048,4096):
                for kind in (0,1):
                    label=f'{mode}-{bs}-'+('shared' if kind else 'independent');disk=out/f'{label}.img'
                    source=fixture_source(bs);gpt(source,disk)
                    milestone=stop_at_write(out,iso,elf,disk,offsets,'jbd2_writer_checkpoint',label+'-cut',3,mode,bs,4,append_kind=kind,usb=args.usb)
                    record=bytes.fromhex(milestone['append_record_hex']);check(disk,out,label+'-crash',kind,record)
                    stop_at_write(out,iso,elf,disk,offsets,'run_append_scenario',label+'-recovered',0,mode,bs,4,recover_append=True,usb=args.usb)
                    check(disk,out,label+'-guest-replay',kind,record)
                    variables=out/f'{label}-final-vars.fd'
                    if mode=='uefi':shutil.copyfile(VARS,variables)
                    def command(bootmode,bootdisk,bootvars,phase,bootiso):
                        cmd=base_command(bootmode,bootdisk,bootvars,phase,bootiso);cmd[cmd.index('-smp')+1]='4'
                        return cmd+['-fw_cfg','name=opt/fortress/ext4_journal_integration,string=1']
                    guest.command=command;guest.boot(mode,disk,variables,2,iso,out,label+'-final');guest.audit(disk,out,label+'-final',2)
                    transcript=(out/f'{label}-final-boot2.log').read_text();assert '[EXT4 INTEGRATION] SMP APPEND PASS' in transcript
                    check_final(out,label)
                    records.append({'label':label,'milestone':milestone});print(f'PASS AP crash/replay {label}',flush=True)
    except Exception as error:errors.append(repr(error));raise
    finally:
        guest.command=original;(out/'manifest.json').write_text(json.dumps({'cases':records,'errors':errors,'workspace':str(workspace),'transport':'usb-bot' if args.usb else 'nvme',
            'iso_sha256':hashlib.sha256(iso.read_bytes()).hexdigest(),'elf_sha256':hashlib.sha256(elf.read_bytes()).hexdigest()},indent=2)+'\n')
        print(f'Evidence: {out}',flush=True)


if __name__=='__main__':main()
