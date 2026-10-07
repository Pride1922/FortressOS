"""Native journal USB admission controls; rejected filesystems remain immutable."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import time
import uuid
import zlib
from test_ext4_journal_usb import command
from test_ext4_guest_crash import GuestRemote,partition
from test_ext4_read import gpt
from test_nmi_transitions import REPO,symbols
from test_net_pci import VARS


def alter_gpt(disk,profile):
    data=bytearray(disk.read_bytes());sectors=len(data)//512
    entries=bytearray(data[1024:17408])
    if profile=='wrong-guid':entries[16:32]=uuid.UUID('ffeeddcc-bbaa-9988-7766-554433221100').bytes_le
    if profile=='duplicate':
        entries[128:256]=entries[:128];struct.pack_into('<QQ',entries,128+32,1024,1027)
    copies=[(512,1024),(len(data)-512,(sectors-33)*512)]
    for header,array in copies:
        current=bytearray(entries)
        if profile=='conflicting' and header!=512:current[16:32]=uuid.UUID('ffeeddcc-bbaa-9988-7766-554433221100').bytes_le
        data[array:array+16384]=current
        struct.pack_into('<I',data,header+88,zlib.crc32(current));struct.pack_into('<I',data,header+16,0)
        struct.pack_into('<I',data,header+16,zlib.crc32(data[header:header+92]))
    if profile=='degraded':data.extend(bytes(1024*1024))
    disk.write_bytes(data)


def main():
    parser=argparse.ArgumentParser();parser.add_argument('workspace',type=Path);args=parser.parse_args()
    root=REPO/'.codex-remote-attachments/ext4-phase9';workspace=args.workspace.resolve();assert workspace.is_relative_to(root.resolve())
    out=Path(tempfile.mkdtemp(prefix='guest-usb-admission-',dir=root));iso=out/'fixture.iso';elf=out/'fortress.elf'
    shutil.copyfile(workspace/'bin/fortress.iso',iso);shutil.copyfile(workspace/'bin/fortress.elf',elf)
    ro=out/'read-only.iso';config=out/'read-only.conf'
    config.write_text((workspace/'limine.conf').read_text().replace('usb_data_mode=rw','usb_data_mode=ro'))
    with (out/'iso-recipe.log').open('wb') as log:
        subprocess.run(['xorriso','-indev',str(iso),'-outdev',str(ro),'-boot_image','any','replay',
            '-map',str(config),'/boot/limine/limine.conf','-map',str(config),'/boot/limine.conf'],stdout=log,stderr=log,check=True)
        subprocess.run([str(workspace/'limine/limine'),'bios-install',str(ro)],stdout=log,stderr=log,check=True)
    records=[];errors=[];sym=symbols(str(elf))
    try:
        for mode in ('bios','uefi'):
            for profile in ('read-only','wrong-guid','duplicate','degraded','conflicting'):
                label=f'{mode}-{profile}';disk=out/f'{label}.img'
                source=REPO/'.codex-remote-attachments/ext4-phase8-5/host-frz8r6nu/1024-normal-512-pending-seed.img';gpt(source,disk)
                if profile!='read-only':alter_gpt(disk,profile)
                before=out/f'{label}-before.ext4';partition(disk,before)
                variables=out/f'{label}-vars.fd'
                if mode=='uefi':shutil.copyfile(VARS,variables)
                sockets=Path(tempfile.mkdtemp(prefix='fortress-usb-admit-'));bootiso=ro if profile=='read-only' else iso
                cmd=command(mode,disk,variables,1,bootiso)
                cmd[cmd.index('-serial')+1]=f'file:{out/(label+".serial.log")}'
                cmd+=['-S','-gdb',f'unix:{sockets/"gdb"},server=on,wait=off']
                assert sum(value=='-drive' for value in cmd)==(3 if mode=='uefi' else 1)
                assert '-blockdev' not in cmd and '-snapshot' not in cmd
                (out/f'{label}-argv.json').write_text(json.dumps(cmd,indent=2)+'\n')
                process=None;remote=None
                with (out/f'{label}.stderr.log').open('wb') as log:
                    try:
                        process=subprocess.Popen(cmd,stdout=subprocess.DEVNULL,stderr=log,cwd=REPO)
                        remote=GuestRemote(sockets/'gdb');remote.sock.settimeout(180)
                        remote.request('qSupported');remote.request('qXfer:features:read:target.xml:0,fff')
                        for name in ('usb_block_write','usb_block_write_sectors'):remote.breakpoint(sym[name])
                        wanted=b'[EXT4 USB JOURNAL] REJECT admission;';deadline=time.monotonic()+180
                        for _ in range(2048):
                            assert time.monotonic()<deadline
                            remote.resume_to(sym['serial_puts']);pointer=remote.registers()[0][5]
                            if remote.memory(pointer,len(wanted))==wanted:break
                            assert remote.request('s').startswith(('T05','S05'))
                        else:raise AssertionError('expected zero-write rejection missed')
                        assert not struct.unpack('<Q',remote.memory(sym['e4_active'],8))[0]
                        record={'label':label,'published':False,'write_callback_hits':0,'stop':remote.last_stop}
                        process.kill();process.wait(timeout=10)
                    finally:
                        if remote:remote.sock.close()
                        if process and process.poll() is None:process.kill();process.wait(timeout=10)
                        (sockets/'gdb').unlink(missing_ok=True);sockets.rmdir()
                after=out/f'{label}-after.ext4';partition(disk,after);assert after.read_bytes()==before.read_bytes()
                record['partition_sha256']=hashlib.sha256(after.read_bytes()).hexdigest();records.append(record)
                print(f'PASS zero-write USB admission {label}',flush=True)
    except Exception as exception:errors.append(repr(exception));raise
    finally:
        (out/'manifest.json').write_text(json.dumps({'cases':records,'errors':errors,'workspace':str(workspace),
            'iso_sha256':hashlib.sha256(iso.read_bytes()).hexdigest(),'ro_iso_sha256':hashlib.sha256(ro.read_bytes()).hexdigest(),
            'elf_sha256':hashlib.sha256(elf.read_bytes()).hexdigest()},indent=2)+'\n');print(f'Evidence: {out}',flush=True)


if __name__=='__main__':main()
