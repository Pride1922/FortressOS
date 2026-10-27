#!/usr/bin/env python3
"""SATA/AHCI installation, installed-root I/O and mixed-NVMe isolation.

All guest data disks are newly created regular files under a private directory.
No physical disks are opened. QEMU argv validation/cleanup and Linux filesystem
audits are shared with the existing installer and installed-root acceptance gates.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import create_installer_media
import test_installer as installer

ROOT=installer.ROOT


def refresh_kernel(disk,directory):
    """Replace only boot artifacts on our disposable installed-disk copy."""
    assert disk.resolve().parent==directory.resolve() and not disk.is_symlink()
    with disk.open('rb') as stream:
        stream.seek(2*512+32)
        assert int.from_bytes(stream.read(8),'little')==4096
    fat=f'{disk}@@{4096*512}'
    ready=directory/'refreshed-readiness.v1'
    subprocess.run(['mcopy','-i',fat,'::boot/readiness.v1',str(ready)],check=True)
    prefix=ready.read_bytes().rstrip(b'\0').split(b'checksum=',1)[0]
    digest=hashlib.sha256((ROOT/'bin/fortress.elf').read_bytes()).hexdigest().encode()
    prefix,count=re.subn(rb'kernel_sha256=[0-9a-f]{64}\n',b'kernel_sha256='+digest+b'\n',prefix)
    assert count==1
    record=prefix+b'checksum='+hashlib.sha256(prefix).hexdigest().encode()+b'\n'
    assert len(record)<512
    ready.write_bytes(record.ljust(512,b'\0'))
    for source,name in ((ROOT/'bin/fortress.elf','fortress.elf'),(ready,'readiness.v1')):
        subprocess.run(['mcopy','-o','-i',fat,str(source),'::boot/'+name],check=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--media',type=Path)
    parser.add_argument('--firmware',choices=('bios','uefi','all'),default='all')
    parser.add_argument('--accel',choices=('kvm','tcg'),default='tcg')
    parser.add_argument('--quick',action='store_true',help='SATA-only; omit mixed inventory and duplicate-PARTUUID rejection')
    parser.add_argument('--mixed-only',action='store_true',help='Run only mixed-transport selection and recovery')
    parser.add_argument('--installed',type=Path,help='Reuse a disposable installed fixture; refresh kernel and test root I/O/shutdown')
    parser.add_argument('--installed-transport',choices=('sata','nvme'),default='sata')
    args=parser.parse_args()
    assert not (args.quick and args.mixed_only)
    parent=ROOT/'build/root-phase4';parent.mkdir(parents=True,exist_ok=True)
    out=Path(tempfile.mkdtemp(prefix='sata-',dir=parent))
    manifest={'result':'FAIL','scope':__doc__,'cases':[]}
    # Match the established acceptance runners: active QEMU data files live on
    # native Linux storage, not DrvFS alongside concurrently rebuilt media.
    temporary=tempfile.TemporaryDirectory(prefix='fortress-sata-disks-')
    work=Path(temporary.name)
    installer.ACCEL=args.accel
    try:
        if args.installed:
            disk=args.installed.resolve()
            assert disk.is_relative_to(parent) and disk.is_file() and not args.installed.is_symlink()
            manifest['scope']=args.installed_transport+' installed-root regression; refreshed kernel on copied disposable fixture'
            manifest['installed_input']=str(disk)
            manifest['kernel_sha256']=installer.fixture.digest(ROOT/'bin/fortress.elf')
            for firmware in (('bios','uefi') if args.firmware=='all' else (args.firmware,)):
                case=work/(firmware+'-refresh');case.mkdir();copy=case/'target.img';shutil.copyfile(disk,copy)
                refresh_kernel(copy,case)
                for cpus in (1,4):
                    installer.root_gate.guest(case,copy,firmware,cpus,0,'eight888','recover8',manifest,
                                             accel=args.accel,transport=args.installed_transport)
            manifest['result']='PASS';return
        media=args.media
        if media is None:
            result=subprocess.run(['python3','scripts/create_installer_payload.py'],cwd=ROOT,
                                  capture_output=True,text=True,check=True)
            (out/'templates.log').write_text(result.stdout+result.stderr)
            templates=Path(result.stdout.strip().splitlines()[-1])
            media=create_installer_media.build(templates,ROOT/'bin/fortress.elf',ROOT/'bin/initramfs.tar',
                    ROOT/'build/tool-install-multi.elf',multi=True,publish=False)
        media=media.resolve();assert media.is_relative_to(parent)
        manifest['media']=str(media)
        print('MEDIA',media,flush=True)
        for firmware in (('bios','uefi') if args.firmware=='all' else (args.firmware,)):
            for mixed in ((True,) if args.mixed_only else (False,) if args.quick else (False,True)):
                case=work/(firmware+('-mixed' if mixed else '-sata'));case.mkdir()
                print('START SATA installation',firmware,'mixed='+str(mixed),flush=True)
                disk=installer.install(case,media,firmware,384,manifest,transport='sata',
                    disposable_gate=False,multi_selection=2 if mixed else 0,
                    operator='eight888',recovery='recover8')
                installer.audit(disk,case,media/'iso/boot/fortress.elf','eight888','recover8',manifest)
                unselected=case/'excluded.img' if mixed else None
                before=installer.fixture.digest(unselected) if mixed else None
                for cpus in (1,4):
                    installer.root_gate.guest(case,disk,firmware,cpus,0,'eight888','recover8',manifest,
                        accel=args.accel,transport='sata',extra=unselected,extra_first=mixed)
                if mixed:
                    assert installer.fixture.digest(unselected)==before,'unselected NVMe changed'
                    # A cloned PARTUUID on another transport must never select a
                    # writable root. Both backends must remain byte-identical.
                    shutil.copyfile(disk,unselected)
                    snapshots=[installer.fixture.digest(p) for p in (disk,unselected)]
                    installer.root_gate.guest(case,disk,firmware,1,'duplicate-sata',
                        'eight888','recover8',manifest,recovery=True,accel=args.accel,
                        transport='sata',extra=unselected,extra_first=True)
                    assert snapshots==[installer.fixture.digest(p) for p in (disk,unselected)]
                print('PASS SATA install/root/shutdown/Linux audits',firmware,'mixed='+str(mixed),flush=True)
        manifest['result']='PASS'
    except BaseException as error:
        manifest['error']=repr(error);raise
    finally:
        try:
            shutil.copytree(work,out/'guests',dirs_exist_ok=True,ignore=shutil.ignore_patterns('*.sock'))
        finally:
            temporary.cleanup()
            (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
            print(out,flush=True)


if __name__=='__main__':main()
