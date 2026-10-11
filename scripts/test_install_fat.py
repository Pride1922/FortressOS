#!/usr/bin/env python3
"""Native FAT editor on disposable regular-file ESP, independent mtools/fsck audit."""
import hashlib
import json
from pathlib import Path
import subprocess
import struct
import tempfile

ROOT=Path(__file__).resolve().parents[1]


def main():
    parent=ROOT/'build/root-phase4'
    parent.mkdir(parents=True,exist_ok=True)
    out=Path(tempfile.mkdtemp(prefix='fat-',dir=parent))
    sources=['src/include/install_fat.h','src/lib/install_fat.c','tests/install_fat_host.c','scripts/test_install_fat.py','Makefile']
    manifest={'scope':'native FAT32 editor; host-created regular-file ESP; no boot acceptance','result':'FAIL',
              'sources':{p:hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in sources},'commands':[]}
    try:
        with (out/'audit.log').open('w') as log:
            def run(cmd):
                manifest['commands'].append([str(x) for x in cmd])
                subprocess.run(cmd,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=120)
            esp=out/'template.img'
            with esp.open('wb') as f:f.truncate(64*1024*1024)
            run(['mformat','-i',esp,'-F','-v','FORTRESS_ESP','::'])
            for path in ('boot','boot/limine','efi','efi/boot'):run(['mmd','-i',esp,'::/'+path])
            for name,size,paths in [('bootstrap',8*1024*1024,['boot/bootstrap.tar']),('readiness',512,['boot/readiness.v1']),
                                    ('config',4096,['limine.conf','boot/limine.conf','boot/limine/limine.conf','efi/boot/limine.conf'])]:
                payload=out/name
                with payload.open('wb') as f:f.truncate(size)
                for path in paths:run(['mcopy','-i',esp,payload,'::/'+path])
            # mformat leaves backup FSInfo zero; normalize the controlled template
            # producer before native validation, and retain the normalized bytes.
            data=bytearray(esp.read_bytes());fi=struct.unpack_from('<H',data,48)[0]*512
            backup=struct.unpack_from('<H',data,50)[0]*512
            data[backup+fi:backup+fi+512]=data[fi:fi+512];esp.write_bytes(data)
            binary=out/'host'
            run(['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-no-pie',
                 '-Isrc/include','src/lib/install_fat.c','tests/install_fat_host.c','user/tools/digest.c','-o',binary])
            edited=out/'edited.img';run([binary,esp,edited]);run(['fsck.fat','-n',edited])
            expected={'boot/bootstrap.tar':b'native-bootstrap-bytes\n','boot/readiness.v1':bytes(512)}
            for path in ['limine.conf','boot/limine.conf','boot/limine/limine.conf','efi/boot/limine.conf']:
                expected[path]=b'timeout: 0\n/FortressOS\n'
            for i,(path,data) in enumerate(expected.items()):
                extracted=out/('file-'+str(i));run(['mcopy','-i',edited,'::/'+path,extracted]);assert extracted.read_bytes()==data
        assert all(hashlib.sha256((ROOT/p).read_bytes()).hexdigest()==h for p,h in manifest['sources'].items())
        manifest['result']='PASS';print('PASS native FAT editor and independent fsck/mtools byte audit');print(out)
    finally:(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')


if __name__=='__main__':main()
