"""Verify candidate readiness from explicit retained emulation results."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import uuid
import zlib
from test_nmi_transitions import REPO
from test_net_pci import CODE
from test_ext4_journal_mount import DATA
from create_ext4_physical_image import sha
from test_jbd2_replay_host import oracle

ROOT=REPO/'.codex-remote-attachments/ext4-phase9'
WORKSPACE=ROOT/'physical-workspace-7hnhj2c4'
MAIN=ROOT/'physical-validation-b01n3fy9'
SUPPLEMENTS={'duplicate':'physical-validation-7wijg0c9','ineligible-durability':'physical-validation-gep3byhh',
             'degraded':'physical-validation-t9hxxrx5','no-opt-in':'physical-validation-n6j6s8l1','ordinary-build':'physical-validation-y1t_6h_l'}

def load(folder):return json.loads((folder/'manifest.json').read_text())

def clean(path):
    with path.open('rb') as stream:stream.seek(1024);sb=stream.read(1024)
    assert struct.unpack_from('<H',sb,58)[0]==1 and not struct.unpack_from('<I',sb,96)[0]&4
    assert struct.unpack_from('<I',sb,232)[0]==0

def gpt_identity(path,expected=None):
    with path.open('rb') as stream:
        stream.seek(512);primary=stream.read(512);alternate=struct.unpack_from('<Q',primary,32)[0]
        tables=[]
        for lba in (1,alternate):
            stream.seek(lba*512);h=bytearray(stream.read(512));size=struct.unpack_from('<I',h,12)[0];stored=struct.unpack_from('<I',h,16)[0];struct.pack_into('<I',h,16,0)
            assert h[:8]==b'EFI PART' and zlib.crc32(h[:size])==stored
            array,count,entrysize=struct.unpack_from('<QII',h,72);stream.seek(array*512);entries=stream.read(count*entrysize)
            assert zlib.crc32(entries)==struct.unpack_from('<I',h,88)[0];tables.append(entries)
        assert tables[0]==tables[1]
        if expected:assert tables[0][144:160]==uuid.UUID(expected).bytes_le
        return tables[0]

def storage(folder):
    for path in folder.glob('*-argv.json'):
        cmd=json.loads(path.read_text());assert not any(option in cmd for option in ('-blockdev','-snapshot','-hda','-hdb','-sd'))
        drives=[cmd[k+1] for k,v in enumerate(cmd) if v=='-drive'];mode='uefi' if path.name.startswith('uefi-') else 'bios'
        disk=Path(drives[0].split(',')[0][5:]);assert disk.parent==folder and disk.is_file()
        assert drives[0]==f'file={disk},if=none,id=e4,format=raw,cache=writeback'
        assert len(drives)==(3 if mode=='uefi' else 1)
        if mode=='uefi':
            assert drives[1]==f'if=pflash,format=raw,unit=0,readonly=on,file={CODE}'
            variables=Path(drives[2].split('file=',1)[1]);assert variables.parent==folder and variables.is_file()
            assert drives[2]==f'if=pflash,format=raw,unit=1,file={variables}'
        devices=[cmd[k+1] for k,v in enumerate(cmd) if v=='-device']
        assert devices==['qemu-xhci,id=xhci,p2=4,p3=0','usb-storage,drive=e4,bus=xhci.0'+('' if '-cdrom' in cmd else ',bootindex=1')]
        if '-cdrom' in cmd:assert Path(cmd[cmd.index('-cdrom')+1]).parent==folder

def main():
    base=load(MAIN);assert base['errors']==["AssertionError('unexpected debugger stop')"]
    accepted={row['label']:(MAIN,row) for row in base['cases'] if not any(row['label'].endswith(profile) for profile in SUPPLEMENTS)}
    for profile,directory in SUPPLEMENTS.items():
        folder=ROOT/directory;manifest=load(folder);assert manifest['errors']==[]
        assert {row['label'] for row in manifest['cases']}=={f'{mode}-{profile}' for mode in ('bios','uefi')}
        for row in manifest['cases']:accepted[row['label']]=(folder,row)
    profiles=('wrong-target','read-only','wrong-capacity',*SUPPLEMENTS)
    expected={f'{mode}-persistence-cycle{cycle}' for mode in ('bios','uefi') for cycle in (1,2,3)}|{f'{mode}-{profile}' for mode in ('bios','uefi') for profile in profiles}
    assert set(accepted)==expected and len(accepted)==22
    folders={folder for folder,row in accepted.values()}
    for folder in folders:storage(folder)
    wanted={f'W{worker:02d}:{record:04d}:APPEND\n'.encode() for worker in range(2) for record in range(100)}
    for label,(folder,row) in accepted.items():
        if 'persistence-cycle' in label:
            phase=row['phase'];prefix=f'{label}-boot{phase}';clean(folder/f'{prefix}.ext4')
            assert (folder/f'{prefix}-journal-persist.bin').read_bytes()==DATA
            assert (folder/f'{prefix}-integration-dir-persist.bin').read_bytes()==DATA[:1041]
            assert (folder/f'{prefix}-reuse.bin').read_bytes()==DATA[:8192]
            text=(folder/f'{prefix}.log').read_text();assert '[EXT4 PHYSICAL] Authorized disposable fixture' in text and '[EXT4 INTEGRATION] SMP APPEND PASS' in text
            for kind in ('independent','shared'):
                data=(folder/f'{prefix}-journal-app-{kind}.txt').read_bytes();assert len(data)==3200 and {data[k:k+16] for k in range(0,3200,16)}==wanted
        else:
            assert row['write_callback_hits']==0 and row['published'] is False
            assert sha(folder/f'{label}-before.ext4')==sha(folder/f'{label}-after.ext4')==row['filesystem_sha256']
            if label.endswith('duplicate'):
                entries=gpt_identity(folder/f'{label}.img');assert entries[144:160]==entries[272:288]
            if label.endswith('ineligible-durability'):assert row['policy_adapter'] is True
    artifact=WORKSPACE/'physical-artifact';info=load(artifact);image=Path(info['image'])
    assert image.stat().st_size==4026531840 and sha(image)==info['image_sha256'];gpt_identity(image,info['data_partuuid'])
    assert sha(WORKSPACE/'bin/fortress.elf')==info['kernel_sha256']
    snapshot=load(WORKSPACE) if (WORKSPACE/'manifest.json').exists() else json.loads((WORKSPACE/'workspace-manifest.json').read_text())
    for name,digest in snapshot['sources'].items():assert sha(WORKSPACE/name)==digest,name
    assert sha(REPO/'src/kernel/main.c')==snapshot['sources']['src/kernel/main.c']
    assert b'#define FORTRESS_EXT4_PHYSICAL_PARTUUID' not in (REPO/'src/include/ext4_physical_fixture.h').read_bytes()
    seed=artifact/'pending-seed.ext4';assert sha(seed)==info['seed_sha256']
    checked=subprocess.run(['e2fsck','-fn',str(seed)],capture_output=True,text=True)
    (artifact/'seed-original-fsck.log').write_text(f'Exit: {checked.returncode}\n'+checked.stdout+checked.stderr)
    # Original is intentionally pending recovery. Independent oracle operates on a copy.
    oracle(seed,artifact/'seed-linux-recovered.ext4',artifact/'seed-linux-recovery.log')
    for directory,expected_cases in (('guest-journal-usb-7rrgx144',1),('guest-usb-fault-uexmpu_2',1)):
        manifest=load(ROOT/directory);assert manifest['errors']==[] and len(manifest['cases'])==expected_cases
    result={'status':'READY FOR AUTHORIZED FIRST PHYSICAL BOOT','physical_acceptance':'PENDING','cases':22,'persistence_boots':6,'rejection_controls':16,
            'accepted':{label:{'directory':str(folder),'row':row} for label,(folder,row) in accepted.items()},'retained_main_attempt_errors':base['errors'],
            'image_sha256':info['image_sha256'],'regressions':['guest-journal-usb-7rrgx144','guest-usb-fault-uexmpu_2'],'errors':[]}
    (artifact/'readiness-review.json').write_text(json.dumps(result,indent=2)+'\n');info['tests']='22/22 reviewed emulation cases; two focused ordinary-build regressions PASS';(artifact/'manifest.json').write_text(json.dumps(info,indent=2)+'\n')
    print(f'PASS reviewed readiness: {artifact}',flush=True)

if __name__=='__main__':main()
