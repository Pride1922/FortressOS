"""Actual ext4/VFS with disposable Linux images and independent byte/fsck audit."""
import hashlib,json,tempfile
from pathlib import Path
from create_ext4_fixtures import ROOT,FEATURES,run

def main():
    parent=ROOT/'build/ext4-phase4';parent.mkdir(exist_ok=True)
    out=Path(tempfile.mkdtemp(prefix='host-',dir=parent));records=[]
    versions=run(['mke2fs','-V']);(out/'versions.txt').write_text(versions)
    payload=out/'seed.bin';payload.write_bytes(bytes(range(256))*17)
    for bs in (1024,2048,4096):
        source=out/f'source-{bs}.img'
        with source.open('xb') as f:f.truncate(64*1024*1024)
        mkfs=['mke2fs','-q','-t','ext4','-b',str(bs),'-I','256','-O',FEATURES,'-E','lazy_itable_init=0','-m','0','-g','4096','-N','512',str(source)]
        log=run(mkfs)+run(['debugfs','-w','-R',f'write {payload} /seed.bin',str(source)])
        log+=run(['debugfs','-w','-R','write /dev/null /unwritten.bin',str(source)])
        log+=run(['debugfs','-w','-R','fallocate /unwritten.bin 0 7',str(source)])
        log+=run(['debugfs','-w','-R',f'set_inode_field /unwritten.bin size {bs*8}',str(source)])
        log+=run(['e2fsck','-fn',str(source)])+run(['dumpe2fs','-h',str(source)])
        for ss in (512,4096):
            result=out/f'result-{bs}-{ss}.img'
            cmd=[str(ROOT/'build/ext4_write_host'),str(source),str(result),str(ss)]
            text=run(cmd);print(text,end='',flush=True)
            text+=run(['e2fsck','-fn',str(result)])
            dump=out/f'large-{bs}-{ss}.bin';run(['debugfs','-R',f'dump /large.bin {dump}',str(result)])
            assert dump.read_bytes()==bytes((i*17+3)&255 for i in range(16*1024*1024))
            (out/f'{bs}-{ss}.log').write_text(log+text)
            records.append({'bs':bs,'sector':ss,'argv':cmd,'mkfs':mkfs,'sha256':hashlib.sha256(result.read_bytes()).hexdigest()})
            print(f'PASS Linux fsck and exact 16MiB bytes bs={bs} sector={ss}',flush=True)
    (out/'manifest.json').write_text(json.dumps(records,indent=2)+'\n');print(f'Evidence: {out}')
if __name__=='__main__':main()
