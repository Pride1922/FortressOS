#!/usr/bin/env python3
"""Build immutable Phase4 templates in a new workspace build directory.
No installer admission, target personalization, physical I/O or ready installation.
Normal ISO/raw-image outputs are read only and audited unchanged.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import tarfile
import tempfile
import uuid
import test_root_phase0 as fixture

ROOT=Path(__file__).resolve().parents[1]
FS_UUID='813ea876-9933-4e1c-ae62-2f6c0f90a441'


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--kernel',type=Path,default=ROOT/'bin/fortress.elf')
    parser.add_argument('--bootstrap',type=Path,default=ROOT/'bin/initramfs.tar')
    args=parser.parse_args()
    assert args.kernel.is_file() and args.bootstrap.is_file() and args.bootstrap.stat().st_size<=8*1024*1024
    parent=ROOT/'build/root-phase4';parent.mkdir(parents=True,exist_ok=True)
    out=Path(tempfile.mkdtemp(prefix='templates-',dir=parent))
    sources=[args.kernel,args.bootstrap,ROOT/'limine/limine',ROOT/'limine/limine-bios.sys',ROOT/'limine/BOOTX64.EFI',
             ROOT/'scripts/create_installer_payload.py',ROOT/'src/lib/install_fat.c',ROOT/'src/include/install_fat.h',
             ROOT/'user/tools/digest.c',ROOT/'tests/install_fat_validate_host.c']
    hashes={str(p.resolve()):fixture.digest(p) for p in sources}
    normal={str(p):fixture.digest(p) for p in (ROOT/'bin/fortress.iso',ROOT/'bin/fortress.img') if p.is_file()}
    manifest={'scope':'immutable host templates, not installer media or installation acceptance','result':'FAIL',
              'sources':hashes,'normal_artifacts':normal,'filesystem_uuid':FS_UUID,'commands':fixture.COMMANDS}
    try:
        run=lambda cmd:fixture.run([str(x) for x in cmd],out/'producer.log',300)
        tree=out/'tree';tree.mkdir();metadata={};seen=set()
        with tarfile.open(args.bootstrap) as archive:
            for member in archive:
                name=member.name.rstrip('/');parts=Path(name).parts
                assert name and not name.startswith('/') and '..' not in parts and len(seen)<510 and name not in seen
                assert re.fullmatch(r'[A-Za-z0-9_./-]+',name) and len(name)<256 and all(len(p)<64 for p in parts)
                assert (member.isfile() or member.isdir()) and member.uid==member.gid==0 and parts[0]!='bootstrap'
                seen.add(name);target=tree/name;assert target.resolve().is_relative_to(tree.resolve())
                if member.isdir():target.mkdir(parents=True,exist_ok=True)
                else:
                    target.parent.mkdir(parents=True,exist_ok=True)
                    with target.open('xb') as f:shutil.copyfileobj(archive.extractfile(member),f)
                metadata[name]=(member.mode,member.uid,member.gid)
        for name in ('bin','etc','dev','run','tmp','mnt','home','var','root','home/operator'):
            (tree/name).mkdir(parents=True,exist_ok=True)
        (tree/'etc/passwd').write_text('root:x:0:0::/root:/bin/shell\noperator:x:1000:1000::/home/operator:/bin/shell\n')
        (tree/'etc/group').write_text('root:x:0:\noperator:x:1000:\nwheel:x:10:operator\n')
        (tree/'etc/shadow').write_text('root:!:::::::\noperator:!:::::::\n')
        metadata.update({'tmp':(0o1777,0,0),'root':(0o700,0,0),'home/operator':(0o700,1000,1000),
                         'etc/passwd':(0o644,0,0),'etc/group':(0o644,0,0),'etc/shadow':(0o600,0,0)})
        root=out/'root.img'
        with root.open('xb') as f:f.truncate(fixture.ROOT_BYTES)
        run(['mke2fs','-q','-t','ext4','-b','4096','-I','256','-U',FS_UUID,'-O',fixture.FEATURES,
             '-E','lazy_itable_init=0','-m','0','-g','4096','-N','4096','-J','size=4','-d',tree,root])
        commands=['set_inode_field / mode 040755','set_inode_field / uid 0','set_inode_field / gid 0']
        for path in sorted(tree.rglob('*')):
            name=path.relative_to(tree).as_posix();mode,uid,gid=metadata.get(name,(0o755 if path.is_dir() else 0o644,0,0))
            mode|=0o40000 if path.is_dir() else 0o100000
            commands.extend([f'set_inode_field /{name} mode 0{mode:o}',f'set_inode_field /{name} uid {uid}',f'set_inode_field /{name} gid {gid}'])
        attrs=out/'attrs';attrs.write_text('\n'.join(commands)+'\n');run(['debugfs','-w','-f',attrs,root])
        fixture.initialize_journal(root,out);run(['e2fsck','-fn',root])
        # Reserve slots before hashing. A locked account template has no usable
        # credential and no root readiness/installation manifest.
        bootstrap=out/'bootstrap.slot';config=out/'config.slot';readiness=out/'readiness.slot'
        for path,size in ((bootstrap,8*1024*1024),(config,4096),(readiness,512)):
            with path.open('xb') as f:f.truncate(size)
        esp=out/'esp.img';fixture.create_esp_partition(esp,None,ROOT/'limine',args.kernel,bootstrap,config,None)
        run(['mren','-i',esp,'::boot/initramfs.tar','bootstrap.tar'])
        run(['mdel','-i',esp,'::initramfs.tar'])
        run(['mcopy','-i',esp,readiness,'::boot/readiness.v1'])
        data=bytearray(esp.read_bytes());fi=struct.unpack_from('<H',data,48)[0]*512;backup=struct.unpack_from('<H',data,50)[0]*512
        data[backup+fi:backup+fi+512]=data[fi:fi+512];esp.write_bytes(data)
        binary=out/'fat-validator';run(['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-no-pie',
             '-Isrc/include','src/lib/install_fat.c','tests/install_fat_validate_host.c','user/tools/digest.c','-o',binary])
        run([binary,esp]);run(['fsck.fat','-n',esp])
        capsules=[]
        for size in (384,512):
            disk,record=fixture.assemble(out,esp,root,size,uuid.UUID('12345678-1234-4234-8234-123456789abc'))
            with disk.open('rb') as f:
                mbr=f.read(440);f.seek(2048*512);bios=f.read(1024*1024)
            capsules.append((mbr,bios));manifest.setdefault('capsule_comparisons',[]).append(record)
        assert capsules[0]==capsules[1],'pinned BIOS stage placement depends on disk capacity'
        (out/'bios.img').write_bytes(capsules[0][1]);(out/'mbr.bin').write_bytes(capsules[0][0])
        role_paths=[esp,root,out/'bios.img',out/'mbr.bin'];wire=bytearray(256)
        struct.pack_into('<8sII4Q',wire,0,b'FOSPAY01',1,256,*(p.stat().st_size for p in role_paths))
        for index,path in enumerate(role_paths):wire[48+index*32:80+index*32]=bytes.fromhex(fixture.digest(path))
        wire[224:]=hashlib.sha256(wire[:224]).digest();(out/'payload.v1').write_bytes(wire)
        shutil.copyfile(args.bootstrap,out/'bootstrap-source.tar')
        assert all(fixture.digest(Path(p))==h for p,h in hashes.items())
        assert all(fixture.digest(Path(p))==h for p,h in normal.items())
        manifest['roles']={p.name:{'bytes':p.stat().st_size,'sha256':fixture.digest(p)} for p in role_paths}
        manifest['result']='PASS';print(out)
    finally:(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')


if __name__=='__main__':main()
