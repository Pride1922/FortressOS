"""Retain and verify the exact source snapshot behind an isolated guest ELF/ISO."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
from test_ext4_guest_crash import REPO


def main():
    parser=argparse.ArgumentParser();parser.add_argument('workspace',type=Path);parser.add_argument('archive',type=Path);parser.add_argument('--review',default='phase9-4-review.json');args=parser.parse_args()
    root=(REPO/'.codex-remote-attachments/ext4-phase9').resolve()
    workspace=args.workspace.resolve();archive=args.archive.resolve()
    assert workspace.is_relative_to(root) and archive.is_relative_to(root)
    assert (archive/'manifest.json').is_file(),'wait for completed archive'
    manifest=json.loads((workspace/'workspace-manifest.json').read_text())
    target=archive/('guest-build-sources' if args.review=='phase9-4-review.json' else workspace.name+'-sources');target.mkdir()
    for name,digest in manifest['sources'].items():
        source=(workspace/name).resolve();destination=(target/name).resolve()
        assert source.is_relative_to(workspace) and destination.is_relative_to(target)
        data=source.read_bytes();assert hashlib.sha256(data).hexdigest()==digest,name
        destination.parent.mkdir(parents=True,exist_ok=True);destination.write_bytes(data)
        assert hashlib.sha256(destination.read_bytes()).hexdigest()==digest
    shutil.copyfile(workspace/'workspace-manifest.json',target/'workspace-manifest.json')
    assert Path(args.review).name==args.review
    shutil.copyfile(root/args.review,archive/args.review)
    review=json.loads((archive/args.review).read_text());assert review['errors']==[]
    for name,built in (('fixture.iso','fortress.iso'),('fortress.elf','fortress.elf')):
        digest=hashlib.sha256((workspace/'bin'/built).read_bytes()).hexdigest()
        if 'builds' in review:assert review['builds'][str(workspace)][name]==digest
        else:assert all(item['binaries'][name]==digest for item in review['campaigns'])
    for name in ('scripts/test_nmi_transitions.py','scripts/test_net_pci.py'):
        source=REPO/name;destination=archive/'sources'/name
        shutil.copyfile(source,destination)
    versions=[]
    for argv in (['qemu-system-x86_64','--version'],['readelf','--version'],['nm','--version']):
        result=subprocess.run(argv,capture_output=True,text=True,check=True)
        versions.append({'argv':argv,'output':result.stdout+result.stderr})
    (archive/'guest-tool-versions.json').write_text(json.dumps(versions,indent=2)+'\n')
    (archive/('guest-source-review.json' if args.review=='phase9-4-review.json' else workspace.name+'-source-review.json')).write_text(json.dumps({'sources':len(manifest['sources']),
        'workspace':str(workspace),'manifest_sha256':hashlib.sha256((target/'workspace-manifest.json').read_bytes()).hexdigest(),
        'review_sha256':hashlib.sha256((archive/args.review).read_bytes()).hexdigest(),'errors':[]},indent=2)+'\n')
    print(f'PASS retained guest build sources: {target}',flush=True)


if __name__=='__main__':main()
