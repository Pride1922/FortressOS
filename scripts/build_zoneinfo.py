#!/usr/bin/env python3
"""Compile the vendored public-domain IANA tzdb; no network or host zone data.

zic is a build dependency (Ubuntu libc-bin). Intermediate aliases are regular
files; the release package uses one bounded database to preserve installer limits.
"""
import hashlib
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import sys
import struct

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / 'assets/tzdata2026c.tar.gz'
SHA256 = 'e4a178a4477f3d0ea77cc31828ff72aa38feff8d61aa13e7e99e142e9d902be4'

def build(destination):
    assert hashlib.sha256(SOURCE.read_bytes()).hexdigest() == SHA256, 'tzdata integrity mismatch'
    destination = Path(destination).resolve()
    destination.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='fortress-tzdata-') as temp:
        source = Path(temp)
        names = ['africa', 'antarctica', 'asia', 'australasia', 'europe',
                 'northamerica', 'southamerica', 'etcetera', 'backward']
        with tarfile.open(SOURCE, 'r:gz') as archive:
            for name in names + ['LICENSE', 'version']:
                (source / name).write_bytes(archive.extractfile(name).read())
        subprocess.run(['zic', '-b', 'fat', '-d', str(source / 'compiled'),
                        *[str(source / name) for name in names]], check=True)
        for path in (source / 'compiled').rglob('*'):
            if path.is_file():
                target = destination / path.relative_to(source / 'compiled')
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(path, target)
        shutil.copyfile(source / 'LICENSE', destination / 'LICENSE')
        shutil.copyfile(source / 'version', destination / 'version')

def pack(source,destination):
    """One regular file keeps the installer's existing 510-entry bound intact."""
    source=Path(source);entries=[]
    for path in sorted(source.rglob('*')):
        if not path.is_file() or path.name in ('LICENSE','version'):continue
        name=path.relative_to(source).as_posix().encode('ascii');data=path.read_bytes()
        assert 0<len(name)<128 and 0<len(data)<16384
        entries.append((name,data))
    assert len(entries)==597, 'unexpected zone inventory; clean the staging directory before a tzdb update'
    wire=bytearray(b'FOSTZ001'+struct.pack('>I',len(entries)))
    for name,data in entries:
        wire+=struct.pack('>BI',len(name),len(data))+name+data
    assert len(wire)<1024*1024
    Path(destination).parent.mkdir(parents=True,exist_ok=True)
    Path(destination).write_bytes(wire)

if __name__ == '__main__':
    if len(sys.argv)==4 and sys.argv[1]=='--pack':pack(sys.argv[2],sys.argv[3])
    else:build(sys.argv[1])
