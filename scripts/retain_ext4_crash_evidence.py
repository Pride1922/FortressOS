"""Archive explicit completed host evidence without deleting original outputs."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
from create_ext4_fixtures import ROOT


def main():
    parser=argparse.ArgumentParser();parser.add_argument('directories',nargs='+',type=Path);args=parser.parse_args()
    evidence=ROOT/'.codex-remote-attachments/ext4-phase9'
    out=Path(tempfile.mkdtemp(prefix='verification-',dir=evidence));records=[]
    for directory in args.directories:
        source=directory.resolve()
        assert source.is_relative_to(ROOT.resolve()) and (source/'manifest.json').is_file()
        assert not out.is_relative_to(source), 'archive destination overlaps source'
        for path in sorted(source.rglob('*')):
            if not path.is_file():continue
            relative=path.relative_to(ROOT);target=out/'artifacts'/relative
            target.parent.mkdir(parents=True,exist_ok=True)
            rawhash=hashlib.sha256()
            if path.suffix=='.img':
                target=Path(str(target)+'.gz')
                with path.open('rb') as inp,target.open('xb') as raw,gzip.GzipFile(filename='',mode='wb',fileobj=raw,mtime=0,compresslevel=1) as compressed:
                    while chunk:=inp.read(1024*1024):rawhash.update(chunk);compressed.write(chunk)
                reconstructed=hashlib.sha256()
                with gzip.open(target,'rb') as stream:
                    while chunk:=stream.read(1024*1024):reconstructed.update(chunk)
                assert reconstructed.digest()==rawhash.digest()
            else:
                shutil.copyfile(path,target);rawhash.update(path.read_bytes())
                assert hashlib.sha256(target.read_bytes()).digest()==rawhash.digest()
            records.append({'source':str(relative),'retained':str(target.relative_to(out)),
                'bytes':path.stat().st_size,'sha256':rawhash.hexdigest(),
                'retained_sha256':hashlib.sha256(target.read_bytes()).hexdigest()})
        print(f'Retained {source}',flush=True)
    snapshot=out/'sources';sources={}
    paths=[ROOT/'Makefile',ROOT/'PROTECTED.md',ROOT/'AGENTS.md',
           *sorted((ROOT/'src/fs').glob('*')),*sorted((ROOT/'tests').glob('ext4*')),
           *sorted((ROOT/'tests').glob('jbd2*')),*sorted((ROOT/'scripts').glob('*ext4*')),
           *sorted((ROOT/'scripts').glob('*jbd2*'))]
    for directory in ('tests/ext4_host','tests/host','tests/pipe_host','src/include'):
        paths.extend(sorted((ROOT/directory).rglob('*')))
    paths.extend([ROOT/'src/drivers/block.h',ROOT/'src/mm/heap.h'])
    for path in paths:
        if not path.is_file():continue
        relative=path.relative_to(ROOT);target=snapshot/relative;target.parent.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(path,target);sources[str(relative)]=hashlib.sha256(path.read_bytes()).hexdigest()
    binaries={}
    for path in sorted((evidence/'bin').glob('*')):
        if path.is_file():
            target=out/'bin'/path.name;target.parent.mkdir(exist_ok=True);shutil.copyfile(path,target)
            binaries[path.name]=hashlib.sha256(path.read_bytes()).hexdigest()
    versions=[]
    for argv in (['gcc','--version'],['python3','--version'],['e2fsck','-V'],['debugfs','-V'],['git','rev-parse','HEAD']):
        result=subprocess.run(argv,capture_output=True,check=True);versions.append({'argv':argv,'output':(result.stdout+result.stderr).decode()})
    (out/'manifest.json').write_text(json.dumps({'argv':__import__('sys').argv,'artifacts':records,
        'sources':sources,'binaries':binaries,'versions':versions},indent=2)+'\n')
    print(f'Verification evidence retained: {out}',flush=True)


if __name__=='__main__':main()
