#!/usr/bin/env python3
"""Disposable actual EXT4 host-key publication and sector-atomic restart cuts."""
from pathlib import Path
import argparse
import ctypes as C
import json
import os
import re
import subprocess
import tempfile
import test_root_phase0 as fixture
ROOT=fixture.REPO

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--quick',action='store_true');args=parser.parse_args()
    build=ROOT/'build/ssh-hostkeys-host';build.mkdir(parents=True,exist_ok=True)
    binary=build/'storage-test'
    def run(cmd,**kw):
        return subprocess.run(list(map(str,cmd)),cwd=ROOT,check=True,capture_output=True,text=True,**kw).stdout
    run(['gcc','-std=c11','-O1','-g','-fsanitize=address,undefined','-Wall','-Wextra','-Werror','-no-pie','-pthread',
         '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-Itests/ext4_host','-Itests/host',
         '-Isrc/include','-Isrc/fs','-Isrc/drivers','-Isrc/mm','-Isrc/kernel','-Isrc/arch/x86_64',
         'tests/ssh_hostkey_storage_host.c','tests/ext4_fault_disk.c','-o',binary])
    cases=[]
    # Deterministic public fixture, not a production-generated host secret.
    lib=C.CDLL(str(build/'codec.so'));w=C.create_string_buffer(1952);pk=C.create_string_buffer(32);sk=C.create_string_buffer(64)
    encoded=C.create_string_buffer(1024);n=C.c_size_t()
    lib.crypto_ed25519_from_seed.argtypes=[C.c_void_p]*3
    lib.ssh_hostkey_encode.argtypes=[C.c_void_p,C.c_uint32,C.c_void_p,C.c_size_t,C.c_void_p,C.c_void_p]
    assert not lib.crypto_ed25519_from_seed(bytes(range(32)),pk,sk)
    assert not lib.ssh_hostkey_encode(sk,123,encoded,1024,C.byref(n),w)
    with tempfile.TemporaryDirectory(prefix='fortress-hostkey-storage-') as tmp:
        tmp=Path(tmp);stage=tmp/'tree';(stage/'etc').mkdir(parents=True);key=tmp/'key';key.write_bytes(encoded.raw[:n.value]);key.chmod(0o600)
        source=tmp/'source.img'
        with source.open('xb') as f:f.truncate(256*1024**2)
        run(['mke2fs','-q','-t','ext4','-b','4096','-I','256','-O',fixture.FEATURES,'-E','lazy_itable_init=0',
             '-m','0','-g','4096','-N','4096','-J','size=4','-d',stage,source])
        for name,mode in [('/', '040755'),('/etc','040755')]:
            for field,value in [('mode',mode),('uid',0),('gid',0)]:run(['debugfs','-w','-R',f'set_inode_field {name} {field} {value}',source])
        fixture.initialize_journal(source,tmp)
        for sector in (512,4096):
            target=tmp/'target.img';run(['cp','--sparse=always',source,target])
            result=run([binary,target,sector,key,'publish']);print(result.strip(),flush=True)
            counts=re.search(r'PUBLISH writes=(\d+) flushes=(\d+)',result);assert counts
            run([binary,target,sector,key,'recover']);run(['e2fsck','-fn',target])
            metadata=run(['debugfs','-R','stat /etc/ssh/ssh_host_ed25519_key',target])
            assert 'Mode:  0600' in metadata and 'User:     0' in metadata and 'Group:     0' in metadata
            exported=tmp/'exported';run(['debugfs','-R',f'dump /etc/ssh/ssh_host_ed25519_key {exported}',target])
            assert exported.read_bytes()==key.read_bytes()
            cases.append({'sector':sector,'normal':'PASS','writes':int(counts[1]),'flushes':int(counts[2])})
            for mode in ('short','zero','enospc','race'):
                run(['cp','--sparse=always',source,target]);run([binary,target,sector,key,mode]);run(['e2fsck','-fn',target])
                cases.append({'sector':sector,'io_adapter':mode,'result':'PASS'})
            # Actual mounted policy rejection against independently changed
            # inode metadata. No malformed canonical key is ever overwritten.
            good=tmp/'good.img';run(['cp','--sparse=always',source,good]);run([binary,good,sector,key,'publish'])
            for path,field,value in [('/etc','mode','040777'),('/etc/ssh','uid','1000'),
                                     ('/etc/ssh/ssh_host_ed25519_key','mode','0100644'),
                                     ('/etc/ssh/ssh_host_ed25519_key','uid','1000')]:
                run(['cp','--sparse=always',good,target])
                run(['debugfs','-w','-R',f'set_inode_field {path} {field} {value}',target])
                run([binary,target,sector,key,'reject']);run(['e2fsck','-fn',target])
                cases.append({'sector':sector,'unsafe':path,'field':field,'result':'PASS'})
            for kind,count in [('flush',int(counts[2])),('write',int(counts[1]))]:
                indices=range(count) if not args.quick else sorted({0,count//2,count-1})
                for index in indices:
                    run(['cp','--sparse=always',source,target])
                    result=run([binary,target,sector,key,'publish',kind,index]);assert 'CUT' in result
                    result=run([binary,target,sector,key,'recover']);run(['e2fsck','-fn',target])
                    cases.append({'sector':sector,'kind':kind,'cut':index,'recovery':'valid' if 'RECOVER valid' in result else 'absent'})
                print(f'PASS sector={sector} {kind} cuts={len(indices)}',flush=True)
    (build/'storage-result.json').write_text(json.dumps({'status':'PASS','cases':cases},indent=2)+'\n')
    print(f'PASS {len(cases)} actual-code storage cases')
if __name__=='__main__':main()
