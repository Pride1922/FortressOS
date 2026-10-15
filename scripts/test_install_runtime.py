#!/usr/bin/env python3
"""Actual runtime deployment to an exclusive disposable regular-file target."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import struct
import os,errno
import tempfile
import test_root_phase0 as fixture

ROOT=Path(__file__).resolve().parents[1]

def sparse_range(source,destination,offset,size):
    with source.open('rb') as src,destination.open('xb') as dst:
        # DrvFS truncate does not mark an NTFS file sparse automatically.
        if str(destination).startswith('/mnt/c/'):
            windows=subprocess.check_output(['wslpath','-w',str(destination)],text=True).strip()
            subprocess.run(['fsutil.exe','sparse','setflag',windows],check=True,capture_output=True)
        dst.truncate(size);position=offset;end=offset+size
        while position<end:
            try:position=os.lseek(src.fileno(),position,os.SEEK_DATA)
            except OSError as error:
                if error.errno==errno.ENXIO:break
                raise
            if position>=end:break
            stop=min(os.lseek(src.fileno(),position,os.SEEK_HOLE),end)
            src.seek(position);dst.seek(position-offset)
            while position<stop:
                data=src.read(min(1024**2,stop-position));assert data;dst.write(data);position+=len(data)


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--templates',type=Path);parser.add_argument('--username');parser.add_argument('--hostname');parser.add_argument('--capacity-mib',type=int,default=384);parser.add_argument('--artifacts',type=Path,default=ROOT/'bin');args=parser.parse_args()
    parent=ROOT/'build/root-phase4';parent.mkdir(parents=True,exist_ok=True)
    out=Path(tempfile.mkdtemp(prefix='runtime-',dir=parent))
    sources={str(p.relative_to(ROOT)):fixture.digest(p) for base in ('src','tests','scripts','user')
             for p in (ROOT/base).rglob('*') if p.is_file() and p.suffix in ('.c','.h','.inc','.py','.asm')}
    normal={str(p):fixture.digest(p) for p in (ROOT/'bin/fortress.iso',ROOT/'bin/fortress.img') if p.exists()}
    manifest={'result':'FAIL','scope':'actual runtime engine with regular-file transport; no guest/driver acceptance',
              'sources':sources,'normal_artifacts':normal,'commands':[]}
    def run(cmd):
        manifest['commands'].append(list(map(str,cmd)));return fixture.run(list(map(str,cmd)),out/'host.log',600)
    try:
        if args.templates is None:
            args.templates=Path(run(['python3','scripts/create_installer_payload.py','--kernel',args.artifacts/'fortress.elf','--bootstrap',args.artifacts/'initramfs.tar']).strip().splitlines()[-1])
        manifest['templates']=str(args.templates)
        binary=out/'host'
        run(['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-no-pie','-pthread',
            '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-Itests/ext4_host','-Itests/host','-Isrc/include','-Isrc/lib',
            '-Isrc/fs','-Isrc/drivers','-Isrc/mm','-Isrc/kernel','-Isrc/arch/x86_64','tests/install_runtime_host.c',
            'tests/ext4_fault_disk.c','src/kernel/install_runtime.c','src/kernel/install_log.c','src/kernel/root_boot.c','src/fs/gpt.c',
            'src/lib/crc32.c','src/lib/account_redo.c','src/lib/install_payload.c','src/lib/install_expand.c','src/lib/install_archive.c','src/lib/install_fat.c',
            'user/tools/userdb.c','user/tools/digest.c','-o',binary])
        # Native WSL I/O; retain output even when assertions/audits fail.
        with tempfile.TemporaryDirectory(prefix='fortress-install-runtime-') as tmp:
            disk=Path(tmp)/'target.img'
            try:
                run([binary,args.templates,disk,args.artifacts]+([args.username or 'operator'] if args.username or args.hostname else [])+([args.hostname,str(args.capacity_mib)] if args.hostname else []))
                with disk.open('rb') as stream:
                    stream.seek(135168*512+1024);sb=stream.read(1024);root_bytes=struct.unpack_from('<I',sb,4)[0]*4096
                for name,lba,size in (('esp.img',4096,64*1024**2),('root.img',135168,root_bytes)):
                    sparse_range(disk,out/name,lba*512,size)
                run(['fsck.fat','-n',out/'esp.img']);run(['e2fsck','-fn',out/'root.img'])
                for name in ('passwd','group','shadow','fortress-install.v1','hostname'):
                    path=out/name;run(['debugfs','-R',f'dump /etc/{name} {path}',out/'root.img']);assert path.is_file()
                log=out/'fortress-install.log'
                run(['debugfs','-R',f'dump /var/log/fortress-install.log {log}',out/'root.img'])
                record=log.read_text();assert 'result=installation complete' in record and 'stage=6 Verifying installed files' in record
                assert log.read_bytes().endswith(b'\n')
                for path,mode in (('/var','0700'),('/var/log','0700'),('/var/log/fortress-install.log','0600')):
                    attrs=run(['debugfs','-R',f'stat {path}',out/'root.img'])
                    assert 'Mode:  '+mode in attrs and 'User:     0' in attrs and 'Group:     0' in attrs
                assert '$5$' not in record and 'recovery' not in record.lower()
                assert (out/'hostname').read_bytes()==((args.hostname or 'fortress')+'\n').encode()
                username=args.username or 'operator'
                assert (out/'shadow').read_bytes().startswith(('root:!:::::::\n'+username+':$5$rounds=100000$').encode())
                assert (out/'passwd').read_text()==f'root:x:0:0::/root:/bin/shell\n{username}:x:1000:1000::/home/{username}:/bin/shell\n'
                assert (out/'group').read_text()==f'root:x:0:\n{username}:x:1000:\nwheel:x:10:{username}\n'
                home=run(['debugfs','-R',f'stat /home/{username}',out/'root.img'])
                assert 'Mode:  0700' in home and 'User:  1000' in home and 'Group:  1000' in home
                for name in ('bootstrap.tar','readiness.v1'):
                    run(['mcopy','-i',out/'esp.img','::boot/'+name,out/name])
                if disk.stat().st_size<=1024**3:manifest['target_sha256']=fixture.digest(disk)
                else:manifest['target_bytes']=disk.stat().st_size
            finally:
                if disk.exists():sparse_range(disk,out/'target.img',0,disk.stat().st_size)
        assert all(fixture.digest(ROOT/p)==value for p,value in sources.items())
        assert all(fixture.digest(Path(p))==value for p,value in normal.items())
        manifest['result']='PASS';print(out)
    finally:(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')


if __name__=='__main__':main()
