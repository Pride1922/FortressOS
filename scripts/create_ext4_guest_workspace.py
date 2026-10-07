"""Snapshot current tracked sources for an isolated guest build, without Git edits."""
import hashlib
import argparse
import json
from pathlib import Path
import shutil
import re
import subprocess
import tempfile
from create_ext4_fixtures import ROOT


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--usb-journal',action='store_true');args=parser.parse_args()
    parent=ROOT/'.codex-remote-attachments/ext4-phase9'
    out=Path(tempfile.mkdtemp(prefix='guest-workspace-',dir=parent));records={};overrides={}
    names=subprocess.check_output(['git','ls-files','-z'],cwd=ROOT).decode().split('\0')
    if (ROOT/'src/include/ext4_physical_fixture.h').is_file() and 'src/include/ext4_physical_fixture.h' not in names:
        names.append('src/include/ext4_physical_fixture.h')
    for name in names:
        if not name:continue
        source=ROOT/name
        if not source.is_file():continue
        data=source.read_bytes()
        assert source.read_bytes()==data, f'source changed during snapshot: {name}'
        if args.usb_journal and name=='limine.conf':
            overrides[name]={'original_sha256':hashlib.sha256(data).hexdigest(),
                'append_cmdline':'usb_data=PARTUUID=11223344-5566-7788-99aa-bbccddeeff00 usb_data_mode=rw'}
            text,count=re.subn(r'(kernel_cmdline:[^\r\n]*)',r'\1 '+overrides[name]['append_cmdline'],data.decode(),count=1)
            assert count==1;data=text.encode()
        target=out/name;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(data)
        records[name]=hashlib.sha256(data).hexdigest()
    # Build dependencies are copied exclusively, never cleaned or rebuilt in place.
    if (ROOT/'limine').is_dir():shutil.copytree(ROOT/'limine',out/'limine',ignore=shutil.ignore_patterns('.git'))
    (out/'workspace-manifest.json').write_text(json.dumps({'scope':'9.4 isolated build preparation',
        'git_head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT).decode().strip(),
        'sources':records,'source_overrides':overrides},indent=2)+'\n')
    print(out,flush=True)


if __name__=='__main__':main()
