#!/usr/bin/env python3
"""Actual-code base update host suite; disposable files, no QEMU/devices/network."""
import binascii
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from build_base_package import bootstrap,encode,BANK,validate
from pkg_repository import catalog,public
from create_installer_media import ROOT


def main():
    out=Path(tempfile.mkdtemp(prefix='base-update-host-',dir=ROOT/'build'))
    report={'result':'INCOMPLETE','physical':'NOT RUN','qemu':'NOT RUN','commands':[]}
    def run(args,timeout=180):
        args=list(map(str,args));report['commands'].append(args)
        r=subprocess.run(args,cwd=ROOT,capture_output=True,text=True,timeout=timeout)
        (out/f'command-{len(report["commands"])}.log').write_text(r.stdout+r.stderr)
        if r.returncode:raise RuntimeError(r.stdout[-2000:]+r.stderr[-3000:])
        return r.stdout
    templates=None;runtime=None
    try:
        kernel=out/'kernel.elf';run(['objcopy','--strip-debug',ROOT/'bin/fortress.elf',kernel])
        elf=kernel.read_bytes();kernel.write_bytes(elf.ljust(BANK,b'\0'))
        platform=bytes.fromhex('42'*32);factory=out/'factory.tar'
        source=(ROOT/'bin/initramfs.tar').read_bytes()
        factory.write_bytes(bootstrap(source,1,1,platform))
        objects=[]
        for build in (2,3):
            obj=encode(elf,bootstrap(source,build,1,platform),build,1,build-1,1,platform);validate(obj)
            path=out/f'base-{build}.fpk';path.write_bytes(obj);objects.append(path)
        key=Ed25519PrivateKey.generate() # ephemeral TEST signing only; never provisioned
        wire,_=catalog(bytes(range(16)),1,5,1793188799,1793189800,[objects[0].read_bytes()],[key])
        (out/'catalog.v1').write_bytes(wire);(out/'public.key').write_bytes(public(key))
        templates=Path(run(['python3','scripts/create_installer_payload.py','--base-system','--kernel',kernel,'--bootstrap',factory],600).strip().splitlines()[-1])
        disk=templates/'nvme-384.img';assert disk.is_file()
        # Installed roots extend to the final GPT usable sector. Reproduce only
        # that geometry here; root content stays the independently formatted seed.
        with disk.open('r+b') as f:
            sectors=disk.stat().st_size//512;f.seek(1024);entries=bytearray(f.read(16384))
            struct.pack_into('<Q',entries,128+40,sectors-34);checksum=binascii.crc32(entries)
            for header_lba,array_lba in ((1,2),(sectors-1,sectors-33)):
                f.seek(header_lba*512);h=bytearray(f.read(512));struct.pack_into('<I',h,88,checksum);struct.pack_into('<I',h,16,0)
                struct.pack_into('<I',h,16,binascii.crc32(h[:92]));f.seek(header_lba*512);f.write(h);f.seek(array_lba*512);f.write(entries)
        before=hashlib.sha256(disk.read_bytes()).hexdigest()
        flags=['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer',
               '-no-pie','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-DTOOL_HOST_TEST',
               '-Isrc/include','-Isrc/lib','-Isrc/fs','-Isrc/kernel','-Isrc/drivers','-Isrc/mm','-Iuser/tools','-Isrc/lib/crypto/upstream/src']
        crypto=[]
        for i,source_file in enumerate(('src/lib/crypto/upstream/src/monocypher.c','src/lib/crypto/upstream/src/optional/monocypher-ed25519.c')):
            obj=out/f'crypto-{i}.o';run(flags+['-include','src/lib/crypto/namespace.h','-c',source_file,'-o',obj]);crypto.append(obj)
        units=['tests/base_update_host.c','src/lib/kernel_update.c','src/lib/install_fat.c','src/lib/install_archive.c',
               'src/kernel/root_boot.c','src/fs/gpt.c','src/lib/crc32.c','user/tools/digest.c','user/tools/userdb.c',
               'user/pkg/base.c','user/pkg/catalog.c','user/pkg/format.c','src/lib/crypto/hash.c','src/lib/crypto/curve.c']
        binary=out/'host';run(flags+units+crypto+['-o',binary])
        output=run([binary,disk,factory,objects[0],out/'catalog.v1',out/'public.key',objects[1],out/'updated-esp.img'],180)
        run(['fsck.fat','-n',out/'updated-esp.img'])
        assert hashlib.sha256(disk.read_bytes()).hexdigest()==before
        shutil.copyfile(kernel,out/'fortress.elf');shutil.copyfile(factory,out/'initramfs.tar')
        runtime=Path(run(['python3','scripts/test_install_runtime.py','--templates',templates,'--artifacts',out],600).strip().splitlines()[-1])
        report['runtime_installation']='PASS: actual installer engine and independent FAT/ext4 audits'
        report['runtime_installation_evidence']=str(runtime)
        report.update(result='PASS',actual_code=output,source_disk_unchanged=True)
    except BaseException as error:
        report.update(result='FAIL',error=repr(error));raise
    finally:
        # Evidence and small signed test objects remain; no disposable disk/ISO
        # copies accumulate on the laptop after the suite.
        for directory in [out]+([templates,templates/'tree'] if templates else [])+([runtime] if runtime else []):
            for p in directory.rglob('*'):
                if p.is_file() and p.suffix in ('.img','.iso'):p.unlink()
        (out/'manifest.json').write_text(json.dumps(report,indent=2)+'\n');print(out,report['result'],flush=True)


if __name__=='__main__':main()
