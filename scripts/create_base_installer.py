#!/usr/bin/env python3
"""One complete installer with repository-updatable base slots. No boot tests.

Only regular-file artifacts inside bin/build are written; no devices or secrets.
"""
import argparse
import io
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
from build_base_package import bootstrap, BANK
from seed_desktop_packages import build_packages, seed
from publish_desktop_bundle import publish_bundle
from create_installer_media import ROOT, build, digest, elf_load_image


def create(output, sequence=1, abi=1, platform=bytes.fromhex('42'*32), app_sequence=1):
    output = Path(output)
    if output.is_symlink() or output.resolve().parent != (ROOT/'bin').resolve():
        raise ValueError('output must be a regular bin image')
    out = Path(tempfile.mkdtemp(prefix='base-installer-',dir=ROOT/'build/root-phase4'))
    kernel = out/'kernel.elf'
    subprocess.run(['objcopy','--strip-debug',str(ROOT/'bin/fortress.elf'),str(kernel)],check=True)
    assert elf_load_image(kernel)==elf_load_image(ROOT/'bin/fortress.elf')
    data=kernel.read_bytes();assert len(data)<=BANK;kernel.write_bytes(data.ljust(BANK,b'\0'))
    archive=out/'bootstrap.tar'
    marked=bootstrap((ROOT/'bin/initramfs.tar').read_bytes(),sequence,abi,platform)
    archive.write_bytes(marked)
    completed=subprocess.run(['python3',str(ROOT/'scripts/create_installer_payload.py'),'--base-system',
        '--kernel',str(kernel),'--bootstrap',str(archive)],cwd=ROOT,capture_output=True,text=True,check=True)
    (out/'payload.log').write_text(completed.stdout+completed.stderr)
    templates=Path(completed.stdout.strip().splitlines()[-1])
    bundle=build_packages(out/'packages',sequence=app_sequence,abi=abi,platform=platform)
    seed(templates,bundle,out/'package-seed')
    media=build(templates,kernel,archive,ROOT/'build/tool-install-multi.elf',combined=False,publish=False)
    pending=output.with_suffix('.img.new');assert not pending.exists()
    shutil.move(media/'fortress-installer.img',pending);pending.replace(output)
    bundle,package_directory=publish_bundle(bundle)
    source_paths=subprocess.check_output(['git','ls-files','--cached','--others','--exclude-standard','-z'],cwd=ROOT).split(b'\0')
    source_hashes={str(Path(p.decode())):digest(ROOT/p.decode()) for p in source_paths if p and (ROOT/p.decode()).is_file()}
    report={'result':'BUILD_PASS_RUNTIME_PENDING','scope':'complete A/B base repository-update baseline; no physical writes or QEMU campaign',
        'image':str(output),'bytes':output.stat().st_size,'sha256':digest(output),'build':sequence,'abi':abi,
        'factory_packages':bundle,'package_directory':str(package_directory),'platform':platform.hex(),'kernel_sha256':digest(kernel),'bootstrap_sha256':digest(archive),
        'source_commit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
        'source_hashes':source_hashes,
        'source_status':subprocess.check_output(['git','status','--short'],cwd=ROOT,text=True),
        'templates':str(templates),'media':str(media)}
    output.with_suffix('.manifest.json').write_text(json.dumps(report,indent=2)+'\n')
    (out/'manifest.json').write_text(json.dumps(report,indent=2)+'\n')
    # Keep the factory bootstrap and kernel for the first publisher release, but
    # remove duplicate transport images after the audited final image is retained.
    for directory in (templates,media,media/'iso/payload'):
        for p in directory.iterdir():
            if p.is_file() and p.suffix in ('.img','.iso'):p.unlink()
    print(json.dumps(report,indent=2));return out


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path,default=ROOT/'bin/fortress-base-installer.img')
    p.add_argument('--app-build',type=int,default=1);p.add_argument('--build',type=int,default=1);p.add_argument('--base-abi',type=int,default=1)
    p.add_argument('--platform-id',default='42'*32)
    a=p.parse_args();create(a.output,a.build,a.base_abi,bytes.fromhex(a.platform_id),a.app_build)
