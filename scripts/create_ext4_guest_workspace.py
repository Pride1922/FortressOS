"""Snapshot current tracked sources for an isolated guest build, without Git edits."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
from create_ext4_fixtures import ROOT


def main():
    parent=ROOT/'.codex-remote-attachments/ext4-phase9'
    out=Path(tempfile.mkdtemp(prefix='guest-workspace-',dir=parent));records={}
    names=subprocess.check_output(['git','ls-files','-z'],cwd=ROOT).decode().split('\0')
    for name in names:
        if not name:continue
        source=ROOT/name
        if not source.is_file():continue
        data=source.read_bytes();target=out/name;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(data)
        assert source.read_bytes()==data, f'source changed during snapshot: {name}'
        records[name]=hashlib.sha256(data).hexdigest()
    # Build dependencies are copied exclusively, never cleaned or rebuilt in place.
    if (ROOT/'limine').is_dir():shutil.copytree(ROOT/'limine',out/'limine',ignore=shutil.ignore_patterns('.git'))
    (out/'workspace-manifest.json').write_text(json.dumps({'scope':'9.4 isolated build preparation',
        'git_head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT).decode().strip(),
        'sources':records},indent=2)+'\n')
    print(out,flush=True)


if __name__=='__main__':main()
