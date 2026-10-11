#!/usr/bin/env python3
"""Actual installer input/cleanup helpers under ASan/UBSan, with adapters."""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
def main():
    parent=ROOT/'build/root-phase4';parent.mkdir(parents=True,exist_ok=True)
    out=Path(tempfile.mkdtemp(prefix='cli-',dir=parent))
    paths=['user/tools/install.c','user/tools/userdb.c','user/tools/digest.c','tests/install_cli_host.c']
    sources={p:hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in paths}
    compile=['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-no-pie',
             '-Isrc/include','-Isrc/fs','-Iuser/tools','tests/install_cli_host.c','user/tools/userdb.c','user/tools/digest.c','-o',str(out/'host')]
    manifest={'result':'FAIL','sources':sources,'scope':'actual text installer with host terminal/control adapters','commands':[compile,[str(out/'host')]]}
    try:
        with (out/'host.log').open('w') as log:
            for command in manifest['commands']:subprocess.run(command,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=120)
        assert all(hashlib.sha256((ROOT/p).read_bytes()).hexdigest()==value for p,value in sources.items())
        manifest['result']='PASS';print((out/'host.log').read_text().strip());print(out)
    finally:(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
if __name__=='__main__':main()
