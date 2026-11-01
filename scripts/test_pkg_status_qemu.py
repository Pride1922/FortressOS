#!/usr/bin/env python3
"""Private normal-kernel installed root: acquire/release vs history-style writes."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import time
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
import pkg_build
import pkg_repository as repo
import test_pkg_upgrade_qemu as qemu
import test_root_phase0 as fixture
from test_root_phase2 import extract_root
ROOT=fixture.REPO


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--expect-old-failure',action='store_true')
    parser.add_argument('--firmware',choices=['bios','uefi'],nargs='+',default=['bios','uefi']);args=parser.parse_args()
    parent=ROOT/'build/pkg-status';parent.mkdir(parents=True,exist_ok=True)
    out=Path(tempfile.mkdtemp(prefix='qemu-',dir=parent));out.chmod(0o755)
    print('Evidence:',out,flush=True)
    workspace=out/'workspace';(workspace/'bin').mkdir(parents=True)
    shutil.copyfile(ROOT/'bin/fortress.elf',workspace/'bin/fortress.elf')
    (workspace/'limine').symlink_to(ROOT/'limine',target_is_directory=True)
    flags=['gcc','-std=c11','-Os','-Wall','-Wextra','-Werror','-ffreestanding','-fno-pie','-fno-builtin',
           '-fno-stack-protector','-mno-red-zone','-mgeneral-regs-only','-Wframe-larger-than=1024',
           '-fstack-usage','-ffunction-sections','-fdata-sections','-Isrc/include','-Isrc/fs','-Iuser/tools']
    for name,source in [('probe','tests/pkg_status_probe_user.c'),('common','user/tools/common.c')]:
        subprocess.run(flags+['-c',source,'-o',str(out/(name+'.o'))],cwd=ROOT,check=True)
    subprocess.run(['nasm','-f','elf64','-DTOOL_ENTRY=pkg_status_probe_main','user/tools/start.asm','-o',str(out/'start.o')],cwd=ROOT,check=True)
    subprocess.run(['ld','-m','elf_x86_64','-nostdlib','-static','-z','noexecstack','--gc-sections','-T','user/shell.ld',
                    str(out/'start.o'),str(out/'probe.o'),str(out/'common.o'),'-o',str(out/'probe.elf')],cwd=ROOT,check=True)
    subprocess.run(['sh','examples/pkg/rot13/build.sh'],cwd=ROOT,check=True)
    artifact=pkg_build.build_spec(ROOT/'examples/pkg/rot13/v1.json');key=Ed25519PrivateKey.generate()
    now=int(time.time());wire,_=repo.catalog(bytes(range(16)),1,1,min(now,1793448000)-86400,now+30*86400,[artifact],[key])
    trust='version=1\nrepository=00010203-0405-0607-0809-0a0b0c0d0e0f\nroot_version=1\nplatform='+'42'*32+'\nbase_version=1\npublic_key='+repo.public(key).hex()+'\n'
    with tarfile.open(ROOT/'bin/initramfs.tar') as src,tarfile.open(workspace/'bin/initramfs.tar','w',format=tarfile.USTAR_FORMAT) as dst:
        for member in src:dst.addfile(member,src.extractfile(member) if member.isfile() else None)
        directory=tarfile.TarInfo('root/pkg-status-seed');directory.type=tarfile.DIRTYPE;directory.mode=0o700;dst.addfile(directory)
        for name,data,mode in [('bin/pkg-status-probe',(out/'probe.elf').read_bytes(),0o755),
                               ('root/pkg-status-seed/artifact.fpk',artifact,0o600),('root/pkg-status-seed/catalog.v1',wire,0o600)]:
            member=tarfile.TarInfo(name);member.mode=mode;member.size=len(data);dst.addfile(member,io.BytesIO(data))
    result={'result':'FAIL','scope':'normal kernel, private probe, SMP=4 acquire/release vs independent ordinary VFS writes',
            'kernel_sha256':fixture.digest(workspace/'bin/fortress.elf'),'cases':[]}
    try:
        baseline=qemu.prepare(workspace,out,trust,18080)
        for firmware in args.firmware:
            disk=out/(firmware+'.img');shutil.copyfile(baseline,disk)
            guest=qemu.Guest(out,disk,firmware,4,firmware,'kvm')
            try:
                guest.login();guest.root()
                try:guest.command('/bin/pkg install rot13 /root/pkg-status-seed')
                except AssertionError:
                    if not args.expect_old_failure:raise
                    # Startup services can overlap the very first acquire,
                    # before the deliberate writer even starts. Preserve the
                    # quarantined image; do not attempt recovery or a mutation.
                    assert '[FAIL] NVMe I/O CQ CID mismatch!' in guest.text()
                    result['cases'].append({'firmware':firmware,'result':'EXPECTED_FAILURE',
                        'phase':'initial package setup','diagnostic':'NVMe CQ CID mismatch; driver quiesced'})
                    continue
                if args.expect_old_failure:
                    try:guest.command('/bin/pkg-status-probe')
                    except AssertionError:
                        assert guest.actions[-1]['command']=='/bin/pkg-status-probe' and guest.actions[-1]['exit']==1
                    else:raise AssertionError('old QEMU race did not reproduce')
                    assert 'PKG_STATUS_STRESS failures=0' not in guest.text() and 'PKG_STATUS_STRESS failures=' in guest.text()
                else:
                    text=guest.command('/bin/pkg-status-probe')
                    assert 'PKG_STATUS_STRESS failures=0 first=0' in text
                guest.command('/bin/pkg status');guest.shutdown()
            finally:guest.close()
            root=out/(firmware+'.root.img');extract_root(disk,root)
            fixture.run(['e2fsck','-fn',str(root)],out/(firmware+'.fsck.log'))
            result['cases'].append({'firmware':firmware,'result':'EXPECTED_FAILURE' if args.expect_old_failure else 'PASS'})
        result['result']='EXPECTED_FAILURE' if args.expect_old_failure else 'PASS'
        print(result['result'],out,flush=True)
    except BaseException as error:
        result['error']=str(error);raise
    finally:(out/'manifest.json').write_text(json.dumps(result,indent=2)+'\n')


if __name__=='__main__':main()
