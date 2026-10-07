"""Native USB/BOT backend errors and pending-transfer removal on owned images."""
import argparse
import hashlib
import json
import re
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import time
from test_ext4_guest_crash import GuestRemote,writer_offsets,stop_at_write,linux_committed,partition
from test_nmi_transitions import REPO,QMP,symbols
from test_ext4_read import gpt
from test_ext4_journal_usb import command as normal_command
import test_ext4_journal_mount as guest
from test_net_pci import CODE,VARS

PROFILES=('admission-flush','ordered-write','journal-write','checkpoint-write','sync-flush','disconnect')


def validate_storage(cmd,out,disk,iso,mode,variables,backend):
    assert out.resolve().is_relative_to((REPO/'.codex-remote-attachments/ext4-phase9').resolve())
    assert disk.resolve().parent==iso.resolve().parent==out.resolve() and disk.is_file() and iso.is_file()
    nodes=[json.loads(cmd[k+1]) for k,value in enumerate(cmd) if value=='-blockdev']
    assert nodes==backend and nodes[0]['filename']==str(disk)
    drives=[cmd[k+1] for k,value in enumerate(cmd) if value=='-drive']
    expected=[] if mode=='bios' else [f'if=pflash,format=raw,unit=0,readonly=on,file={CODE}',f'if=pflash,format=raw,unit=1,file={variables}']
    assert mode in ('bios','uefi') and drives==expected
    assert [cmd[k+1] for k,value in enumerate(cmd) if value=='-device']==[
        'qemu-xhci,id=xhci,p2=4,p3=0','usb-storage,drive=e4,id=journalusb,bus=xhci.0,werror=report,rerror=report,share-rw=on']
    assert [cmd[k+1] for k,value in enumerate(cmd) if value=='-cdrom']==[str(iso)]
    assert not any(value in cmd for value in ('-snapshot','-hda','-hdb','-sd'))


def dma_layouts(elf,out):
    result=subprocess.run(['readelf','--debug-dump=info',str(elf)],capture_output=True,text=True,check=True)
    layouts={}
    for label,names in {'bot':('bulk_in_ring_phys','bulk_out_ring_phys','transport_failed','latched_offline'),
                        'dev':('bounce_buf_phys','bounce_buf_virt','bounce_allocation_phys','bounce_allocation_pages')}.items():
        for section in result.stdout.split(' <1><'):
            if '(DW_TAG_structure_type)' not in section:continue
            found={}
            for member in section.split('(DW_TAG_member)')[1:]:
                name=re.search(r'DW_AT_name\s*:\s*(?:\([^\n]*\): )?(\w+)',member)
                location=re.search(r'DW_AT_data_member_location:\s*(\d+)',member)
                if name and location:found[name[1]]=int(location[1])
            if all(name in found for name in names):layouts[label]=found;break
        assert label in layouts,f'missing complete {label} DMA layout'
    (out/'dma-layouts.json').write_text(json.dumps(layouts,indent=2)+'\n');return layouts


def fields(remote,writer,offsets):
    return {key:struct.unpack('<I',remote.memory(writer+offsets[key],4))[0] for key in ('state','data_count','metadata_count','sequence')}


def first_write(remote,sym,offsets,bs,checkpoint=False):
    for _ in range(256):
        remote.resume_to(sym['jbd2_writer_checkpoint' if checkpoint else 'jbd2_writer_commit'])
        writer=remote.registers()[0][5];state=fields(remote,writer,offsets)
        if state['data_count']==16384//bs and struct.unpack('<Q',remote.memory(sym['e4_active'],8))[0]:return writer,state
        assert remote.request('s').startswith(('T05','S05'))
    raise AssertionError('target write not reached')


def case(out,iso,elf,offsets,layouts,mode,bs,smp,profile):
    label=f'{mode}-{bs}-smp{smp}-{profile}';disk=out/f'{label}.img'
    source=REPO/f'.codex-remote-attachments/ext4-phase8-5/host-frz8r6nu/{bs}-normal-512-pending-seed.img';gpt(source,disk)
    before=out/f'{label}-before.ext4';partition(disk,before)
    sockets=Path(tempfile.mkdtemp(prefix='fortress-usb-fault-'));variables=out/f'{label}-vars.fd'
    event='flush_to_disk' if profile.endswith('flush') else 'write_aio'
    backend=[{'driver':'file','filename':str(disk),'node-name':'file','cache':{'direct':False,'no-flush':False}},
        {'driver':'blkdebug','image':'file','node-name':'debug',
         'inject-error':[{'event':event,'state':2,'errno':5,'once':True,'immediately':True}],
         'set-state':[{'event':'pwritev_zero','state':1,'new_state':2}]},
        {'driver':'raw','file':'debug','node-name':'e4'}]
    cmd=['qemu-system-x86_64','-M','q35','-m','2G','-accel','tcg','-smp',str(smp),'-S',
        '-display','none','-monitor','none','-no-reboot','-boot','d','-cdrom',str(iso),
        '-serial',f'file:{out/(label+".serial.log")}','-net','none',
        '-device','qemu-xhci,id=xhci,p2=4,p3=0',
        '-device','usb-storage,drive=e4,id=journalusb,bus=xhci.0,werror=report,rerror=report,share-rw=on',
        '-fw_cfg','name=opt/fortress/ext4_journal_usb_test,string=1',
        '-qmp',f'unix:{sockets/"qmp"},server=on,wait=off','-gdb',f'unix:{sockets/"gdb"},server=on,wait=off']
    for node in backend:cmd+=['-blockdev',json.dumps(node)]
    if profile!='admission-flush':cmd+=['-fw_cfg',f'name=opt/fortress/ext4_journal_usb_{"sync" if profile=="sync-flush" else "write"}_fault,string=1']
    if mode=='uefi':
        shutil.copyfile(VARS,variables);cmd+=['-drive',f'if=pflash,format=raw,unit=0,readonly=on,file={CODE}',
                                            '-drive',f'if=pflash,format=raw,unit=1,file={variables}']
    validate_storage(cmd,out,disk,iso,mode,variables,backend)
    for extra in (['-drive','file=/dev/sda'],['-blockdev','{"driver":"host_device","filename":"/dev/sda"}'],['-snapshot'],['-device','nvme,drive=e4'],['-hda','/dev/sda']):
        try:validate_storage(cmd+extra,out,disk,iso,mode,variables,backend)
        except AssertionError:pass
        else:raise AssertionError('extra storage accepted')
    (out/f'{label}-argv.json').write_text(json.dumps(cmd,indent=2)+'\n')
    sym=symbols(str(elf));remote=None;qmp=None;process=None;milestone={'profile':profile}
    with (out/f'{label}.stderr.log').open('wb') as error:
        try:
            process=subprocess.Popen(cmd,stdout=subprocess.DEVNULL,stderr=error,cwd=REPO)
            qmp=QMP(sockets/'qmp');qmp.sock.settimeout(180)
            remote=GuestRemote(sockets/'gdb');remote.sock.settimeout(180)
            remote.request('qSupported');remote.request('qXfer:features:read:target.xml:0,fff')
            if profile=='admission-flush':
                remote.resume_to(sym['test_ext4_journal_usb_mount']);remote.resume_to(sym['block_flush'])
                assert not struct.unpack('<Q',remote.memory(sym['e4_active'],8))[0]
                milestone['published']=False
            elif profile=='sync-flush':
                remote.resume_to(sym['ext4_sync'])
                assert remote.registers()[0][5]==struct.unpack('<Q',remote.memory(sym['g_ext4_fixture_mount'],8))[0]
            else:
                writer,state=first_write(remote,sym,offsets,bs,profile=='checkpoint-write');milestone.update(state)
                assert state['state']==(3 if profile=='checkpoint-write' else 1)
                if profile=='journal-write':
                    io=struct.unpack('<Q',remote.memory(writer,8))[0];writes=0
                    for _ in range(256):
                        remote.resume_to(sym['j_io']);regs=remote.registers()[0]
                        if regs[5]==io and regs[2]:
                            writes+=1
                            if writes==state['data_count']+1:
                                header=remote.memory(regs[3],8);assert header==bytes.fromhex('c03b399800000001')
                                milestone.update(journal_block=regs[4],descriptor_header=header.hex());break
                        assert remote.request('s').startswith(('T05','S05'))
                    else:raise AssertionError('journal descriptor not reached')
                if profile=='disconnect':
                    remote.resume_to(sym['xhci_bot_transfer']);regs=remote.registers()[0]
                    assert remote.memory(regs[8],1)==b'\x2a','removal must target WRITE(10)'
                    bot=regs[2];dev=regs[3]
                    retained={name:struct.unpack('<Q',remote.memory(dev+layouts['dev'][name],8))[0] for name in ('bounce_buf_phys','bounce_buf_virt','bounce_allocation_phys')}
                    retained.update({name:struct.unpack('<Q',remote.memory(bot+layouts['bot'][name],8))[0] for name in ('bulk_in_ring_phys','bulk_out_ring_phys')})
                    remote.resume_to(sym['wait_transfer_event']);regs=remote.registers()[0]
                    milestone.update(bot_rings=bot,submitted_trb=regs[8],slot=regs[3],endpoint=regs[2])
                    milestone['retained_dma_before']=retained
            milestone['stop']=remote.last_stop
            if profile=='disconnect':milestone['qmp_remove']=qmp.execute('device_del',{'id':'journalusb'})
            else:
                # Zero an already-zero, unallocated GPT gap sector. This host-only
                # event arms blkdebug; subsequent errors traverse real USB/BOT.
                with disk.open('rb') as stream:stream.seek(1024*512);assert stream.read(512)==bytes(512)
                milestone['qmp_arm']=qmp.execute('human-monitor-command',{'command-line':'qemu-io e4 "write -z 524288 512"'})
                assert milestone['qmp_arm']=='',milestone['qmp_arm']
            (out/f'{label}-armed.json').write_text(json.dumps(milestone,indent=2)+'\n')
            wanted=b'[EXT4 USB JOURNAL] REJECT flush preflight' if profile=='admission-flush' else b'[EXT4 USB JOURNAL] FAULT PASS;'
            deadline=time.monotonic()+180
            for _ in range(1024):
                assert time.monotonic()<deadline
                remote.resume_to(sym['serial_puts']);pointer=remote.registers()[0][5]
                observed=remote.memory(pointer,len(wanted))
                if observed.startswith(b'[FAIL]'):raise AssertionError('native fixture assertion failed; see retained serial log')
                if observed==wanted:break
                assert remote.request('s').startswith(('T05','S05'))
            else:raise AssertionError('expected native failure assertions not reached')
            milestone['native_assertions']=wanted.decode();milestone['active_mount']=struct.unpack('<Q',remote.memory(sym['e4_active'],8))[0]
            if profile=='admission-flush':assert not milestone['active_mount']
            else:assert milestone['active_mount']
            if profile=='disconnect':
                current={name:struct.unpack('<Q',remote.memory(dev+layouts['dev'][name],8))[0] for name in ('bounce_buf_phys','bounce_buf_virt','bounce_allocation_phys')}
                current.update({name:struct.unpack('<Q',remote.memory(bot+layouts['bot'][name],8))[0] for name in ('bulk_in_ring_phys','bulk_out_ring_phys')})
                assert current==retained and all(current.values()),'DMA state changed after uncertain removal'
                failed={name:bool(remote.memory(bot+layouts['bot'][name],1)[0]) for name in ('transport_failed','latched_offline')}
                assert failed['transport_failed'] or failed['latched_offline']
                milestone.update(retained_dma_after=current,transport_latches=failed)
            (out/f'{label}-milestone.json').write_text(json.dumps(milestone,indent=2)+'\n')
            process.kill();process.wait(timeout=10)
        finally:
            if remote:remote.sock.close()
            if qmp:qmp.sock.close()
            if process and process.poll() is None:process.kill();process.wait(timeout=10)
            for name in ('qmp','gdb'):(sockets/name).unlink(missing_ok=True)
            sockets.rmdir()
    after=out/f'{label}-failed.ext4';partition(disk,after)
    expected=guest.DATA if profile in ('checkpoint-write','sync-flush') else b''
    if profile=='admission-flush':assert after.read_bytes()==before.read_bytes()
    else:
        sb=after.read_bytes()[1024:2048];assert struct.unpack_from('<I',sb,96)[0]&4 and not struct.unpack_from('<H',sb,58)[0]&1
        linux_committed(disk,out,label+'-failed',expected)
    stop_at_write(out,iso,elf,disk,offsets,'jbd2_writer_commit',label+'-recovered',1,mode,bs,smp,usb=True)
    linux_committed(disk,out,label+'-guest-replay',expected)
    original=guest.command
    def command(bootmode,bootdisk,bootvars,phase,bootiso):
        result=normal_command(bootmode,bootdisk,bootvars,phase,bootiso);result[result.index('-smp')+1]=str(smp);return result
    guest.command=command
    try:guest.boot(mode,disk,variables,1,iso,out,label+'-final');guest.audit(disk,out,label+'-final',1)
    finally:guest.command=original
    print(f'PASS USB native failure {label}',flush=True);return {'label':label,'milestone':milestone}


def main():
    parser=argparse.ArgumentParser();parser.add_argument('workspace',type=Path);parser.add_argument('--matrix',action='store_true');parser.add_argument('--calibrate',action='store_true');parser.add_argument('--profile',choices=PROFILES,default='ordered-write');parser.add_argument('--continue-from',type=Path);args=parser.parse_args()
    root=REPO/'.codex-remote-attachments/ext4-phase9';workspace=args.workspace.resolve();assert workspace.is_relative_to(root.resolve())
    out=Path(tempfile.mkdtemp(prefix='guest-usb-fault-',dir=root));iso=out/'fixture.iso';elf=out/'fortress.elf'
    shutil.copyfile(workspace/'bin/fortress.iso',iso);shutil.copyfile(workspace/'bin/fortress.elf',elf);records=[];errors=[]
    total=72 if args.matrix else 6 if args.calibrate else 1
    continuation=None
    if args.continue_from:
        assert args.matrix,'continuation requires the full matrix'
        previous=args.continue_from.resolve();assert previous.is_relative_to(root.resolve())
        prior=json.loads((previous/'manifest.json').read_text())
        assert prior['workspace']==str(workspace)
        assert prior['iso_sha256']==hashlib.sha256(iso.read_bytes()).hexdigest()
        assert prior['elf_sha256']==hashlib.sha256(elf.read_bytes()).hexdigest()
        labels=[f'{mode}-{bs}-smp{smp}-{profile}' for mode in ('bios','uefi') for bs in (1024,2048,4096) for smp in (1,4) for profile in PROFILES]
        assert [record['label'] for record in prior['cases']]==labels[:len(prior['cases'])]
        records=[dict(record,evidence_directory=record.get('evidence_directory',str(previous))) for record in prior['cases']]
        continuation={'directory':str(previous),'manifest_sha256':hashlib.sha256((previous/'manifest.json').read_bytes()).hexdigest(),'previous_errors':prior['errors']}
    def progress(active=None):
        temporary=out/'progress.tmp';temporary.write_text(json.dumps({'expected':total,'completed':len(records),'errors':errors,'active':active})+'\n');temporary.replace(out/'progress.json')
    progress()
    try:
        offsets=writer_offsets(elf,out)
        layouts=dma_layouts(elf,out)
        configs=[(mode,bs,smp) for mode in ('bios','uefi') for bs in (1024,2048,4096) for smp in (1,4)] if args.matrix else [('bios',1024,1)]
        for mode,bs,smp in configs:
            for profile in PROFILES if args.matrix or args.calibrate else (args.profile,):
                if any(record['label']==f'{mode}-{bs}-smp{smp}-{profile}' for record in records):continue
                progress(f'{mode}-{bs}-smp{smp}-{profile}')
                records.append(case(out,iso,elf,offsets,layouts,mode,bs,smp,profile));progress()
    except Exception as error:errors.append(repr(error));raise
    finally:
        progress()
        (out/'manifest.json').write_text(json.dumps({'cases':records,'errors':errors,'workspace':str(workspace),'continuation':continuation,
            'iso_sha256':hashlib.sha256(iso.read_bytes()).hexdigest(),'elf_sha256':hashlib.sha256(elf.read_bytes()).hexdigest()},indent=2)+'\n')
        print(f'Evidence: {out}',flush=True)


if __name__=='__main__':main()
