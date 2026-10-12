#!/usr/bin/env python3
"""Actual runtime deployment to an exclusive disposable regular-file target."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import test_root_phase0 as fixture

ROOT=Path(__file__).resolve().parents[1]


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--templates',type=Path);parser.add_argument('--username');args=parser.parse_args()
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
            args.templates=Path(run(['python3','scripts/create_installer_payload.py']).strip().splitlines()[-1])
        manifest['templates']=str(args.templates)
        binary=out/'host'
        run(['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-no-pie','-pthread',
            '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-Itests/ext4_host','-Itests/host','-Isrc/include','-Isrc/lib',
            '-Isrc/fs','-Isrc/drivers','-Isrc/mm','-Isrc/kernel','-Isrc/arch/x86_64','tests/install_runtime_host.c',
            'tests/ext4_fault_disk.c','src/kernel/install_runtime.c','src/kernel/root_boot.c','src/fs/gpt.c',
            'src/lib/crc32.c','src/lib/account_redo.c','src/lib/install_payload.c','src/lib/install_archive.c','src/lib/install_fat.c',
            'user/tools/userdb.c','user/tools/digest.c','-o',binary])
        # Native WSL I/O; retain output even when assertions/audits fail.
        with tempfile.TemporaryDirectory(prefix='fortress-install-runtime-') as tmp:
            disk=Path(tmp)/'target.img'
            try:
                run([binary,args.templates,disk,ROOT/'bin']+([args.username] if args.username else []))
                for name,lba,size in (('esp.img',4096,64*1024**2),('root.img',135168,256*1024**2)):
                    with disk.open('rb') as source,(out/name).open('xb') as destination:
                        source.seek(lba*512)
                        while size:
                            data=source.read(min(1024**2,size));assert data;destination.write(data);size-=len(data)
                run(['fsck.fat','-n',out/'esp.img']);run(['e2fsck','-fn',out/'root.img'])
                for name in ('passwd','group','shadow','fortress-install.v1','hostname'):
                    path=out/name;run(['debugfs','-R',f'dump /etc/{name} {path}',out/'root.img']);assert path.is_file()
                assert (out/'hostname').read_bytes()==b'fortress\n'
                username=args.username or 'operator'
                assert (out/'shadow').read_bytes().startswith(('root:!:::::::\n'+username+':$5$rounds=100000$').encode())
                assert (out/'passwd').read_text()==f'root:x:0:0::/root:/bin/shell\n{username}:x:1000:1000::/home/{username}:/bin/shell\n'
                assert (out/'group').read_text()==f'root:x:0:\n{username}:x:1000:\nwheel:x:10:{username}\n'
                home=run(['debugfs','-R',f'stat /home/{username}',out/'root.img'])
                assert 'Mode:  0700' in home and 'User:  1000' in home and 'Group:  1000' in home
                for name in ('bootstrap.tar','readiness.v1'):
                    run(['mcopy','-i',out/'esp.img','::boot/'+name,out/name])
                manifest['target_sha256']=fixture.digest(disk)
            finally:
                if disk.exists():shutil.copyfile(disk,out/'target.img')
        assert all(fixture.digest(ROOT/p)==value for p,value in sources.items())
        assert all(fixture.digest(Path(p))==value for p,value in normal.items())
        manifest['result']='PASS';print(out)
    finally:(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')


if __name__=='__main__':main()
