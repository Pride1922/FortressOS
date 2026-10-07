"""Verify the declared finite guest matrix and supplementary final AP byte audits."""
import argparse
import hashlib
import json
import struct
from pathlib import Path
from test_ext4_guest_append_crash import check_final
from test_ext4_guest_crash import REPO,validate_storage


def allocation_identity(image):
    data=image.read_bytes();bs=1024<<struct.unpack_from('<I',data,1048)[0]
    first=struct.unpack_from('<I',data,1044)[0];blocks=struct.unpack_from('<I',data,1028)[0]
    bpg=struct.unpack_from('<I',data,1056)[0];groups=(blocks-first+bpg-1)//bpg
    identity=[data[1036:1044]]  # superblock free block/inode counts
    assert struct.unpack_from('<I',data,1256)[0]==0,'orphan drain incomplete'
    for group in range(groups):
        descriptor=(first+1)*bs+group*32
        identity.append(data[descriptor+12:descriptor+16])
        for offset in (0,4):
            block=struct.unpack_from('<I',data,descriptor+offset)[0]
            identity.append(data[block*bs:(block+1)*bs])
    return identity


def main():
    parser=argparse.ArgumentParser()
    for name in ('transaction','recovery','append','orphan'):parser.add_argument(name,type=Path)
    args=parser.parse_args();root=REPO/'.codex-remote-attachments/ext4-phase9'
    configurations=[f'{mode}-{bs}-smp{smp}' for mode in ('bios','uefi') for bs in (1024,2048,4096) for smp in (1,4)]
    expected={
        'transaction':{f'{label}-{milestone}' for label in configurations for milestone in ('before-write','durable-write','middle-checkpoint')},
        'recovery':{f'{label}-recovery-interruption' for label in configurations},
        'append':{f'{mode}-{bs}-{kind}' for mode in ('bios','uefi') for bs in (1024,2048,4096) for kind in ('independent','shared')},
        'orphan':{f'{mode}-{bs}' for mode in ('bios','uefi') for bs in (1024,2048,4096)}}
    results=[];identity=None
    for name in expected:
        folder=getattr(args,name).resolve();assert folder.is_relative_to(root.resolve())
        manifest=json.loads((folder/'manifest.json').read_text());assert manifest['errors']==[]
        rows=manifest.get('records',manifest.get('cases'));labels=[row['label'] for row in rows]
        assert len(labels)==len(set(labels)) and set(labels)==expected[name],name
        hashes={file:hashlib.sha256((folder/file).read_bytes()).hexdigest() for file in ('fixture.iso','fortress.elf')}
        if identity is None:identity=hashes
        assert hashes==identity,'campaigns used different binaries'
        for file in folder.glob('*-argv.json'):
            if '-boot' in file.name:continue  # normal boots have their own exact argv preflight
            cmd=json.loads(file.read_text());mode='uefi' if 'uefi-' in file.name else 'bios'
            drive=cmd[cmd.index('-drive')+1];disk=Path(drive.split(',')[0][5:])
            variables=folder/(file.name.removesuffix('-argv.json')+'-vars.fd')
            validate_storage(cmd,mode,disk,folder/'fixture.iso',variables)
        for row in rows:
            label=row['label']
            assert (folder/f'{label}-final-boot{2 if name in ("append","orphan") else 1}-linux.log').is_file()
            if name=='append':
                milestone=row['milestone'];assert int(milestone['debugger_thread'].split('.')[-1],16)>1
                record=bytes.fromhex(milestone['append_record_hex']);assert len(record)==16
                for suffix in ('crash','guest-replay'):assert (folder/f'{label}-{suffix}.bin').read_bytes()==record
                check_final(folder,label)
            elif name in ('transaction','recovery'):
                expected_bytes=b'' if label.endswith('-before-write') else bytes((k*17+3)&255 for k in range(16384))
                for suffix in ('crash','guest-replay'):assert (folder/f'{label}-{suffix}.bin').read_bytes()==expected_bytes
                if name=='recovery':
                    milestone=json.loads((folder/f'{label}-partial-replay-milestone.json').read_text())
                    assert milestone['home_images_completed']==1 and milestone['published'] is False
            else:
                for suffix in ('crash','guest-replay','final'):assert (folder/f'{label}-{suffix}.bin').read_bytes()==bytes((k*17+3)&255 for k in range(1041))
                assert allocation_identity(folder/f'{label}-crash.linux.ext4')==allocation_identity(folder/f'{label}-guest-replay.ext4'),'Linux/guest orphan allocation disagreement'
        evidence={file.name:hashlib.sha256(file.read_bytes()).hexdigest() for file in folder.iterdir()
                  if file.is_file() and file.suffix in ('.json','.log','.bin')}
        results.append({'campaign':name,'directory':str(folder),'cases':len(rows),'binaries':hashes,'evidence_sha256':evidence})
    output=root/'phase9-4-review.json';output.write_text(json.dumps({'campaigns':results,'cases':sum(item['cases'] for item in results),'errors':[]},indent=2)+'\n')
    print(f'PASS finite guest evidence review: {output}',flush=True)


if __name__=='__main__':main()
