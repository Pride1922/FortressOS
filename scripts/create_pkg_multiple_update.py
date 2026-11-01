#!/usr/bin/env python3
"""Kernel-only update bound to the accepted laptop candidate; separate CLI asset.

Creates new regular files only. Never installs a root or opens a physical disk.
The existing rot13 seed, old media and installed base userland stay untouched.
"""
import argparse,hashlib,json,shutil,subprocess,tarfile,tempfile
from pathlib import Path
from build_kernel_updater import build as updater
from create_installer_media import build as media,ROOT,elf_load_image

def main():
    p=argparse.ArgumentParser();p.add_argument('--predecessor-manifest',type=Path,default=ROOT/'build/kernel-update/media-snul1hju/manifest.json');a=p.parse_args()
    previous=json.loads(a.predecessor_manifest.read_text());old=Path(previous['candidate_kernel']);assert old.is_file() and hashlib.sha256(old.read_bytes()).hexdigest()==previous['candidate_sha256']
    out=Path(tempfile.mkdtemp(prefix='pkg-multiple-',dir=ROOT/'build/kernel-update'));print(out,flush=True)
    candidate=out/'candidate.elf';subprocess.run(['objcopy','--strip-debug',str(ROOT/'bin/fortress.elf'),str(candidate)],check=True);assert elf_load_image(candidate)==elf_load_image(ROOT/'bin/fortress.elf')
    predecessor=out/'predecessor-bank.elf';predecessor.write_bytes(old.read_bytes().ljust(8*1024*1024,b'\0'))
    executable=updater(candidate,predecessor,out/'updater')
    bootstrap=out/'bootstrap.tar'
    with tarfile.open(bootstrap,'w',format=tarfile.USTAR_FORMAT) as archive:
        for name in ('bin','etc'):
            info=tarfile.TarInfo(name);info.type=tarfile.DIRTYPE;info.mode=0o755;archive.addfile(info)
    packaged=media(Path(previous['templates']),predecessor,bootstrap,executable,combined=False,publish=False)
    image=ROOT/'bin'/f'fortress-{out.name}-update-usb.img';assert not image.exists();shutil.copyfile(packaged/'fortress-installer.img',image)
    cli=out/'pkg-next.elf';shutil.copyfile(ROOT/'build/pkg/pkg.elf',cli)
    record={'result':'BUILD_PASS_RUNTIME_PENDING','scope':'predecessor-bound kernel-only update; separate CLI; no physical I/O','predecessor_manifest':str(a.predecessor_manifest.resolve()),'predecessor_sha256':hashlib.sha256(predecessor.read_bytes()).hexdigest(),'candidate_kernel':str(candidate),'candidate_sha256':hashlib.sha256(candidate.read_bytes()).hexdigest(),'update_media':str(packaged),'update_usb':str(image),'update_usb_sha256':hashlib.sha256(image.read_bytes()).hexdigest(),'cli':str(cli),'cli_sha256':hashlib.sha256(cli.read_bytes()).hexdigest(),'sources':{name:hashlib.sha256((ROOT/name).read_bytes()).hexdigest() for name in ('src/fs/pkg_executor.inc','src/fs/pkg_executor.h','src/fs/pkg_upgrade.inc','user/pkg/system_cli.c','user/pkg/fetch.c','user/pkg/repository.h')}}
    (out/'manifest.json').write_text(json.dumps(record,indent=2)+'\n');print(json.dumps(record,indent=2),flush=True)
if __name__=='__main__':main()
