#!/usr/bin/env python3
"""Number confirmation and cancellation; disposable regular-file NVMe only."""
import argparse
import json
from pathlib import Path
import shutil
import tempfile
import test_installer as gate

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--media',type=Path,required=True)
    parser.add_argument('--password-guidance',action='store_true');parser.add_argument('--disk-timeout',action='store_true')
    args=parser.parse_args();media=args.media.resolve()
    assert media.is_relative_to(gate.ROOT/'build/root-phase4')
    producer=json.loads((media/'manifest.json').read_text())
    assert producer['result']=='PASS' and producer['profile'].startswith('explicit user-selected')
    out=Path(tempfile.mkdtemp(prefix='confirmation-',dir=gate.ROOT/'build/root-phase4'))
    manifest={'result':'FAIL','media':str(media),'scope':'actual NVMe timeout failure; regular files only' if args.disk_timeout else 'number confirmation retries/cancel; regular files only','cases':[]}
    try:
        with tempfile.TemporaryDirectory(prefix='fortress-confirmation-') as tmp:
            native=Path(tmp)
            try:
                for firmware in ('bios','uefi'):
                    for selected in ((1,) if args.disk_timeout else (1,2)):
                        work=native/f'{firmware}-{selected}';work.mkdir()
                        gate.install(work,media,firmware,384,manifest,source='usb',disposable_gate=False,
                            media_stem='fortress-installer-multi',multi_selection=0 if args.disk_timeout else selected,confirmation_cancel=not args.disk_timeout,
                            reject='timeout' if args.disk_timeout else None,
                            ui_capture=True,username='fabio',
                            operator='cobalt meadow' if args.password_guidance and selected==2 else 'eight888',
                            recovery='cobalt meadow lantern' if args.password_guidance else 'recover8',
                            password_guidance=args.password_guidance)
                        print('PASS '+('disk timeout failure' if args.disk_timeout else 'confirmation retries/cancel'),firmware,selected,flush=True)
            finally:shutil.copytree(native,out,dirs_exist_ok=True,ignore=shutil.ignore_patterns('*.sock'))
        manifest['result']='PASS';print(out,flush=True)
    except BaseException as error:
        manifest['error']=type(error).__name__+': '+str(error);raise
    finally:(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')

if __name__=='__main__':main()
