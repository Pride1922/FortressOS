"""Prepare an isolated physical-fixture build. Writes regular artifacts only."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import uuid
from create_ext4_fixtures import ROOT

CAPACITY=4026531840

def prepare():
    parent=ROOT/'.codex-remote-attachments/ext4-phase9'
    out=Path(tempfile.mkdtemp(prefix='physical-workspace-',dir=parent));identity=str(uuid.uuid4())
    names=subprocess.check_output(['git','ls-files','-z'],cwd=ROOT).decode().split('\0')
    names+=['src/include/ext4_physical_fixture.h']
    sources={};overrides={}
    for name in dict.fromkeys(names):
        if not name or not (ROOT/name).is_file():continue
        data=(ROOT/name).read_bytes();original=hashlib.sha256(data).hexdigest()
        if name=='src/include/ext4_physical_fixture.h':
            data=(f'#define FORTRESS_EXT4_PHYSICAL_PARTUUID "{identity}"\n'
                  f'#define FORTRESS_EXT4_PHYSICAL_CAPACITY {CAPACITY}ULL\n').encode()
            overrides[name]={'original_sha256':original,'reason':'Authorized isolated physical test only'}
        if name=='limine.conf':
            data=(f'timeout: 10\n\n'
                  f'/EXT4 9.6 DISPOSABLE TEST - START\n    protocol: limine\n    kernel_path: boot():/boot/fortress.elf\n'
                  f'    module_path: boot():/boot/initramfs.tar\n    kernel_cmdline: ext4_physical=start usb_data=PARTUUID={identity} usb_data_mode=rw\n\n'
                  f'/EXT4 9.6 DISPOSABLE TEST - VERIFY\n    protocol: limine\n    kernel_path: boot():/boot/fortress.elf\n'
                  f'    module_path: boot():/boot/initramfs.tar\n    kernel_cmdline: ext4_physical=verify usb_data=PARTUUID={identity} usb_data_mode=rw\n').encode()
            overrides[name]={'original_sha256':original,'reason':'Labelled target-specific physical boot entries'}
        target=out/name;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(data);sources[name]=hashlib.sha256(data).hexdigest()
    shutil.copytree(ROOT/'limine',out/'limine',ignore=shutil.ignore_patterns('.git'))
    manifest={'scope':'9.6 authorized physical artifact preparation; no device I/O','git_head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT).decode().strip(),
              'sources':sources,'source_overrides':overrides,'data_partuuid':identity,
              'target':{'dell':'Latitude 5590','friendly_name':'Generic Flash Disk','serial':'C','capacity_bytes':CAPACITY,'reported_windows_disk':1,'erase_authorized':True,'authorization_date':'2026-10-07'}}
    (out/'workspace-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(out,flush=True)
    return out

if __name__=='__main__':prepare()
