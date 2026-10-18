"""Bounded log writer with independent FAT extraction and byte-range audit."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]


def sha(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--media',type=Path,required=True)
    parser.add_argument('--image-name',default='fortress-installer-multi.img');args=parser.parse_args()
    assert Path(args.image_name).name==args.image_name
    out=Path(tempfile.mkdtemp(prefix='logging-',dir=ROOT/'build/root-phase4'))
    manifest={'result':'FAIL','scope':'actual log slot; regular files only; no USB hardware claim','commands':[]}
    source=args.media/args.image_name;original=sha(source)
    def run(cmd):
        command=list(map(str,cmd));manifest['commands'].append(command)
        with (out/'host.log').open('ab') as log:subprocess.run(command,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=120)
    try:
        run(['gcc','-std=c11','-D_POSIX_C_SOURCE=200809L','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-no-pie',
             '-Isrc/include','-Isrc/fs','-Isrc/kernel','-Isrc/drivers','tests/install_log_host.c','src/kernel/install_log.c','user/tools/digest.c','-o',out/'host'])
        with tempfile.TemporaryDirectory(prefix='fortress-log-') as tmp:
            disk=Path(tmp)/'usb.img';shutil.copyfile(source,disk);run([out/'host',disk])
            start=(514*2048+161)*512;end=start+4096
            with source.open('rb') as old,disk.open('rb') as new:
                position=0
                while before:=old.read(1024**2):
                    after=new.read(len(before))
                    left=max(0,min(len(before),start-position));right=max(0,min(len(before),end-position))
                    assert before[:left]==after[:left] and before[right:]==after[right:],position
                    position+=len(before)
            volume=out/'log-volume.img'
            with disk.open('rb') as stream:stream.seek(514*1024**2);volume.write_bytes(stream.read(8*1024**2))
            run(['fsck.fat','-n',volume]);run(['mcopy','-i',volume,'::fortress-install.log',out/'fortress-install.log'])
            record=(out/'fortress-install.log').read_text();assert 'result=installation complete' in record and '\nuser=fabio\n' in record and '$5$' not in record
        assert sha(source)==original;manifest['source_sha256']=original;manifest['allowed_byte_range']=[start,end];manifest['result']='PASS';print(out)
    finally:(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')


if __name__=='__main__':main()
