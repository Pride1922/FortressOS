"""Explicit multi-orphan disposable fixtures, crash retries and Linux audits."""
import gzip
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
from create_ext4_fixtures import ROOT
from test_jbd2_replay_host import oracle


def main():
    source=ROOT/'build/ext4-phase8-4/run-a7txzk0q'
    assert (source/'manifest.json').is_file()
    evidence=ROOT/'.codex-remote-attachments/ext4-phase9'
    out=Path(tempfile.mkdtemp(prefix='multi-orphan-',dir=evidence));records=[];errors=[]
    binary=evidence/'bin/ext4_multi_orphan_host'
    try:
        for bs in (1024,2048,4096):
            for placement in ('normal','wrap'):
                image=source/f'{bs}-{placement}.img';cfg=source/f'config-{bs}.bin'
                for ss in (512,4096):
                    prefix=out/f'{bs}-{placement}-{ss}';argv=[str(binary),str(image),str(cfg),str(ss),str(prefix)]
                    with Path(str(prefix)+'.log').open('w') as stream:
                        result=subprocess.run(argv,stdout=stream,stderr=subprocess.STDOUT,timeout=7200)
                    assert result.returncode==0,(argv,result.returncode)
                    text=Path(str(prefix)+'.log').read_text();match=re.search(r'MULTI ORPHAN PASS.*events=(\d+) cuts=(\d+) repeated=(\d+)',text);assert match
                    assert int(match[2])==8*int(match[1])
                    for phase in ('pending','clean'):
                        original=Path(f'{prefix}-{phase}.img');linux=Path(str(original)+'.linux.img')
                        oracle(original,linux,Path(str(original)+'.linux.log'))
                        for name in ('victim.bin','large.bin'):
                            stat=subprocess.run(['debugfs','-R',f'stat /{name}',str(linux)],capture_output=True,text=True,check=True)
                            assert 'File not found' in stat.stderr
                        dump=Path(str(original)+'.tree.bin')
                        subprocess.run(['debugfs','-R',f'dump /tree.bin {dump}',str(linux)],capture_output=True,check=True)
                        assert dump.read_bytes()==bytes(bs)
                        for retained in (original,linux):
                            raw=retained.read_bytes()
                            with gzip.open(str(retained)+'.gz','wb',compresslevel=1) as stream:stream.write(raw)
                            assert hashlib.sha256(gzip.decompress(Path(str(retained)+'.gz').read_bytes())).digest()==hashlib.sha256(raw).digest()
                            retained.unlink()
                    records.append({'block':bs,'sector':ss,'placement':placement,'argv':argv,'events':int(match[1]),'cuts':int(match[2]),'repeated':int(match[3]),'source_sha256':hashlib.sha256(image.read_bytes()).hexdigest()})
                    print(f'PASS multi-orphan {bs}-{placement}-{ss}: {match[2]} cuts',flush=True)
    except Exception as error:errors.append(repr(error));raise
    finally:
        (out/'manifest.json').write_text(json.dumps({'cases':records,'errors':errors,'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest()},indent=2)+'\n')
        print(f'Evidence: {out}',flush=True)


if __name__=='__main__':main()
