#!/usr/bin/env python3
"""Focused desktop factory packaging and signed upgrade checks. No QEMU/devices.

Factory seeding uses image authority and no keys. Ephemeral TEST signers are used
only on a separate host copy to exercise ordinary authenticated upgrade paths.
"""
import gzip
import hashlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from create_installer_media import ROOT,digest
from seed_desktop_packages import build_packages,seed,run,ORDER
from build_base_package import bootstrap,BANK
from pkg_build import encode
from pkg_repository import catalog,public


def main():
    out=Path(tempfile.mkdtemp(prefix='desktop-packages-',dir=ROOT/'build'));templates=None
    report={'result':'INCOMPLETE','scope':'actual factory FPK/inventory/VFS/EXT4 and signed upgrade callbacks; host block/credential adapters',
            'qemu':'NOT RUN','physical':'NOT RUN'}
    try:
        platform=bytes.fromhex('42'*32)
        archive=out/'base.tar';archive.write_bytes(bootstrap((ROOT/'bin/initramfs.tar').read_bytes(),1,1,platform))
        with tarfile.open(archive) as tar:
            names=set(tar.getnames());assert 'bin/compositor' in names
            assert not {'bin/notepad','bin/files','bin/terminal','bin/calc'}&names
        # Template generation uses the stripped base archive; full live media's
        # archive is independently checked for the original bundled clients.
        with tarfile.open(ROOT/'bin/initramfs.tar') as tar:assert {'bin/notepad','bin/files','bin/terminal','bin/calc'}<=set(tar.getnames())
        result=subprocess.run(['python3','scripts/create_installer_payload.py','--kernel',str(ROOT/'bin/fortress.elf'),
            '--bootstrap',str(archive)],cwd=ROOT,capture_output=True,text=True,check=True,timeout=600)
        (out/'payload.log').write_text(result.stdout+result.stderr);templates=Path(result.stdout.strip().splitlines()[-1])
        bundle=build_packages(out/'packages')
        # New member bytes, not merely a changed catalog revision, are activated.
        new=[];paths=[]
        for record in bundle['packages']:
            data=Path(record['path']).read_bytes();mn=int.from_bytes(data[20:24],'little')
            with tarfile.open(fileobj=io.BytesIO(gzip.decompress(data[128+mn:]))) as tar:
                members=[]
                for m in tar:
                    value=tar.extractfile(m).read()
                    role=1 if m.name.startswith('bin/') else 2
                    if role==1:value+=b'\nHOST TEST desktop release 2\n'
                    members.append((m.name,role,m.mode,value))
            value=encode(record['name'],record['command'],record['version'],platform,2,1,1,members)
            path=out/(record['name']+'-2.fpk');path.write_bytes(value);new.append(value);paths.append(path)
        key=Ed25519PrivateKey.generate() # TEST-only; never persisted/provisioned
        wire,_=catalog(bytes(range(16)),1,5,1792929500,1793029600,new,[key])
        cat=out/'test-catalog.v1';cat.write_bytes(wire)
        trust=out/'test-trust.conf';trust.write_text('version=1\nrepository=00010203-0405-0607-0809-0a0b0c0d0e0f\nroot_version=1\nplatform='+platform.hex()+'\nbase_version=1\npublic_key='+public(key).hex()+'\n')
        seed_out=out/'seed';seed(templates,bundle,seed_out,[trust,cat,*paths])
        run(['e2fsck','-fn',seed_out/'seeded-root.img.upgraded.img'],out/'upgrade-audit.log')
        # Old source cache remains byte exact after independently upgrading apps.
        for p in bundle['packages']:
            dest=out/(p['name']+'-source-after')
            run(['debugfs','-R',f"dump /var/lib/pkg/sources/{p['sha256'][:8]}/{p['sha256'][8:]}.fpk {dest}",seed_out/'seeded-root.img.upgraded.img'],out/'upgrade-audit.log')
            assert digest(dest)==p['sha256']
        report.update(result='PASS',packages=bundle,templates=str(templates),seed_evidence=str(seed_out))
    except BaseException as error:report.update(result='FAIL',error=repr(error));raise
    finally:
        for directory in [out]+([templates] if templates else []):
            for p in directory.rglob('*.img'):
                if p.is_file() and not p.is_symlink():p.unlink()
        (out/'manifest.json').write_text(json.dumps(report,indent=2)+'\n');print(out,report['result'],flush=True)


if __name__=='__main__':main()
