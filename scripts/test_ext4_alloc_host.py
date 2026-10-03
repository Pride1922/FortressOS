"""Phase-3 actual-engine mutation on fresh regular images; Linux offline audit."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
from create_ext4_fixtures import ROOT, FEATURES, run

def main():
    parent=ROOT/'build/ext4-phase3';parent.mkdir(exist_ok=True)
    out=Path(tempfile.mkdtemp(prefix='run-',dir=parent));records=[]
    for bs in (1024,2048,4096):
        source=out/f'source-{bs}.img'
        with source.open('xb') as f:f.truncate(32*1024*1024)
        mkfs=['mke2fs','-q','-t','ext4','-b',str(bs),'-I','256','-O',FEATURES,'-E','lazy_itable_init=0','-m','0','-g','4096','-N','128',str(source)]
        evidence=run(mkfs)
        evidence+=run(['debugfs','-w','-R','write /dev/null /engine.bin',str(source)])
        evidence+=run(['debugfs','-w','-R','write /dev/null /blocker.bin',str(source)])
        blocker_stat=run(['debugfs','-R','stat /blocker.bin',str(source)])
        blocker=int(re.search(r'Inode:\s+(\d+)',blocker_stat).group(1))
        stat=run(['debugfs','-R','stat /engine.bin',str(source)])
        ino=int(re.search(r'Inode:\s+(\d+)',stat).group(1))
        for ss in (512,4096):
            target=out/f'result-{bs}-{ss}.img'
            cmd=[str(ROOT/'build/ext4_alloc_host'),str(source),str(target),str(ss),str(ino),str(blocker)]
            result=run(cmd);print(result,end='',flush=True)
            check=run(['e2fsck','-fn',str(target)])
            for suffix in ('contiguous','promotion','depth2','crossgroup'):
                snapshot=Path(str(target)+f'.{suffix}.img')
                check+=run(['e2fsck','-fn',str(snapshot)])
                dump=out/f'{bs}-{ss}-{suffix}.bin'
                run(['debugfs','-R',f'dump /engine.bin {dump}',str(snapshot)])
                expected=1048576 if suffix=='contiguous' else 9*bs if suffix=='promotion' else (8*((bs-12)//12)+1)*bs
                if suffix=='crossgroup': expected=dump.stat().st_size
                assert dump.stat().st_size==expected
                assert dump.read_bytes()==bytes(expected)
                check+=run(['debugfs','-R','stat /engine.bin',str(snapshot)])
            high=Path(str(target)+'.highoffset.img')
            check+=run(['e2fsck','-fn',str(high)])
            high_stat=run(['debugfs','-R','stat /engine.bin',str(high)])
            assert int(re.search(r'Size:\s+(\d+)',high_stat).group(1))==2**32+8*bs
            check+=high_stat
            dump=out/f'empty-{bs}-{ss}.bin';run(['debugfs','-R',f'dump /engine.bin {dump}',str(target)])
            assert dump.stat().st_size==0
            (out/f'{bs}-{ss}.log').write_text(evidence+stat+result+check)
            records.append({'bs':bs,'sector':ss,'argv':cmd,'mkfs':mkfs,'sha256':hashlib.sha256(target.read_bytes()).hexdigest()})
            print(f'PASS Linux e2fsck bs={bs} sector={ss}',flush=True)
        maximum=out/f'max-source-{bs}.img'
        with maximum.open('xb') as f:f.truncate(32*1024*1024)
        max_mkfs=mkfs[:-1]+[str(maximum)];max_evidence=run(max_mkfs)
        payload=out/f'max-payload-{bs}.bin'
        with payload.open('xb') as f:
            for k in range(4095):
                f.seek(k*2*bs);f.write(bytes([0x2b])*bs)
        max_evidence+=run(['debugfs','-w','-R',f'write {payload} /engine.bin',str(maximum)])
        max_stat=run(['debugfs','-R','stat /engine.bin',str(maximum)])
        max_ino=int(re.search(r'Inode:\s+(\d+)',max_stat).group(1))
        for ss in (512,4096):
            target=out/f'max-result-{bs}-{ss}.img'
            cmd=[str(ROOT/'build/ext4_alloc_host'),str(maximum),str(target),str(ss),str(max_ino),'0','maximum']
            result=run(cmd);print(result,end='',flush=True)
            check=run(['e2fsck','-fn',str(target)])
            dump=out/f'max-dump-{bs}-{ss}.bin'
            run(['debugfs','-R',f'dump /engine.bin {dump}',str(target)])
            expected=payload.read_bytes()+bytes(2*bs)
            assert dump.read_bytes()==expected
            (out/f'max-{bs}-{ss}.log').write_text(max_evidence+max_stat+result+check)
            records.append({'kind':'maximum','bs':bs,'sector':ss,'argv':cmd,'mkfs':max_mkfs,'sha256':hashlib.sha256(target.read_bytes()).hexdigest()})
            print(f'PASS Linux maximum-map fsck/bytes bs={bs} sector={ss}',flush=True)
    (out/'manifest.json').write_text(json.dumps(records,indent=2)+'\n')
    print(f'Evidence: {out}')

if __name__=='__main__':main()
