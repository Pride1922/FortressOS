#!/usr/bin/env python3
"""Actual mounted timestamp writes, disposable regular files and Linux oracle."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import struct
import subprocess
import tempfile
from create_ext4_fixtures import ROOT,run
from test_jbd2_replay_host import FEATURES,blocks,be,put,seal,sbseal

def fixture(directory,bs):
    image=directory/f'base-{bs}.img'
    with image.open('xb') as stream:stream.truncate(32*1024*1024)
    run(['mke2fs','-q','-t','ext4','-b',str(bs),'-I','256','-O',FEATURES,
         '-E','lazy_itable_init=0','-m','0','-g','4096','-N','512','-J','size=4',str(image)])
    seed=directory/f'seed-{bs}.bin';seed.write_bytes(b'O'*(3*bs))
    commands=directory/f'journal-{bs}.commands'
    commands.write_text(f'write {seed} /target.bin\njournal_open -c -v 3\njournal_close\n')
    run(['debugfs','-w','-f',str(commands),str(image)])
    data=bytearray(image.read_bytes());ino=struct.unpack_from('<I',data,1248)[0]
    mapping=blocks(image,f'<{ino}>');start=mapping[0]*bs
    journal=bytearray(data[start:start+bs]);assert be(journal,40) in (0x10,0x11)
    put(journal,24,1);put(journal,28,0);put(journal,88,1);put(journal,40,0x11)
    head=bytearray(journal[:1024]);seal(head,252,0xffffffff);journal[:1024]=head
    data[start:start+bs]=journal
    struct.pack_into('<I',data,1120,0x42);struct.pack_into('<H',data,1082,1);sbseal(data)
    image.write_bytes(data);return image

def metadata(image,path,epoch,written):
    text=run(['debugfs','-R',f'stat {path}',str(image)])
    for label,expected in [('atime',epoch),('crtime',epoch),('ctime',epoch+written),('mtime',epoch+written)]:
        match=re.search(rf'^\s*{label}:\s+0x([0-9a-f]+):([0-9a-f]+)',text,re.M|re.I)
        assert match,(image,path,label,text)
        lo,extra=(int(part,16) for part in match.groups())
        signed=lo if lo<2**31 else lo-2**32
        assert signed+(extra&3)*2**32==expected,(image,path,label,text)
        assert extra>>2==0,(image,path,label,'unexpected nanoseconds',text)
    assert 'User:     0' in text and 'Group:     0' in text,text
    mode=re.search(r'Mode:\s+([0-7]+)',text)
    assert mode and int(mode[1],8)==(0o755 if path=='/directory' else 0o644),text
    return text

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--expect-old-failure',action='store_true')
    args=parser.parse_args();records=[]
    evidence=ROOT/'build/ext4-time';evidence.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='fortress-ext4-time-') as temp:
        directory=Path(temp)
        for bs in ((4096,) if args.expect_old_failure else (1024,2048,4096)):
            source=fixture(directory,bs);digest=hashlib.sha256(source.read_bytes()).hexdigest()
            for sector in ((512,) if args.expect_old_failure else (512,4096)):
                prefix=directory/f'result-{bs}-{sector}'
                command=[str(ROOT/'build/ext4_time_host'),str(source),str(sector),str(prefix)]
                result=subprocess.run(command,capture_output=True,text=True,timeout=120)
                text=result.stdout+result.stderr
                (evidence/f'{bs}-{sector}.log').write_text(text)
                if args.expect_old_failure:
                    assert result.returncode==2 and 'CREATION_TIMESTAMP_REGRESSION' in text,text
                    (evidence/'baseline-failure.json').write_text(json.dumps({'result':'EXPECTED_FAILURE',
                        'source_sha256':hashlib.sha256((ROOT/'src/fs/ext4_mutate.inc').read_bytes()).hexdigest(),
                        'diagnostic':text},indent=2)+'\n')
                    print('PASS regression reproduces old creation timestamp defect');return
                assert result.returncode==0,text
                audit=''
                for i,epoch in enumerate((1792368000,2147483647,2147483648,4102444800)):
                    image=Path(str(prefix)+f'-epoch-{i}.img')
                    audit+=run(['e2fsck','-fn',str(image)])
                    audit+=metadata(image,'/stamp',epoch,1)
                    audit+=metadata(image,'/directory',epoch,0)
                    output=directory/'bytes.bin'
                    run(['debugfs','-R',f'dump /stamp {output}',str(image)])
                    assert output.read_bytes()==b'bytes';output.unlink()
                for size in (4,8,12,16,20):
                    image=Path(str(prefix)+f'-short-{size}.img')
                    audit+=run(['e2fsck','-fn',str(image)])
                    output=directory/'bytes.bin'
                    run(['debugfs','-R',f'dump /short {output}',str(image)])
                    assert output.read_bytes()==b'AFTER!+';output.unlink()
                (evidence/f'{bs}-{sector}-linux.log').write_text(audit)
                assert hashlib.sha256(source.read_bytes()).hexdigest()==digest
                records.append({'block':bs,'sector':sector,'epochs':4,'short_extra_sizes':[4,8,12,16,20],'source_unchanged':True})
                print(f'PASS mounted timestamps + Linux fsck/debugfs/bytes block={bs} sector={sector}',flush=True)
    (evidence/'manifest.json').write_text(json.dumps({'result':'PASS','profiles':records,
        'source_sha256':{name:hashlib.sha256((ROOT/name).read_bytes()).hexdigest() for name in (
            'src/fs/ext4_time.h','src/fs/ext4_mutate.inc','src/fs/ext4_orphan.inc',
            'tests/ext4_time_host.c','tests/ext4_time_clock.c','scripts/test_ext4_time_host.py')},
        'binary_sha256':hashlib.sha256((ROOT/'build/ext4_time_host').read_bytes()).hexdigest(),
        'scope':'host actual-code and Linux oracle; disposable disks removed; no physical/firmware claim'},indent=2)+'\n')

if __name__=='__main__':main()
