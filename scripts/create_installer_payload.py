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
import subprocess
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
        for name in ('bin','etc','dev','run','tmp','mnt','home','var','var/log','root','home/operator','usr','usr/bin','usr/share','usr/share/pkg'):
            (tree/name).mkdir(parents=True,exist_ok=True)
            if name not in metadata:
                metadata[name] = (0o755, 0, 0)
        (tree/'etc/passwd').write_text('root:x:0:0::/root:/bin/shell\noperator:x:1000:1000::/home/operator:/bin/shell\n')
        (tree/'etc/group').write_text('root:x:0:\noperator:x:1000:\nwheel:x:10:operator\n')
        (tree/'etc/shadow').write_text('root:!:::::::\noperator:!:::::::\n')
        if not (tree/'etc/crontab').is_file():
            (tree/'etc/crontab').write_text('# /etc/crontab - FortressOS crond configuration\n# <interval_minutes> <absolute_command_path> [args...]\n60 /sbin/logrotate --skip-if-locked /etc/logrotate.conf\n')
        metadata['etc/crontab'] = (0o644, 0, 0)
        (tree/'etc/watchtower').mkdir(parents=True, exist_ok=True)
        (tree/'etc/pkg').mkdir(parents=True, exist_ok=True)
        if not (tree/'etc/pkg/repository.conf').is_file():
            (tree/'etc/pkg/repository.conf').write_text('version=1\nurl=https://fortress.ameyerocha.com\n')
        if not (tree/'etc/pkg/trust.conf').is_file():
            (tree/'etc/pkg/trust.conf').write_text('version=1\nrepository=00010203-0405-0607-0809-0a0b0c0d0e0f\nroot_version=1\nplatform=4242424242424242424242424242424242424242424242424242424242424242\nbase_version=1\npublic_key=03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8\n')
        metadata['etc/pkg'] = (0o755, 0, 0)
        metadata['etc/pkg/repository.conf'] = (0o644, 0, 0)
        metadata['etc/pkg/trust.conf'] = (0o644, 0, 0)
        default_services = {
            'boot-log.service': 'Exec=/bin/boot-snapshot\nOrder=01\nType=simple\nRestart=no\n',
            'wifi.service': 'Exec=/bin/wifi auto\nOrder=08\nEnabled=yes\nType=simple\nRestart=no\nTimeoutStop=10\n',
            'dhcpc.service': 'Exec=/sbin/dhcpc -f\nOrder=10\nEnabled=yes\nRestart=on-failure\nRestartLimit=3\nRestartDelay=5\nTimeoutStop=5\n',
            'ntpd.service': 'Exec=/sbin/ntpd -f pool.ntp.org\nOrder=15\nEnabled=yes\nRestart=on-failure\nTimeoutStop=2\n',
            'sshd.service': 'Exec=/sbin/sshd -D\nOrder=20\nEnabled=yes\nRestart=on-failure\nRestartLimit=3\nRestartDelay=5\nTimeoutStop=10\n',
            'crond.service': 'Exec=/sbin/crond -f\nOrder=30\nType=simple\nRestart=on-failure\nTimeoutStop=2\n',
        }
        for svc_file, svc_content in default_services.items():
            target_svc = tree / 'etc/watchtower' / svc_file
            if not target_svc.is_file():
                target_svc.write_text(svc_content)
            metadata['etc/watchtower/' + svc_file] = (0o644, 0, 0)
        metadata.update({'tmp':(0o1777,0,0),'root':(0o700,0,0),'home/operator':(0o700,1000,1000),
                         'etc/passwd':(0o644,0,0),'etc/group':(0o644,0,0),'etc/shadow':(0o600,0,0)})
        root=out/'root.img'
        with root.open('xb') as f:f.truncate(fixture.ROOT_BYTES)
        run(['mke2fs','-q','-t','ext4','-b','4096','-I','256','-U',FS_UUID,'-O',fixture.FEATURES+',resize_inode',
             '-E','lazy_itable_init=0,resize=268435456','-m','0','-g','32768','-N','256','-J','size=4','-d',tree,root])
        # Leave metadata beyond 64 free descriptor-growth blocks. Remove the
        # resize-inode feature before shipping the restricted-profile seed.
        run(['tune2fs','-O','^resize_inode',root])
        repaired=subprocess.run(['e2fsck','-fy',str(root)],capture_output=True,text=True)
        (out/'producer.log').open('a').write(repaired.stdout+repaired.stderr)
        assert repaired.returncode in (0,1,2)
        commands=['set_inode_field / mode 040755','set_inode_field / uid 0','set_inode_field / gid 0']
        for path in sorted(tree.rglob('*')):
            name=path.relative_to(tree).as_posix();mode,uid,gid=metadata.get(name,(0o755 if path.is_dir() else 0o644,0,0))
            if name=='var' or name.startswith('var/'):
                mode,uid,gid=(0o700 if path.is_dir() else 0o600),0,0
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
