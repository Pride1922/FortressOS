"""Bounded 9.4 guest crash/recovery matrix with independent Linux audits."""
import argparse
import functools
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import time
from test_ext4_read import gpt
import test_ext4_journal_mount as guest
from test_ext4_journal_mount import DATA, boot, audit
from test_net_pci import CODE, VARS
from test_jbd2_replay_host import oracle
from test_nmi_transitions import Remote, symbols, REPO


class GuestRemote(Remote):
    def resume_to(self,addr):
        self.breakpoint(addr);response=self.request('c')
        assert response.startswith(('T05','S05')),response
        thread=re.search(r'thread:([^;]+)',response)
        if thread:assert self.request('Hg'+thread[1])=='OK'
        assert self.registers()[0][16]==addr,'unexpected debugger stop'
        self.breakpoint(addr,False);self.last_stop=response


def validate_storage(cmd,mode,disk,iso,variables=None,usb=False):
    assert '-snapshot' not in cmd and '-blockdev' not in cmd
    assert '-hda' not in cmd and '-hdb' not in cmd and '-sd' not in cmd
    drives=[cmd[k+1] for k,arg in enumerate(cmd) if arg=='-drive']
    expected=[f'file={disk},if=none,id=e4,format=raw,cache=writeback']
    if mode=='uefi':
        expected += [f'if=pflash,format=raw,unit=0,readonly=on,file={CODE}',
                     f'if=pflash,format=raw,unit=1,file={variables}']
    assert mode in ('bios','uefi') and drives==expected,'unexpected storage'
    devices=['qemu-xhci,id=xhci,p2=4,p3=0','usb-storage,drive=e4,id=journalusb,bus=xhci.0'] if usb else ['nvme,drive=e4,serial=EXT4JOURNAL']
    assert [cmd[k+1] for k,arg in enumerate(cmd) if arg=='-device']==devices
    assert [cmd[k+1] for k,arg in enumerate(cmd) if arg=='-cdrom']==[str(iso)]
    assert disk.resolve().parent==iso.resolve().parent and disk.is_file() and iso.is_file()
    assert disk.resolve().is_relative_to((REPO/'.codex-remote-attachments').resolve())


def writer_offsets(elf,out):
    result=subprocess.run(['readelf','--debug-dump=info',str(elf)],capture_output=True,text=True,check=True)
    (out/'debug-layout-warnings.log').write_text(result.stderr)
    offsets=None
    for match in re.finditer(r'DW_AT_name[^\n]*: jbd2_writer\n',result.stdout):
        section=result.stdout[match.end():];section=section.split(' <1><',1)[0]
        found={}
        for member in section.split('(DW_TAG_member)')[1:]:
            name=re.search(r'DW_AT_name\s*:\s*(?:\([^\n]*\): )?(\w+)',member)
            location=re.search(r'DW_AT_data_member_location:\s*(\d+)',member)
            if name and location:found[name[1]]=int(location[1])
        if all(k in found for k in ('state','data_count','metadata_count','sequence')):offsets=found;break
    assert offsets,'missing complete writer DWARF layout'
    (out/'writer-layout.json').write_text(json.dumps(offsets,indent=2)+'\n');return offsets


def stop_at_write(out,iso,elf,disk,offsets,symbol,label,state,mode='bios',bs=1024,smp=1,middle=False,recovery=False,append_kind=None,recover_append=False,orphan=False,recover_mount=False,usb=False):
    socketdir=Path(tempfile.mkdtemp(prefix='fortress-e4-guest-'));debug=socketdir/'gdb'
    cmd=['qemu-system-x86_64','-M','q35','-m','2G','-accel','tcg','-smp',str(smp),'-S',
        '-display','none','-monitor','none','-no-reboot','-boot','d','-cdrom',str(iso),
        '-serial',f'file:{out/(label+".serial.log")}', '-net','none',
        '-drive',f'file={disk},if=none,id=e4,format=raw,cache=writeback',
        '-device','nvme,drive=e4,serial=EXT4JOURNAL',
        '-fw_cfg','name=opt/fortress/ext4_journal_test,string=1','-gdb',f'unix:{debug},server=on,wait=off']
    if usb:
        index=cmd.index('-device');cmd[index:index+2]=['-device','qemu-xhci,id=xhci,p2=4,p3=0','-device','usb-storage,drive=e4,id=journalusb,bus=xhci.0']
        cmd[cmd.index('-fw_cfg')+1]='name=opt/fortress/ext4_journal_usb_test,string=1'
    assert disk.parent==iso.parent==elf.parent==out and disk.is_file()
    variables=None
    if mode=='uefi':
        variables=out/f'{label}-vars.fd';shutil.copyfile(VARS,variables)
        cmd+=['-drive',f'if=pflash,format=raw,unit=0,readonly=on,file={CODE}',
            '-drive',f'if=pflash,format=raw,unit=1,file={variables}']
    if append_kind is not None or recover_append or orphan:
        cmd+=['-fw_cfg','name=opt/fortress/ext4_journal_integration,string=1']
    if recover_append:cmd+=['-fw_cfg','name=opt/fortress/ext4_journal_verify,string=1']
    validate_storage(cmd,mode,disk,iso,variables,usb)
    for extra in (['-drive','file=/dev/sda'],['-blockdev','driver=host_device'],['-snapshot'],['-hda','/dev/sda']):
        try:validate_storage(cmd+extra,mode,disk,iso,variables,usb)
        except AssertionError:pass
        else:raise AssertionError('extra storage accepted')
    (out/f'{label}-argv.json').write_text(json.dumps(cmd,indent=2)+'\n')
    sym=symbols(str(elf));address=sym[symbol];remote=None;hits=[];proc=None
    with (out/f'{label}.stderr.log').open('wb') as error:
        try:
            proc=subprocess.Popen(cmd,cwd=REPO,stdout=subprocess.DEVNULL,stderr=error)
            remote=GuestRemote(debug);remote.sock.settimeout(180);remote.request('qSupported')
            remote.request('qXfer:features:read:target.xml:0,fff')
            if recover_mount or orphan:
                wanted=b'/mnt/target.bin\0' if recover_mount else b'/mnt/pinned.bin\0'
                for _ in range(128):
                    remote.resume_to(sym['vfs_lookup' if recover_mount else 'vfs_unlink'])
                    pointer=remote.registers()[0][5]
                    if remote.memory(pointer,len(wanted))==wanted:break
                    assert remote.request('s').startswith(('T05','S05'))
                else:raise AssertionError('namespace milestone missed')
                assert struct.unpack('<Q',remote.memory(sym['e4_active'],8))[0]
                if recover_mount:
                    fields={'recovered_before_fixture_mutation':True,'rip':remote.registers()[0][16]}
                    (out/f'{label}-milestone.json').write_text(json.dumps(fields)+'\n')
                    proc.kill();proc.wait(timeout=10);return fields
            if recover_append:
                remote.resume_to(sym['run_append_scenario'])
                assert struct.unpack('<Q',remote.memory(sym['e4_active'],8))[0]
                fields={'recovered_before_append_truncation':True,'rip':remote.registers()[0][16]}
                (out/f'{label}-milestone.json').write_text(json.dumps(fields)+'\n')
                proc.kill();proc.wait(timeout=10);return fields
            if append_kind is not None:
                for scenario in range(append_kind+1):
                    remote.resume_to(sym['run_append_scenario']);assert remote.registers()[0][4]==scenario
                    assert remote.request('s').startswith(('T05','S05'))
            if recovery:
                remote.resume_to(sym['jbd2_replay']);plan=remote.registers()[0][5];completed=0
                for _ in range(128):
                    remote.resume_to(sym['j_io']);regs=remote.registers()[0]
                    if regs[5]==plan and regs[2]:
                        completed+=1
                        if completed==2:break
                    assert remote.request('s').startswith(('T05','S05'))
                else:raise AssertionError('recovery checkpoint milestone missed')
                assert not struct.unpack('<Q',remote.memory(sym['e4_active'],8))[0]
                fields={'replay_plan':plan,'home_images_completed':1,'rip':regs[16],'published':False}
                (out/f'{label}-milestone.json').write_text(json.dumps(fields,indent=2)+'\n')
                proc.kill();proc.wait(timeout=10);return fields
            deadline=time.monotonic()+180
            for _ in range(256):
                assert time.monotonic()<deadline and proc.poll() is None
                remote.resume_to(address);registers=remote.registers()[0];writer=registers[5]
                fields={key:struct.unpack('<I',remote.memory(writer+offsets[key],4))[0]
                    for key in ('state','data_count','metadata_count','sequence')}
                fields.update(writer=writer,rip=registers[16],mounted=bool(struct.unpack('<Q',remote.memory(sym['e4_active'],8))[0]))
                hits.append(fields)
                selected=fields['mounted'] and (fields['metadata_count'] if orphan else fields['data_count'])
                if selected and append_kind is not None:
                    pointer=struct.unpack('<Q',remote.memory(writer+offsets['data']+8,8))[0]
                    record=remote.memory(pointer,16);selected=record.startswith((b'W00:',b'W01:'))
                    if selected:
                        thread=re.search(r'thread:([^;]+)',remote.last_stop);assert thread and int(thread[1].split('.')[-1],16)>1,remote.last_stop
                        fields.update(append_record_hex=record.hex(),debugger_thread=thread[1],debugger_stop=remote.last_stop,shared=bool(append_kind))
                if selected:
                    assert fields['state']==state and fields['data_count']==(0 if orphan else 1 if append_kind is not None else 16384//bs)
                    break
                assert remote.request('s').startswith(('T05','S05'))
            else:raise AssertionError('write milestone never reached')
            if middle:
                io=struct.unpack('<Q',remote.memory(writer,8))[0];checkpoint_hits=0
                for _ in range(128):
                    remote.resume_to(sym['j_io']);regs=remote.registers()[0]
                    now=struct.unpack('<I',remote.memory(writer+offsets['state'],4))[0]
                    if regs[5]==io and regs[2] and now==4:
                        checkpoint_hits+=1
                        if checkpoint_hits==2:fields.update(state=now,rip=regs[16],checkpoint_images_completed=1);break
                    assert remote.request('s').startswith(('T05','S05'))
                else:raise AssertionError('middle checkpoint milestone missed')
            (out/f'{label}-milestone.json').write_text(json.dumps({'symbol':symbol,'middle':middle,'hits':hits,'selected':fields,'cache':'writeback; host page cache retained'},indent=2)+'\n')
            proc.kill();proc.wait(timeout=10)
            return fields
        finally:
            if remote:remote.sock.close()
            if proc and proc.poll() is None:proc.kill();proc.wait(timeout=10)
            debug.unlink(missing_ok=True);socketdir.rmdir()


def partition(disk,target):
    with disk.open('rb') as stream:
        stream.seek(1056);first,last=struct.unpack('<QQ',stream.read(16));stream.seek(first*512)
        target.write_bytes(stream.read((last-first+1)*512))


def linux_committed(disk,out,label,expected=DATA):
    image=out/f'{label}.ext4';partition(disk,image);linux=out/f'{label}.linux.ext4'
    oracle(image,linux,out/f'{label}-linux.log');dump=out/f'{label}.bin'
    result=subprocess.run(['debugfs','-R',f'dump /journal-persist.bin {dump}',str(linux)],capture_output=True,check=True)
    (out/f'{label}-dump.log').write_bytes(result.stdout+result.stderr);assert dump.read_bytes()==expected


def fixture_source(bs, placement='normal'):
    allowed=(REPO/'.codex-remote-attachments').resolve()
    directory=Path(os.environ.get('FORTRESS_EXT4_GUEST_FIXTURES',
        allowed/'ext4-phase8-5/host-frz8r6nu')).resolve()
    assert directory.is_relative_to(allowed) and (directory/'manifest.json').is_file()
    source=directory/f'{bs}-{placement}-512-pending-seed.img'
    assert source.is_file()
    return source


def main():
    parser=argparse.ArgumentParser();parser.add_argument('workspace',type=Path);parser.add_argument('--matrix',action='store_true');parser.add_argument('--recovery-only',action='store_true');parser.add_argument('--usb',action='store_true');args=parser.parse_args()
    allowed=(REPO/'.codex-remote-attachments').resolve();workspace=args.workspace.resolve();assert workspace.is_relative_to(allowed)
    out=Path(tempfile.mkdtemp(prefix='guest-usb-crash-' if args.usb else 'guest-crash-',dir=allowed/'ext4-phase9'));errors=[];records=[]
    iso=out/'fixture.iso';elf=out/'fortress.elf';shutil.copyfile(workspace/'bin/fortress.iso',iso);shutil.copyfile(workspace/'bin/fortress.elf',elf)
    original_command=guest.command
    base_command=original_command
    if args.usb:
        from test_ext4_journal_usb import command as base_command
        assert 'limine.conf' in json.loads((workspace/'workspace-manifest.json').read_text())['source_overrides']
    stop=functools.partial(stop_at_write,usb=args.usb)
    try:
        offsets=writer_offsets(elf,out)
        configurations=[(mode,bs,smp) for mode in ('bios','uefi') for bs in (1024,2048,4096) for smp in (1,4)] if args.matrix else [('bios',1024,1)]
        milestones=('before-write','durable-write','middle-checkpoint') if args.matrix else ('durable-write',)
        if args.usb and args.matrix:milestones=('durable-write','middle-checkpoint')
        if args.recovery_only:milestones=('recovery-interruption',)
        for mode,bs,smp in configurations:
            placement='wrap' if args.usb and bs==2048 else 'normal'
            source=fixture_source(bs,placement)
            for milestone in milestones:
                label=f'{mode}-{bs}-smp{smp}-{milestone}';disk=out/f'{label}.img';gpt(source,disk)
                before=milestone=='before-write';expected=b'' if before else DATA
                stop(out,iso,elf,disk,offsets,'jbd2_writer_commit' if before else 'jbd2_writer_checkpoint',label+'-cut',1 if before else 3,mode,bs,smp,milestone=='middle-checkpoint')
                linux_committed(disk,out,label+'-crash',expected)
                if args.recovery_only:
                    stop(out,iso,elf,disk,offsets,'jbd2_replay',label+'-partial-replay',0,mode,bs,smp,recovery=True)
                    linux_committed(disk,out,label+'-partial-replay')
                stop(out,iso,elf,disk,offsets,'jbd2_writer_commit',label+'-recovered',1,mode,bs,smp)
                linux_committed(disk,out,label+'-guest-replay',expected)
                variables=out/f'{label}-final-vars.fd'
                if mode=='uefi':shutil.copyfile(VARS,variables)
                def final_command(bootmode,bootdisk,bootvars,phase,bootiso):
                    cmd=base_command(bootmode,bootdisk,bootvars,phase,bootiso);cmd[cmd.index('-smp')+1]=str(smp);return cmd
                guest.command=final_command
                final=boot(mode,disk,variables,1,iso,out,label+'-final');audit(disk,out,label+'-final',1)
                records.append({'label':label,'mode':mode,'block':bs,'smp':smp,'milestone':milestone,'final_boot':final})
                print(f'PASS 9.4 {label}: crash, guest replay, clean Linux bytes/fsck',flush=True)
        guest.command=original_command
    except Exception as error:errors.append(repr(error));raise
    finally:
        guest.command=original_command
        (out/'manifest.json').write_text(json.dumps({'scope':'finite guest crash/recovery milestones; no physical power-loss claim',
            'workspace':str(workspace),'iso_sha256':hashlib.sha256(iso.read_bytes()).hexdigest(),
            'transport':'usb-bot' if args.usb else 'nvme',
            'elf_sha256':hashlib.sha256(elf.read_bytes()).hexdigest(),'records':records,'errors':errors},indent=2)+'\n')
        print(f'Evidence: {out}',flush=True)


if __name__=='__main__':main()
