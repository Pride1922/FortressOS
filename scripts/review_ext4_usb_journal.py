"""Read-only review of the finite Phase 9.5 ledger and native USB evidence."""
import hashlib
import json
import struct
import subprocess
from pathlib import Path
from test_ext4_guest_crash import REPO, validate_storage
from test_ext4_usb_journal_fault import PROFILES, validate_storage as fault_storage
from test_ext4_guest_append_crash import check_final
from review_ext4_guest_crash import allocation_identity
from test_ext4_journal_mount import DATA
import test_ext4_integration as integration

ROOT=REPO/'.codex-remote-attachments/ext4-phase9'
LEDGER={'normal':'guest-journal-usb-9lpsdcgh','transaction':'guest-usb-crash-lqhs2g04',
        'recovery':'guest-usb-crash-y6w7emcp','append':'guest-usb-append-crash-9s4ke5b5',
        'orphan':'guest-usb-orphan-crash-53aynd29','failure':'guest-usb-fault-2nz4jry3',
        'admission':'guest-usb-admission-p9kxg2p3','nvme':'guest-nvme-regression-lq3h4l6b'}

def sha(path):
    digest=hashlib.sha256()
    with path.open('rb') as stream:
        while chunk:=stream.read(1024*1024):digest.update(chunk)
    return digest.hexdigest()

def clean(image):
    with image.open('rb') as stream:stream.seek(1024);sb=stream.read(1024)
    assert struct.unpack_from('<H',sb,58)[0]&1 and not struct.unpack_from('<I',sb,96)[0]&4
    assert struct.unpack_from('<I',sb,232)[0]==0

def storage(folder,usb=True):
    for file in folder.glob('*-argv.json'):
        cmd=json.loads(file.read_text());mode='uefi' if file.name.startswith('uefi-') else 'bios'
        if '-blockdev' in cmd:
            backend=[json.loads(cmd[k+1]) for k,value in enumerate(cmd) if value=='-blockdev']
            disk=Path(backend[0]['filename']);variables=folder/(file.name.removesuffix('-argv.json')+'-vars.fd')
            fault_storage(cmd,folder,disk,folder/'fixture.iso',mode,variables,backend)
            for extra in (['-drive','file=/dev/sda'],['-blockdev','{"driver":"host_device","filename":"/dev/sda"}'],['-snapshot'],['-device','nvme,drive=e4']):
                try:fault_storage(cmd+extra,folder,disk,folder/'fixture.iso',mode,variables,backend)
                except AssertionError:pass
                else:raise AssertionError('extra storage accepted by review')
        else:
            drives=[cmd[k+1] for k,value in enumerate(cmd) if value=='-drive']
            disk=Path(drives[0].split(',')[0][5:]);variables=None
            if mode=='uefi':variables=Path(drives[-1].split('file=',1)[1])
            iso=Path(cmd[cmd.index('-cdrom')+1]);assert iso.parent==folder and iso.name in ('fixture.iso','read-only.iso')
            if usb:validate_storage(cmd,mode,disk,iso,variables,usb=True)
            else:
                integration.SMP=4
                phase=2 if file.name.endswith('-boot2-argv.json') else 1
                assert cmd==integration.command(mode,disk,variables,phase,iso)

def main():
    configurations=[f'{mode}-{bs}-smp{smp}' for mode in ('bios','uefi') for bs in (1024,2048,4096) for smp in (1,4)]
    expected={'normal':set(configurations),'transaction':{f'{label}-{cut}' for label in configurations for cut in ('durable-write','middle-checkpoint')},
              'recovery':{f'{label}-recovery-interruption' for label in configurations},
              'append':{f'{mode}-{bs}-{kind}' for mode in ('bios','uefi') for bs in (1024,2048,4096) for kind in ('independent','shared')},
              'orphan':set(configurations),'failure':{f'{label}-{profile}' for label in configurations for profile in PROFILES},
              'admission':{f'{mode}-{profile}' for mode in ('bios','uefi') for profile in ('read-only','wrong-guid','duplicate','degraded','conflicting')},
              'nvme':{f'{mode}-{bs}' for mode in ('bios','uefi') for bs in (1024,2048,4096)}}
    results=[];folders=set();builds={}
    for name,directory in LEDGER.items():
        folder=ROOT/directory;manifest=json.loads((folder/'manifest.json').read_text());assert manifest['errors']==[],name
        rows=manifest.get('records',manifest.get('cases'))
        if name=='nvme':rows=[dict(row,label=row['case']) for row in rows]
        labels=[row['label'] for row in rows]
        assert len(labels)==len(set(labels)) and set(labels)==expected[name],(name,labels)
        workspace=Path(manifest.get('workspace',str(ROOT/'guest-workspace-zwm23w_9')))
        binaries={'fixture.iso':sha(folder/'fixture.iso'),'fortress.elf':sha(workspace/'bin/fortress.elf' if name=='nvme' else folder/'fortress.elf')}
        for file,built in (('fixture.iso','fortress.iso'),('fortress.elf','fortress.elf')):
            assert binaries[file]==sha(workspace/'bin'/built),(name,file)
        builds[str(workspace)]=binaries;folders.add(folder)
        storage(folder,usb=name!='nvme')
        for row in rows:
            label=row['label'];evidence=Path(row.get('evidence_directory',str(folder)));assert evidence.resolve().is_relative_to(ROOT.resolve())
            folders.add(evidence)
            if name=='normal':
                assert len(row['boots'])==3
                for cycle,phase in ((1,1),(2,2),(3,2)):
                    prefix=f'{label}-cycle{cycle}-boot{phase}';clean(folder/f'{prefix}.ext4')
                    text=(folder/f'{prefix}.log').read_text();assert 'durability=sync-backed' in text
                    assert (folder/f'{prefix}-journal-persist.bin').read_bytes()==DATA
                    assert (folder/f'{prefix}-integration-dir-persist.bin').read_bytes()==DATA[:1041]
                    assert (folder/f'{prefix}-reuse.bin').read_bytes()==DATA[:8192]
                    if cycle==3:
                        audit=(folder/f'{prefix}-linux.log').read_text();assert audit.count('File not found')>=3
                    if label.endswith('smp4'):
                        assert '[EXT4 INTEGRATION] SMP APPEND PASS' in text
                        wanted={f'W{wid:02d}:{seq:04d}:APPEND\n'.encode() for wid in range(2) for seq in range(100)}
                        for kind in ('independent','shared'):
                            data=(folder/f'{prefix}-journal-app-{kind}.txt').read_bytes()
                            assert len(data)==3200 and {data[k:k+16] for k in range(0,3200,16)}==wanted
            elif name in ('transaction','recovery'):
                for suffix in ('crash','guest-replay'):assert (folder/f'{label}-{suffix}.bin').read_bytes()==DATA
                clean(folder/f'{label}-final-boot1.ext4')
                if name=='recovery':
                    cut=json.loads((folder/f'{label}-partial-replay-milestone.json').read_text())
                    assert cut['home_images_completed']==1 and cut['published'] is False
            elif name=='append':
                cut=row['milestone'];assert int(cut['debugger_thread'].split('.')[-1],16)>1
                for suffix in ('crash','guest-replay'):assert (folder/f'{label}-{suffix}.bin').read_bytes()==bytes.fromhex(cut['append_record_hex'])
                check_final(folder,label);clean(folder/f'{label}-final-boot2.ext4')
            elif name=='orphan':
                for suffix in ('crash','guest-replay','final'):assert (folder/f'{label}-{suffix}.bin').read_bytes()==DATA[:1041]
                assert allocation_identity(folder/f'{label}-crash.linux.ext4')==allocation_identity(folder/f'{label}-guest-replay.ext4')
                clean(folder/f'{label}-final-boot2.ext4')
            elif name=='failure':
                cut=json.loads((evidence/f'{label}-milestone.json').read_text());assert cut==row['milestone']
                profile=cut['profile'];assert cut['native_assertions'].startswith('[EXT4 USB JOURNAL]')
                assert bool(cut['active_mount'])==(profile!='admission-flush')
                if profile=='admission-flush':assert sha(evidence/f'{label}-before.ext4')==sha(evidence/f'{label}-failed.ext4')
                else:
                    with (evidence/f'{label}-failed.ext4').open('rb') as stream:stream.seek(1024);sb=stream.read(1024)
                    assert struct.unpack_from('<I',sb,96)[0]&4 and not struct.unpack_from('<H',sb,58)[0]&1
                    payload=DATA if profile in ('checkpoint-write','sync-flush') else b''
                    for suffix in ('failed','guest-replay'):assert (evidence/f'{label}-{suffix}.bin').read_bytes()==payload
                    if profile=='disconnect':
                        assert cut['retained_dma_before']==cut['retained_dma_after'] and all(cut['retained_dma_after'].values())
                        assert any(cut['transport_latches'].values()) and cut['submitted_trb']
                clean(evidence/f'{label}-final-boot1.ext4')
            elif name=='admission':
                assert not row['published'] and row['write_callback_hits']==0
                assert sha(folder/f'{label}-before.ext4')==sha(folder/f'{label}-after.ext4')==row['partition_sha256']
            else:
                for phase in (1,2):clean(folder/f'{label}-boot{phase}.ext4')
        results.append({'campaign':name,'directory':str(folder),'cases':len(rows),'binaries':binaries,'manifest_sha256':sha(folder/'manifest.json')})
    assert sum(item['cases'] for item in results if item['campaign'] not in ('admission','nvme'))==144
    for folder in folders:
        if folder.name.startswith('guest-usb-fault-'):storage(folder)
    workspace=ROOT/'guest-workspace-zwm23w_9'
    logs={name:sha(workspace/name) for name in ('bot-host.log','mount-host.log','ext2-usb-regression.log','e4a-usb-regression.log')}
    assert 'ALL BOT/SCSI HOST UNIT TESTS PASSED!' in (workspace/'bot-host.log').read_text()
    assert '[ALL PASS] Phase 9G.4 host unit tests passed successfully!' in (workspace/'mount-host.log').read_text()
    assert '[ALL PASS] 3-boot persistence suite passed successfully under BIOS!' in (workspace/'ext2-usb-regression.log').read_text()
    assert '[ALL PASS] 3-boot persistence suite passed successfully under UEFI!' in (workspace/'ext2-usb-regression.log').read_text()
    assert '4/4 cases, 12 boots PASS' in (workspace/'e4a-usb-regression.log').read_text()
    versions=[]
    for argv in (['qemu-system-x86_64','--version'],['e2fsck','-V'],['debugfs','-V'],['gcc','--version'],['python3','--version']):
        result=subprocess.run(argv,capture_output=True,text=True,check=True);versions.append({'argv':argv,'output':result.stdout+result.stderr})
    provenance=[]
    for bs,placement in ((1024,'normal'),(2048,'normal'),(2048,'wrap'),(4096,'normal')):
        seed=REPO/f'.codex-remote-attachments/ext4-phase8-5/host-frz8r6nu/{bs}-{placement}-512-pending-seed.img'
        info=subprocess.run(['dumpe2fs','-h',str(seed)],capture_output=True,text=True,check=True)
        provenance.append({'source':str(seed),'sha256':sha(seed),'geometry':info.stdout,'diagnostics':info.stderr})
    evidence={str(file.relative_to(ROOT)):sha(file) for folder in folders for file in folder.iterdir() if file.is_file() and file.suffix in ('.json','.log','.bin')}
    output=ROOT/'phase9-5-review.json'
    output.write_text(json.dumps({'campaigns':results,'main_cases':144,'admission_cases':10,'regressions':logs,'builds':builds,'fixtures':provenance,'versions':versions,'evidence_sha256':evidence,'errors':[]},indent=2)+'\n')
    print(f'PASS Phase 9.5 evidence review: {output}',flush=True)

if __name__=='__main__':main()
