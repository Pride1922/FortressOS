#!/usr/bin/env python3
"""Package status exclusion regression on a fresh regular-file ext4 filesystem."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from create_ext4_fixtures import ROOT, run
from initialize_ext4_journal import initialize


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--expect-old-failure',action='store_true');args=parser.parse_args()
    parent=ROOT/'build/pkg-status';parent.mkdir(parents=True,exist_ok=True)
    out=Path(tempfile.mkdtemp(prefix='host-',dir=parent));out.chmod(0o755)
    print('Evidence:',out,flush=True)
    manifest={'result':'FAIL','scope':'actual package status/VFS/EXT4/JBD2 with host mutex and disk adapters',
              'source_sha256':hashlib.sha256((ROOT/'src/fs/pkg_executor.inc').read_bytes()).hexdigest()}
    try:
        image=out/'root.img'
        with image.open('xb') as f:f.truncate(32*1024*1024)
        (out/'mkfs.log').write_text(run(['mke2fs','-q','-t','ext4','-b','4096','-I','256',
            '-O','none,extent,filetype,sparse_super,large_file,metadata_csum,has_journal',
            '-E','lazy_itable_init=0','-m','0','-g','4096','-N','1024','-J','size=4',str(image)]))
        initialize(image)
        flags=['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
            '-fno-omit-frame-pointer','-no-pie','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
            '-pthread','-DHOST_TEST','-DTOOL_HOST_TEST','-DFORTRESS_DAC_ENFORCED',
            '-I.','-Itests','-Itests/ext4_host','-Itests/host','-Isrc/include','-Isrc/fs','-Isrc/drivers',
            '-Isrc/mm','-Isrc/lib','-Isrc/kernel','-Isrc/arch/x86_64','-Isrc/net','-Iuser/tools','-Isrc/lib/crypto/upstream/src']
        binary=out/'status'
        objects=[]
        for i,source in enumerate(('src/lib/crypto/upstream/src/monocypher.c',
                                  'src/lib/crypto/upstream/src/optional/monocypher-ed25519.c')):
            obj=out/f'mono{i}.o';objects.append(obj)
            subprocess.run(flags+['-include','src/lib/crypto/namespace.h','-c',source,'-o',str(obj)],cwd=ROOT,check=True)
        units=['tests/pkg_status_host.c','tests/ext4_fault_disk.c','src/fs/permission_values.c',
            'src/kernel/creds.c','src/lib/account_redo.c','src/lib/crypto/hash.c','src/lib/crypto/curve.c',
            'user/tools/digest.c','user/tools/gzip.c','user/pkg/catalog.c','user/pkg/format.c',
            'user/pkg/manifest.c','user/pkg/verify.c','user/pkg/repository.c','src/fs/pkg_plan.c']
        with (out/'compile.log').open('w') as log:
            subprocess.run(flags+units+list(map(str,objects))+['-o',str(binary)],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=300)
        result=subprocess.run([str(binary),str(image),str(out/'checked.img')],cwd=ROOT,capture_output=True,text=True,timeout=240)
        (out/'runtime.log').write_text(result.stdout+result.stderr)
        if args.expect_old_failure:
            assert result.returncode==2 and 'PKG_STATUS_UNPROTECTED_READ' in result.stderr,result.stdout+result.stderr
            manifest['result']='EXPECTED_FAILURE';print('Reproduced: PKG_STATUS_UNPROTECTED_READ',flush=True)
        else:
            assert result.returncode==0,result.stdout+result.stderr
            (out/'fsck.log').write_text(run(['e2fsck','-fn',str(out/'checked.img')]))
            manifest['result']='PASS';print(result.stdout,end='',flush=True)
    finally:
        (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')


if __name__=='__main__':main()
