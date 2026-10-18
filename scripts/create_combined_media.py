#!/usr/bin/env python3
"""One published raw USB image: live session plus full, gated installer."""
import argparse
import json
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile
import binascii
import hashlib
import re
import struct
import tarfile
from create_installer_media import ROOT, build, digest

def layout(image):
    assert not image.is_symlink() and stat.S_ISREG(image.stat().st_mode)
    sectors=image.stat().st_size//512
    with image.open('rb') as stream:
        assert stream.read(512)[510:]==b'\x55\xaa'
        entries=None
        for at,other,array_lba in ((1,sectors-1,2),(sectors-1,1,sectors-33)):
            stream.seek(at*512);header=bytearray(stream.read(512))
            checksum=struct.unpack_from('<I',header,16)[0];struct.pack_into('<I',header,16,0)
            assert header[:8]==b'EFI PART' and binascii.crc32(header[:92])==checksum
            assert struct.unpack_from('<QQ',header,24)==(at,other)
            assert struct.unpack_from('<QII',header,72)==(array_lba,128,128)
            stream.seek(array_lba*512);current=stream.read(16384)
            assert binascii.crc32(current)==struct.unpack_from('<I',header,88)[0]
            if entries is not None:assert entries==current
            entries=current
    result=[struct.unpack_from('<16s16sQQQ72s',entries,i*128) for i in range(4)]
    assert not any(entries[512:]) and all(e[0]!=bytes(16) and e[1]!=bytes(16) for e in result)
    assert len({e[1] for e in result})==4
    regions=sorted((e[2],e[3]) for e in result)
    assert all(34<=a<=b<sectors-33 for a,b in regions)
    assert all(regions[i][1]<regions[i+1][0] for i in range(3))
    assert [(e[2],e[3]-e[2]+1) for e in result]==[(4096,512*2048),(2048,2048),(514*2048,8*2048),(522*2048,64*2048)]
    return result

def verify(image):
    import uuid
    from create_installer_media import ROLES,SIZES
    parts=layout(image);fat=f'{image}@@{parts[0][2]*512}'
    with tempfile.TemporaryDirectory(prefix='fortress-combined-audit-') as directory:
        temp=Path(directory)
        def extract(name):
            target=temp/Path(name).name
            subprocess.run(['mcopy','-o','-i',fat,'::'+name,str(target)],check=True,capture_output=True)
            return target
        config=extract('boot/limine/limine.conf').read_text()
        assert re.findall(r'^/(.+)$',config,re.M)==['FortressOS (Live USB)','FortressOS (Installer)']
        live,installer=config.split('/FortressOS (Installer)',1)
        assert f'usb_data=PARTUUID={str(uuid.UUID(bytes_le=parts[3][1])).upper()} usb_data_mode=rw' in live
        assert live.count('module_path:')==1 and 'role=install-' not in live
        assert 'boot_mode=installer' in installer and installer.count('module_path:')==6
        for role in ('esp','root','bios','mbr','manifest'):assert installer.count('role=install-'+role+'\n')==1
        with tarfile.open(extract('boot/initramfs.tar')) as archive:
            tool=archive.getmember('bin/install')
            assert tool.isfile() and tool.uid==tool.gid==0 and tool.mode==0o755
            assert b'FortressOS installer' in archive.extractfile(tool).read()
        manifest=extract('payload/payload.v1').read_bytes()
        assert len(manifest)==256 and manifest[:8]==b'FOSPAY01'
        assert hashlib.sha256(manifest[:224]).digest()==manifest[224:]
        for index,name in enumerate(ROLES[:4]):
            path=extract('payload/'+name)
            assert path.stat().st_size==SIZES[index]
            assert bytes.fromhex(digest(path))==manifest[48+index*32:80+index*32]
        data=temp/'live.ext4'
        with image.open('rb') as stream:
            stream.seek(parts[3][2]*512);data.write_bytes(stream.read((parts[3][3]-parts[3][2]+1)*512))
        subprocess.run(['e2fsck','-fn',str(data)],check=True,capture_output=True)
    return True

def create(output,kernel,bootstrap,installer):
    output=Path(output)
    assert not output.is_symlink()
    output=output.resolve()
    assert output.is_relative_to(ROOT/'bin') or output.is_relative_to(ROOT/'build')
    if output.exists():assert stat.S_ISREG(output.stat().st_mode)
    result=subprocess.run(['python3',str(ROOT/'scripts/create_installer_payload.py'),
        '--kernel',str(kernel),'--bootstrap',str(bootstrap)],cwd=ROOT,
        capture_output=True,text=True,check=True,timeout=600)
    templates=Path(result.stdout.strip().splitlines()[-1])
    work=build(templates,kernel,bootstrap,installer,combined=True,publish=False)
    verify(work/'fortress.img')
    # All GPT, FAT, payload hashes and source-preservation checks precede publication.
    output.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.NamedTemporaryFile(prefix='.'+output.name+'-',dir=output.parent,delete=False) as stream:
        pending=Path(stream.name)
        try:
            with (work/'fortress.img').open('rb') as source:shutil.copyfileobj(source,stream,1024**2)
            stream.flush()
        except BaseException:
            pending.unlink(missing_ok=True);raise
    try:pending.replace(output)
    finally:pending.unlink(missing_ok=True)
    manifest=json.loads((work/'manifest.json').read_text())
    assert digest(output)==manifest['artifacts']['fortress.img']['sha256']
    shutil.copyfile(work/'manifest.json',output.with_suffix('.manifest.json'))
    print(f'Combined USB image: {output} ({output.stat().st_size//1024**2} MiB)')
    print('Boot choices: FortressOS (Live USB); FortressOS (Installer)')
    print(f'Audited build: {work}')
    return work

if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('output',type=Path)
    parser.add_argument('--kernel',type=Path,default=ROOT/'bin/fortress.elf')
    parser.add_argument('--bootstrap',type=Path,default=ROOT/'bin/initramfs.tar')
    parser.add_argument('--installer',type=Path,default=ROOT/'build/tool-install-multi.elf')
    args=parser.parse_args();create(args.output,args.kernel,args.bootstrap,args.installer)
