"""Inventory actual mounted recovery and independently audit its final media."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import tempfile
from test_ext4_crash_inventory import ROOT, OPS, command, linux_snapshot


def main():
    parser=argparse.ArgumentParser();parser.add_argument('inputs',type=Path);args=parser.parse_args()
    allowed=(ROOT/'.codex-remote-attachments').resolve();source=args.inputs.resolve()
    assert source.is_relative_to(allowed)
    fixtures=json.loads((source/'manifest.json').read_text());assert not fixtures['errors']
    evidence=allowed/'ext4-phase9';binary=evidence/'bin/ext4_recovery_inventory_host'
    out=Path(tempfile.mkdtemp(prefix='recovery-inventory-',dir=evidence));records=[];errors=[]
    try:
        for case in fixtures['cases']:
            label=case['label'];image=out/f'{label}.img';prefix=out/label;log=out/f'{label}.log'
            raw=gzip.decompress((source/case['image']).read_bytes())
            assert hashlib.sha256(raw).hexdigest()==case['sha256'];image.write_bytes(raw)
            command([str(binary),str(image),str(case['sector']),str(OPS.index(case['operation'])),str(prefix)],log)
            clean=out/f'{label}-clean.img'
            linux_snapshot(clean,case['block'],case['operation'],out/f'{label}-linux.log',committed=True)
            events=[json.loads(line) for line in (out/f'{label}.events.jsonl').read_text().splitlines()]
            assert events and [e['index'] for e in events]==list(range(len(events)))
            payload=(out/f'{label}.payload.bin').read_bytes()
            assert len(payload)==sum(e['kind']=='write' for e in events)*case['sector']
            records.append(dict(case,events=len(events),payload_sha256=hashlib.sha256(payload).hexdigest(),
                clean_sha256=hashlib.sha256(clean.read_bytes()).hexdigest()))
            with gzip.open(str(clean)+'.gz','wb',compresslevel=1) as stream:stream.write(clean.read_bytes())
            clean.unlink();image.unlink()
            print(f'PASS mounted recovery inventory and Linux audit: {label}',flush=True)
        assert len(records)==60
    except Exception as error:
        errors.append(repr(error));raise
    finally:
        (out/'manifest.json').write_text(json.dumps({'scope':'Phase 9.3 recovery inventory; interruption campaign pending',
            'inputs':str(source),'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),
            'cases':records,'errors':errors},indent=2)+'\n')
        print(f'Evidence: {out}',flush=True)


if __name__=='__main__':main()
