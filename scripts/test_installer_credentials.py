#!/usr/bin/env python3
"""Chosen username and eight-character login/recovery, regular-file targets only."""
import argparse
import json
from pathlib import Path
import shutil
import tempfile
import test_installer as gate
from test_installer_bound import FIXTURE_DIGEST

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--media',type=Path,required=True);args=parser.parse_args()
    media=args.media.resolve();assert media.is_relative_to(gate.ROOT/'build/root-phase4')
    producer=json.loads((media/'manifest.json').read_text())
    assert producer['result']=='PASS' and producer['bound_inspector_digest']==FIXTURE_DIGEST
    out=Path(tempfile.mkdtemp(prefix='credentials-',dir=gate.ROOT/'build/root-phase4'))
    sources={str(p.relative_to(gate.ROOT)):gate.fixture.digest(p) for base in ('src','tests','scripts','user')
             for p in (gate.ROOT/base).rglob('*') if p.is_file() and p.suffix in ('.c','.h','.py','.asm')}
    images={str(p):gate.fixture.digest(p) for p in (gate.ROOT/'bin/fortress.img',gate.ROOT/'bin/fortress.iso',media/'fortress-installer-bound.img',media/'fortress-installer-bound.iso')}
    manifest={'result':'FAIL','scope':'BIOS/UEFI chosen username, eight-character login and recovery; regular-file fixtures only',
              'sources':sources,'image_hashes':images,'media':str(media),'cases':[]}
    try:
        for firmware in ('bios','uefi'):
            retained=out/firmware;retained.mkdir()
            with tempfile.TemporaryDirectory(prefix='fortress-credentials-') as tmp:
                native=Path(tmp)
                try:
                    disk=gate.install(native,media,firmware,384,manifest,source='usb',operator='eight888',recovery='recover8',
                        media_stem='fortress-installer-bound',serial='SPARE-INSPECT-TEST',disposable_gate=False,username='fabio',ui_capture=True)
                    gate.audit(disk,native,media/'iso/boot/fortress.elf','eight888','recover8',manifest,username='fabio')
                    gate.root_gate.guest(native,disk,firmware,1,0,'eight888','recover8',manifest,accel=gate.ACCEL,username='fabio')
                    rejected=native/'missing.img';shutil.copyfile(disk,rejected)
                    gate.root_gate.corrupt(rejected,'missing',native,lambda cmd:gate.fixture.run(cmd,native/'mutations.log'))
                    before=gate.fixture.digest(rejected)
                    gate.root_gate.guest(native,rejected,firmware,1,'missing','eight888','recover8',manifest,
                        recovery=True,wrong_secret='eight888',accel=gate.ACCEL,username='fabio')
                    assert gate.fixture.digest(rejected)==before
                finally:shutil.copytree(native,retained,dirs_exist_ok=True,ignore=shutil.ignore_patterns('*.sock'))
        assert all(gate.fixture.digest(gate.ROOT/p)==h for p,h in sources.items())
        assert all(gate.fixture.digest(Path(p))==h for p,h in images.items())
        manifest['result']='PASS';print(out,flush=True)
    except BaseException as error:
        manifest['error']=type(error).__name__+': '+str(error);raise
    finally:(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')

if __name__=='__main__':main()
