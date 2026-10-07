"""Exercise the capacity-matched physical artifact on owned emulated USB copies."""
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
import test_ext4_journal_mount as guest
import test_ext4_integration as integration
from test_ext4_read import gpt
from test_ext4_guest_crash import GuestRemote
from test_nmi_transitions import REPO,symbols
from test_net_pci import CODE,VARS
from test_ext4_usb_journal_fault import dma_layouts

def config(identity,token='start',rw='rw'):
    return f'timeout: 0\n\n/EXT4 9.6 DISPOSABLE\n    protocol: limine\n    kernel_path: boot():/boot/fortress.elf\n    module_path: boot():/boot/initramfs.tar\n    kernel_cmdline: ext4_physical={token} usb_data=PARTUUID={identity} usb_data_mode={rw}\n'

def put_config(disk,path,text):
    path.write_text(text)
    for name in ('::limine.conf','::boot/limine/limine.conf','::EFI/BOOT/limine.conf'):
        subprocess.run(['mcopy','-o','-i',str(disk)+'@@1048576',str(path),name],capture_output=True,check=True)

def command(mode,disk,variables,phase,iso):
    cmd=['qemu-system-x86_64','-M','q35','-m','2G','-accel','tcg','-smp','4','-display','none','-monitor','none','-no-reboot','-boot','c','-serial','stdio','-net','none',
         '-device','qemu-xhci,id=xhci,p2=4,p3=0','-drive',f'file={disk},if=none,id=e4,format=raw,cache=writeback','-device','usb-storage,drive=e4,bus=xhci.0,bootindex=1']
    if mode=='uefi':cmd+=['-drive',f'if=pflash,format=raw,unit=0,readonly=on,file={CODE}','-drive',f'if=pflash,format=raw,unit=1,file={variables}']
    return cmd

def partition(disk,out):
    with disk.open('rb') as stream:
        stream.seek(1184);start,end=struct.unpack('<QQ',stream.read(16));stream.seek(start*512);out.write_bytes(stream.read((end-start+1)*512))

def audit(disk,out,label,phase):
    payload=out/f'{label}-source.ext4';partition(disk,payload);view=out/f'{label}-view.img';gpt(payload,view)
    guest.audit(view,out,label,phase);integration.SMP=4;integration.audit(view,out,label,phase,base=False)

def reject(out,disk,iso,elf,mode,label,wanted):
    variables=out/f'{label}-vars.fd'
    if mode=='uefi':shutil.copyfile(VARS,variables)
    socketdir=Path(tempfile.mkdtemp(prefix='physical-reject-'));serial=out/f'{label}.serial.log'
    cmd=command(mode,disk,variables,1,iso);cmd[cmd.index('-serial')+1]='file:'+str(serial);cmd+=['-S','-gdb',f'unix:{socketdir/"gdb"},server=on,wait=off']
    # Malformed GPT controls must not depend on that GPT for bootloader loading.
    cmd[cmd.index('-boot')+1]='d'
    cmd[cmd.index('usb-storage,drive=e4,bus=xhci.0,bootindex=1')]='usb-storage,drive=e4,bus=xhci.0'
    cmd+=['-cdrom',str(iso)]
    (out/f'{label}-argv.json').write_text(json.dumps(cmd,indent=2)+'\n')
    sym=symbols(str(elf));before=out/f'{label}-before.ext4';partition(disk,before);process=None;remote=None
    with (out/f'{label}.stderr.log').open('wb') as errors:
        try:
            process=subprocess.Popen(cmd,stdout=subprocess.DEVNULL,stderr=errors)
            remote=GuestRemote(socketdir/'gdb');remote.sock.settimeout(180);remote.request('qSupported');remote.request('qXfer:features:read:target.xml:0,fff')
            for name in ('usb_block_write','usb_block_write_sectors'):remote.breakpoint(sym[name])
            if label.endswith('degraded'):
                # UEFI may reconstruct a damaged backup GPT while scanning USB.
                # Apply this control after firmware, before kernel USB probing.
                remote.resume_to(sym['xhci_boot_probe'])
                with disk.open('r+b') as stream:
                    stream.seek(512);header=stream.read(512);alternate=struct.unpack_from('<Q',header,32)[0]
                    stream.seek(alternate*512);stream.write(bytes(512));stream.flush()
            if label.endswith('ineligible-durability'):
                # Explicit debugger policy adapter, not a USB cache-probe failure.
                remote.resume_to(sym['test_ext4_journal_usb_mount'])
                remote.resume_to(sym['xhci_bot_get_durability_mode'])
                bot=remote.registers()[0][5];offset=dma_layouts(elf,out)['bot']['durability_mode']
                assert remote.request(f'M{bot+offset:x},4:04000000')=='OK'
            deadline=time.monotonic()+180
            for _ in range(2048):
                assert time.monotonic()<deadline
                remote.resume_to(sym['serial_puts']);pointer=remote.registers()[0][5]
                if remote.memory(pointer,len(wanted))==wanted:break
                assert remote.request('s').startswith(('T05','S05'))
            else:raise AssertionError('expected rejection not observed')
            assert not struct.unpack('<Q',remote.memory(sym['e4_active'],8))[0]
            process.kill();process.wait(timeout=10)
        finally:
            if remote:remote.sock.close()
            if process and process.poll() is None:process.kill();process.wait(timeout=10)
            (socketdir/'gdb').unlink(missing_ok=True);socketdir.rmdir()
    after=out/f'{label}-after.ext4';partition(disk,after);assert after.read_bytes()==before.read_bytes()
    print(f'PASS physical gate control {label}',flush=True)
    return {'label':label,'write_callback_hits':0,'published':False,'policy_adapter':label.endswith('ineligible-durability'),'filesystem_sha256':hashlib.sha256(after.read_bytes()).hexdigest()}

def main():
    parser=argparse.ArgumentParser();parser.add_argument('workspace',type=Path);parser.add_argument('ordinary_workspace',type=Path);parser.add_argument('--only-control',choices=('duplicate','degraded','ineligible-durability','no-opt-in','ordinary-build'));parser.add_argument('--persistence-only',action='store_true');parser.add_argument('--cycles',type=int,choices=(1,3),default=3);parser.add_argument('--verify-only',action='store_true');args=parser.parse_args()
    assert not (args.persistence_only and args.only_control), 'choose one focused scope'
    assert not args.verify_only or args.persistence_only, 'verify-only requires persistence-only'
    root=REPO/'.codex-remote-attachments/ext4-phase9';workspace=args.workspace.resolve();ordinary=args.ordinary_workspace.resolve()
    assert workspace.is_relative_to(root.resolve()) and ordinary.is_relative_to(root.resolve())
    info=json.loads((workspace/'physical-artifact/manifest.json').read_text());base=Path(info['image']);identity=info['data_partuuid']
    out=Path(tempfile.mkdtemp(prefix='physical-validation-',dir=root));iso=out/'fixture.iso';elf=out/'fortress.elf';shutil.copyfile(workspace/'bin/fortress.iso',iso);shutil.copyfile(workspace/'bin/fortress.elf',elf)
    rows=[];errors=[];original=guest.command;guest.command=command
    def copy_disk(label):
        disk=out/f'{label}.img';subprocess.run(['cp','--sparse=always',str(base),str(disk)],check=True);assert disk.parent==out
        # Also covers a kernel-only update to an existing physical fixture:
        # every owned copy boots the selected snapshot, never a stale base ELF.
        for name in ('fortress.elf','initramfs.tar'):
            subprocess.run(['mcopy','-o','-i',str(disk)+'@@1048576',str(workspace/'bin'/name),'::boot/'+name],capture_output=True,check=True)
        return disk
    try:
        for mode in ('bios','uefi'):
            label=mode+'-persistence';disk=copy_disk(label);variables=out/f'{label}-vars.fd'
            if mode=='uefi':shutil.copyfile(VARS,variables)
            for cycle,phase in (() if args.only_control else ((1,2),) if args.verify_only else ((1,1),) if args.cycles==1 else ((1,1),(2,2),(3,2))):
                put_config(disk,out/f'{label}.conf',config(identity,'start' if phase==1 else 'verify'))
                tag=f'{label}-cycle{cycle}';guest.boot(mode,disk,variables,phase,iso,out,tag);audit(disk,out,tag,phase)
                text=(out/f'{tag}-boot{phase}.log').read_text();assert '[EXT4 PHYSICAL] Authorized disposable fixture' in text and '[EXT4 INTEGRATION] SMP APPEND PASS' in text
                rows.append({'label':tag,'phase':phase});print(f'PASS physical image {tag}',flush=True)
            for profile in (() if args.persistence_only else (args.only_control,) if args.only_control else ('wrong-target','read-only','wrong-capacity','duplicate','degraded','no-opt-in','ordinary-build','ineligible-durability')):
                tag=f'{mode}-{profile}';disk=copy_disk(tag)
                put_config(disk,out/f'{tag}.conf',config(str(uuid.uuid4()) if profile=='wrong-target' else identity,'absent' if profile=='no-opt-in' else 'start','ro' if profile=='read-only' else 'rw'))
                wanted=b'[EXT4 PHYSICAL] REJECT target' if profile in ('wrong-target','wrong-capacity') else b'[EXT4 USB JOURNAL] REJECT admission;'
                if profile=='wrong-capacity':
                    with disk.open('r+b') as stream:stream.truncate(info['target']['capacity_bytes']+512)
                if profile in ('duplicate','degraded'):
                    with disk.open('r+b') as stream:
                        if profile=='degraded':
                            stream.seek(info['target']['capacity_bytes']-512);stream.write(bytes(512))
                        else:
                            stream.seek(512);primary=stream.read(512);count,size=struct.unpack_from('<II',primary,80);array=struct.unpack_from('<Q',primary,72)[0]
                            stream.seek(array*512);entries=bytearray(stream.read(count*size));assert size==128 and count>=3
                            entries[256:384]=entries[128:256];struct.pack_into('<QQ',entries,256+32,300000,300003)
                            for offset in (512,info['target']['capacity_bytes']-512):
                                stream.seek(offset);header=bytearray(stream.read(512));array=struct.unpack_from('<Q',header,72)[0]*512
                                assert struct.unpack_from('<II',header,80)==(count,size)
                                struct.pack_into('<I',header,88,zlib.crc32(entries));struct.pack_into('<I',header,16,0);struct.pack_into('<I',header,16,zlib.crc32(header[:92]));stream.seek(offset);stream.write(header);stream.seek(array);stream.write(entries)
                selected_elf=elf
                if profile=='ordinary-build':
                    wanted=b'[EXT4 PHYSICAL] REJECT disabled';selected_elf=ordinary/'bin/fortress.elf'
                    for target,file in (('::boot/fortress.elf','fortress.elf'),('::boot/initramfs.tar','initramfs.tar')):subprocess.run(['mcopy','-o','-i',str(disk)+'@@1048576',str(ordinary/'bin'/file),target],capture_output=True,check=True)
                if profile=='no-opt-in':wanted=b'[USB E4-A] FAIL: ext4 mount failed on '
                control_iso=out/f'{tag}.iso';source_iso=ordinary/'bin/fortress.iso' if profile=='ordinary-build' else iso
                subprocess.run(['xorriso','-indev',str(source_iso),'-outdev',str(control_iso),'-boot_image','any','replay','-map',str(out/f'{tag}.conf'),'/boot/limine/limine.conf','-map',str(out/f'{tag}.conf'),'/boot/limine.conf'],capture_output=True,check=True)
                subprocess.run([str(workspace/'limine/limine'),'bios-install',str(control_iso)],capture_output=True,check=True)
                rows.append(reject(out,disk,control_iso,selected_elf,mode,tag,wanted))
    except Exception as error:errors.append(repr(error));raise
    finally:
        guest.command=original;(out/'manifest.json').write_text(json.dumps({'cases':rows,'errors':errors,'workspace':str(workspace),'ordinary_workspace':str(ordinary),'physical_writes':False},indent=2)+'\n');print(f'Evidence: {out}',flush=True)

if __name__=='__main__':main()
