"""Independent recovery truth: known prefixes and deliberate media changes."""
import argparse
import gzip
import json
import os
from pathlib import Path
import struct
from ext4_crash_sparse import SparseDisk
from ext4_crash_recovery_oracle import Truth
from test_jbd2_replay_host import blocks
from test_ext4_crash_inventory import ROOT
import tempfile


def main():
    parser = argparse.ArgumentParser(); parser.add_argument('inventory', type=Path)
    args = parser.parse_args(); source = args.inventory.resolve()
    assert source.is_relative_to((ROOT/'.codex-remote-attachments').resolve())
    evidence = Path(os.environ.get('FORTRESS_EXT4_CRASH_EVIDENCE', ROOT/'.codex-remote-attachments/ext4-phase9')).resolve()
    assert evidence.is_relative_to((ROOT/'.codex-remote-attachments').resolve())
    out = Path(tempfile.mkdtemp(prefix='oracle-', dir=evidence))
    records = []
    for bs in (1024,2048,4096):
        for placement in ('normal','wrap'):
            for ss in (512,4096):
                label = f'{bs}-{placement}-{ss}-create'
                base = gzip.decompress((source/f'{label}-before.img.gz').read_bytes())
                work = out/'map.img';work.write_bytes(base)
                mapping = blocks(work, f'<{struct.unpack_from("<I",base,1248)[0]}>')
                events = [json.loads(line) for line in (source/f'{label}.events.jsonl').read_text().splitlines()]
                payload = (source/f'{label}.payload.bin').read_bytes()
                truth = Truth(base,ss,bs,events,payload,mapping)
                old = SparseDisk(base,ss);assert truth.expected(old)[0]==0
                new = SparseDisk(base,ss)
                for event in events:new.apply(event,payload,None)
                assert truth.expected(new)[0]==1
                bad = SparseDisk(base,ss);offset = mapping[0]*bs+252
                lba,at = divmod(offset,ss);sector=bytearray(base[lba*ss:(lba+1)*ss]);sector[at]^=1
                bad.stable[lba]=bytes(sector)
                assert truth.expected(bad)==(2,'journal-superblock-crc')
                # A healthy-looking old home image cannot stand in for the
                # committed metadata prefix once the checkpoint is complete.
                block = next(b for b,v in truth.transactions[0]['images'].items() if v!=base[b*bs:(b+1)*bs])
                for lba in range(block*bs//ss,(block*bs+bs-1)//ss+1):
                    sector = bytearray(new.read_stable(lba*ss,ss))
                    lo=max(block*bs,lba*ss);hi=min((block+1)*bs,(lba+1)*ss)
                    sector[lo-lba*ss:hi-lba*ss]=base[lo:hi];new.stable[lba]=bytes(sector)
                assert truth.expected(new)[0]==2
                records.append({'label':label,'controls':4})
    work.unlink()
    (out/'manifest.json').write_text(json.dumps(records,indent=2)+'\n')
    print(f'Recovery oracle PASS: 48 prefix/corruption controls; evidence: {out}')


if __name__=='__main__':main()
