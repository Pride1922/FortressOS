"""Reconstruct explicitly selected mounted recovery inputs from verified ledgers."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import tempfile
from ext4_crash_campaign import settings
from ext4_crash_model import Fault, Disconnected
from ext4_crash_sparse import SparseDisk, digest
from create_ext4_fixtures import ROOT


def main():
    parser=argparse.ArgumentParser();parser.add_argument('campaign',type=Path);args=parser.parse_args()
    allowed=(ROOT/'.codex-remote-attachments').resolve();campaign=args.campaign.resolve()
    assert campaign.is_relative_to(allowed)
    manifest=json.loads((campaign/'manifest.json').read_text())
    verified=json.loads((campaign/'ledger-verification.json').read_text())
    assert not manifest['errors'] and verified['cases']==manifest['total']
    inventory=Path(manifest['inventory']);records=[]
    out=Path(tempfile.mkdtemp(prefix='recovery-inputs-',dir=allowed/'ext4-phase9'))
    for entry in manifest['cases']:
        label=entry['label'];bs,placement,ss,op=label.split('-',3);ss=int(ss)
        if op not in ('write','truncate','open-unlink','last-close','reuse'):continue
        assert entry['complete']
        ledger=campaign/label/'cases.jsonl'
        proof=next(item for item in verified['records'] if item['label']==label)
        assert hashlib.sha256(ledger.read_bytes()).hexdigest()==proof['ledger_sha256']
        cases=[json.loads(line) for line in ledger.read_text().splitlines()]
        candidates=[c for c in cases if c['expected']==1 and c['profile']=='cached' and
                    c['kind']=='atomic' and c['result']['writes']>0]
        assert candidates, label
        # Earliest durable committed state retains the longest recovery work.
        chosen=min(candidates,key=lambda c:(c['fault']['event'],c['fault']['after']))
        base=gzip.decompress((inventory/f'{label}-before.img.gz').read_bytes())
        payload=(inventory/f'{label}.payload.bin').read_bytes()
        events=[json.loads(line) for line in (inventory/f'{label}.events.jsonl').read_text().splitlines()]
        disk=SparseDisk(base,ss,**settings(events,chosen['profile']));fault=Fault(**chosen['fault'])
        for event in events[:fault.event+1]:
            try:disk.apply(event,payload,fault if event['index']==fault.event else None)
            except Disconnected:break
        assert digest(disk.canonical())==chosen['input_delta_sha256']
        image=disk.read_stable(0,len(base));name=f'{label}.img.gz'
        with (out/name).open('wb') as stream:
            with gzip.GzipFile(filename='',mode='wb',fileobj=stream,mtime=0) as compressed:compressed.write(image)
        records.append({'label':label,'block':int(bs),'sector':ss,'placement':placement,
            'operation':op,'image':name,'sha256':hashlib.sha256(image).hexdigest(),
            'source_case':chosen,'source_ledger_sha256':proof['ledger_sha256']})
    assert len(records)==60
    (out/'manifest.json').write_text(json.dumps({'scope':'Phase 9.3 input preparation; no recovery-cut acceptance',
        'campaign':str(campaign),'cases':records,'errors':[]},indent=2)+'\n')
    print(f'Reconstructed 60 verified recovery inputs: {out}')


if __name__=='__main__':main()
