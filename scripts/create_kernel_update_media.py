#!/usr/bin/env python3
"""Create a two-slot rot13-1 baseline and a predecessor-bound update USB.

Run from an isolated complete build checkout. All writes are new regular files.
The caller provides two genuinely distinct kernel builds and the baseline archive.
"""
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
import create_rot13_installer as seed
import pkg_repository as repo
import pkg_build
from create_installer_media import build,digest,ROOT,elf_load_image
from build_kernel_updater import build as compile_updater

def main():
    p=argparse.ArgumentParser();p.add_argument('--reuse-seeded-root',type=Path);p.add_argument('--baseline',type=Path,required=True);p.add_argument('--candidate',type=Path,required=True);p.add_argument('--bootstrap',type=Path,required=True);p.add_argument('--installer',type=Path,required=True);a=p.parse_args()
    for path in (a.baseline,a.candidate,a.bootstrap,a.installer):assert path.is_file() and not path.is_symlink()
    assert digest(a.baseline)!=digest(a.candidate),'two distinct builds required'
    parent=ROOT/'build/kernel-update';parent.mkdir(parents=True,exist_ok=True)
    out=Path(tempfile.mkdtemp(prefix='media-',dir=parent));out.chmod(0o755)
    print(out,flush=True)
    candidate=out/'candidate.elf';subprocess.run(['objcopy','--strip-debug',str(a.candidate),str(candidate)],check=True)
    assert elf_load_image(a.candidate)==elf_load_image(candidate)
    a.candidate=candidate
    kernel=out/'baseline-bank.elf';data=a.baseline.read_bytes();assert len(data)<8*1024*1024
    kernel.write_bytes(data.ljust(8*1024*1024,b'\0'))
    result=subprocess.run(['python3','scripts/create_installer_payload.py','--banked-kernel','--kernel',str(kernel),'--bootstrap',str(a.bootstrap)],cwd=ROOT,capture_output=True,text=True,check=True)
    templates=Path(result.stdout.strip().splitlines()[-1]);templates.chmod(0o755)
    trust=(templates/'tree/etc/pkg/trust.conf').read_text()
    expected='version=1\nrepository=00010203-0405-0607-0809-0a0b0c0d0e0f\nroot_version=1\nplatform='+'42'*32+'\nbase_version=1\npublic_key='+seed.PUBLIC_KEY+'\n'
    assert trust==expected
    assert (templates/'tree/etc/pkg/repository.conf').read_text()==f'version=1\nurl={seed.URL}\n'
    if a.reuse_seeded_root:
        source=a.reuse_seeded_root.resolve();receipt=json.loads((source.parent/'manifest.json').read_text())
        assert receipt['package_seed']['build']==1 and receipt['package_seed']['artifact_sha256']==seed.V1_HASH
        assert digest(source)==receipt['roles']['root.img']['sha256']
        shutil.copyfile(source,templates/'root.img')
        wire=bytearray((templates/'payload.v1').read_bytes());wire[80:112]=bytes.fromhex(digest(source));wire[224:]=hashlib.sha256(wire[:224]).digest();(templates/'payload.v1').write_bytes(wire)
        receipt2=json.loads((templates/'manifest.json').read_text());receipt2['roles']['root.img']['sha256']=digest(source);receipt2['package_seed']=receipt['package_seed'];receipt2['reused_seed']=str(source)
        (templates/'manifest.json').write_text(json.dumps(receipt2,indent=2)+'\n')
    else:
        artifact=seed.download(seed.URL+'/objects/sha256/'+seed.V1_HASH+'.fpk',9*1024*1024)
        assert hashlib.sha256(artifact).hexdigest()==seed.V1_HASH;pkg_build.verify(artifact,sanitize=True)
        (out/'rot13-v1.fpk').write_bytes(artifact)
        now=int(time.time());key=Ed25519PrivateKey.from_private_bytes(bytes(range(32)))
        assert repo.public(key).hex()==seed.PUBLIC_KEY
        catalog,_=repo.catalog(bytes(range(16)),1,1,now-86400,now+7*86400,[artifact],[key]);(out/'catalog.v1').write_bytes(catalog)
        seed.seed(templates,out/'rot13-v1.fpk',out/'catalog.v1',out,now)
    baseline_media=build(templates,kernel,a.bootstrap,a.installer,combined=False,publish=False)
    updater=compile_updater(a.candidate,kernel,out/'updater-build')
    # The update frontend needs no persistent-root programs. A minimal archive
    # leaves room for its embedded candidate while preserving ordinary admission.
    bootstrap=out/'update-bootstrap.tar'
    with tarfile.open(bootstrap,'w',format=tarfile.USTAR_FORMAT) as archive:
        for name in ('bin','etc'):
            member=tarfile.TarInfo(name);member.type=tarfile.DIRTYPE;member.mode=0o755;archive.addfile(member)
    update_media=build(templates,kernel,bootstrap,updater,combined=False,publish=False)
    stamp=out.name.removeprefix('media-');outputs={}
    for name,media in (('baseline',baseline_media),('update-usb',update_media)):
        destination=ROOT/'bin'/f'fortress-kernel-{name}-{stamp}.img';assert not destination.exists()
        shutil.copyfile(media/'fortress-installer.img',destination);outputs[name]={'path':str(destination),'sha256':digest(destination)}
    record={'result':'BUILD_PASS_RUNTIME_PENDING','outputs':outputs,'templates':str(templates),'baseline_media':str(baseline_media),'update_media':str(update_media),
            'baseline_kernel':str(kernel),'baseline_kernel_sha256':digest(kernel),'candidate_kernel':str(a.candidate),'candidate_sha256':digest(a.candidate),
            'bootstrap_sha256':digest(a.bootstrap),'root_seed_sha256':digest(templates/'root.img'),'scope':'offline kernel-only predecessor-bound prototype; no physical writes'}
    (out/'manifest.json').write_text(json.dumps(record,indent=2)+'\n');print(json.dumps(record,indent=2),flush=True)
if __name__=='__main__':main()
