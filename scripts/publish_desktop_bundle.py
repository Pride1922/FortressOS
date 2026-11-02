"""Publish immutable LOCAL unsigned desktop artifacts for transfer to the signer.

This does not sign a catalog or deploy to the HTTP repository.
"""
import hashlib
import json
from pathlib import Path
from create_installer_media import ROOT,digest


def publish_bundle(bundle):
    identity=hashlib.sha256(json.dumps([(p['name'],p['build'],p['sha256']) for p in bundle['packages']]).encode()).hexdigest()[:16]
    directory=ROOT/'bin'/('fortress-desktop-packages-'+identity)
    if directory.is_symlink():raise ValueError('symlink bundle refused')
    directory.mkdir(exist_ok=True)
    published=dict(bundle);published['packages']=[]
    for record in bundle['packages']:
        source=Path(record['path']);assert digest(source)==record['sha256']
        target=directory/(record['name']+'.fpk')
        if target.is_symlink():raise ValueError('symlink artifact refused')
        if target.exists():assert digest(target)==record['sha256'],'immutable local bundle conflict'
        else:
            with target.open('xb') as stream:stream.write(source.read_bytes())
        published['packages'].append(dict(record,path=str(target)))
    manifest=directory/'bundle.json'
    text=json.dumps(published,indent=2)+'\n'
    if manifest.exists():assert not manifest.is_symlink() and manifest.read_text()==text
    else:manifest.write_text(text)
    return published,directory
