"""Backup identity guard controls using the first retained torn-descriptor case."""
import gzip
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import argparse
from create_ext4_fixtures import ROOT
from ext4_crash_sparse import SparseDisk
from ext4_crash_model import Fault, Disconnected
from test_jbd2_replay_host import crc, blocks
from test_ext4_crash_inventory import command


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('inventory', type=Path)
    args = parser.parse_args()
    evidence = Path(os.environ.get('FORTRESS_EXT4_CRASH_EVIDENCE', ROOT/'.codex-remote-attachments/ext4-phase9')).resolve()
    assert evidence.is_relative_to((ROOT/'.codex-remote-attachments').resolve())
    out = Path(tempfile.mkdtemp(prefix='bootstrap-',dir=evidence))
    inventory = args.inventory.resolve()
    assert inventory.is_relative_to((ROOT/'.codex-remote-attachments').resolve())
    label = '1024-normal-512-create'
    source = inventory/f'{label}-before.img.gz'
    base = gzip.decompress(source.read_bytes())
    events = [json.loads(line) for line in (inventory/f'{label}.events.jsonl').read_text().splitlines()]
    payload = (inventory/f'{label}.payload.bin').read_bytes()
    event = next(e for e in events if e['kind']=='write' and e['lba']==4 and
                 any(p['role']=='metadata' for p in e['parts']))
    disk = SparseDisk(base, 512)
    for item in events[:event['index']+1]:
        try: disk.apply(item, payload, Fault(item['index'], tear_bytes=16) if item is event else None)
        except Disconnected: break
    initial = bytearray(disk.read_stable(0, len(base)))
    first,bpg = struct.unpack_from('<I',initial,1044)[0],struct.unpack_from('<I',initial,1056)[0]
    backup = (first+bpg+1)*1024
    variants = [('valid-backup', initial, 'recover')]
    bad = bytearray(initial);bad[backup+30] ^= 1;variants.append(('backup-crc',bad,'reject'))
    bad = bytearray(initial);bad[backup] ^= 1
    seed = crc(0xffffffff,initial[1128:1144]); raw = bytearray(bad[backup:backup+32]);raw[30:32] = bytes(2)
    struct.pack_into('<H',bad,backup+30,crc(crc(seed,bytes(4)),raw)&0xffff)
    variants.append(('backup-location-mismatch',bad,'reject'))
    bad = bytearray(initial);bad[2048] ^= 1;variants.append(('primary-location-mismatch',bad,'reject'))
    work = out/'map.img';work.write_bytes(base)
    journal = blocks(work, f'<{struct.unpack_from("<I",base,1248)[0]}>');work.unlink()
    bad = bytearray(initial);where=journal[0]*1024
    superblock=bytearray(bad[where:where+1024])
    struct.pack_into('>I',superblock,28,0);struct.pack_into('>I',superblock,252,0)
    struct.pack_into('>I',superblock,252,crc(0xffffffff,superblock))
    bad[where:where+1024]=superblock
    variants.append(('backup-without-authoritative-replay',bad,'reject'))
    records = []
    for label,data,expected in variants:
        image = out/f'{label}.img';image.write_bytes(data)
        argv = [str(evidence/'bin/ext4_crash_bootstrap_host'),str(image),'512',expected,str(out/label)]
        command(argv,out/f'{label}.log')
        if expected=='recover':command(['e2fsck','-fn',str(out/f'{label}-recovered.img')],out/f'{label}.log')
        assert image.read_bytes()==data
        records.append({'label':label,'expected':expected,'argv':argv})
    (out/'manifest.json').write_text(json.dumps(records,indent=2)+'\n')
    print(f'Bootstrap guard PASS {len(records)}/{len(records)}; evidence: {out}')


if __name__=='__main__':main()
