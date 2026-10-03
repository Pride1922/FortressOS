#!/usr/bin/env python3
"""Prepare deterministic LAN payloads and identify the generated EXT4 test image.
Writes only build artifacts; never flashes, repairs or converts a device.
"""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import uuid

ROOT=Path(__file__).resolve().parent.parent

def main():
    out=ROOT/'build/ext4-dell';out.mkdir(parents=True,exist_ok=True)
    pattern=bytes((i*17+3)&255 for i in range(256));checksums=[]
    for name,size in [('data-1m.bin',1048576),('data-16m.bin',16777216)]:
        payload=pattern*(size//256);(out/name).write_bytes(payload)
        checksums.append(hashlib.sha256(payload).hexdigest()+'  '+name)
    (out/'SHA256SUMS').write_text('\n'.join(checksums)+'\n')
    image=ROOT/'bin/fortress-ext4-test.img'
    with image.open('rb') as f:
        f.seek(1024+128+16);guid=str(uuid.UUID(bytes_le=f.read(16)))
        f.seek(133120*512);data=f.read(64*1024*1024)
    if len(data)!=64*1024*1024:raise RuntimeError('Incomplete generated image')
    with tempfile.NamedTemporaryFile() as partition:
        partition.write(data);partition.flush()
        result=subprocess.run(['dumpe2fs','-h',partition.name],capture_output=True,text=True,check=True)
    (out/'image-dumpe2fs.txt').write_text(result.stdout+result.stderr)
    (out/'image.json').write_text(json.dumps({'image':str(image),
        'image_sha256':hashlib.sha256(image.read_bytes()).hexdigest(),
        'data_partuuid':guid,'data_partition_bytes':len(data)},indent=2)+'\n')
    print(f'Dell payloads and image identity: {out}')

if __name__=='__main__':main()
