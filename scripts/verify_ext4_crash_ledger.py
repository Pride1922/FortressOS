"""Check every declared schedule identity and retained audit delta independently."""
import argparse
import gzip
import hashlib
import json
import struct
from pathlib import Path
from itertools import zip_longest
from ext4_crash_campaign import PROFILES, settings
from ext4_crash_model import Fault, Disconnected
from ext4_crash_sparse import SparseDisk, digest
from test_ext4_crash_recovery import faults, ROOT
from test_jbd2_replay_host import blocks


def main():
    parser=argparse.ArgumentParser();parser.add_argument('campaign',type=Path);args=parser.parse_args()
    out=args.campaign.resolve()
    assert out.is_relative_to((ROOT/'.codex-remote-attachments').resolve())
    manifest=json.loads((out/'manifest.json').read_text())
    assert not manifest['errors'];source=Path(manifest['inventory'])
    inventory=json.loads((source/'manifest.json').read_text())
    baselines={f'{b["block"]}-{b["placement"]}-{b["sector"]}-{b["operation"]}':b for b in inventory['cases']}
    checked=samples=audits=0;records=[]
    for entry in manifest['cases']:
        assert entry['complete'];label=entry['label'];bs,_,ss,_=label.split('-',3);bs,ss=int(bs),int(ss)
        base=gzip.decompress((source/f'{label}-before.img.gz').read_bytes())
        payload=(source/f'{label}.payload.bin').read_bytes()
        baseline=baselines[label]
        preimage=next(i for i in baseline['images'] if i['file']==f'{label}-before.img.gz')
        assert hashlib.sha256(base).hexdigest()==preimage['sha256']
        assert hashlib.sha256(payload).hexdigest()==baseline['payload_sha256']
        events=[json.loads(line) for line in (source/f'{label}.events.jsonl').read_text().splitlines()]
        folder=out/label;work=out/'verify-map.img';work.write_bytes(base)
        journal=blocks(work,f'<{struct.unpack_from("<I",base,1248)[0]}>')
        wanted=((profile,kind,fault,config) for profile in PROFILES for kind,fault,config in faults(events,ss,profile))
        home_keys=set();seen={};count=0
        with (folder/'cases.jsonl').open() as stream:
            for declared,line in zip_longest(wanted,stream):
                assert declared is not None and line is not None,'missing or extra declared schedule'
                profile,kind,fault,config=declared;case=json.loads(line)
                assert case['profile']==profile and case['kind']==kind
                assert case['fault']==json.loads(json.dumps(fault.__dict__))
                result=case['result'];assert result['expected']==case['expected']
                assert result['status']==int(case['expected']==2)
                if result['status']:assert not result['writes'] and not result['flushes']
                else:assert result['return_code']==0;home_keys.add(result['linux_home_sha256'])
                key=case['input_delta_sha256']
                if key in seen:assert seen[key]==result
                else:seen[key]=result
                # Reconstruct deterministic samples from immutable input and
                # event payload, including every profile's first cut.
                if count%257==0 or fault.event==0:
                    disk=SparseDisk(base,ss,**config)
                    for event in events[:fault.event+1]:
                        try:disk.apply(event,payload,fault if event['index']==fault.event else None)
                        except Disconnected:break
                    assert digest(disk.canonical())==key;samples+=1
                count+=1
        assert count==sum(entry['expected_counts'].values())
        assert len(seen)==entry['outcomes']['unique_inputs']
        for key in home_keys:
            delta=(folder/f'linux-{key}.delta').read_bytes();home=bytearray()
            assert (folder/f'linux-{key}.log').is_file()
            for at in range(0,len(delta),4+ss):
                lba=struct.unpack_from('<I',delta,at)[0];value=delta[at+4:at+4+ss]
                for skip in range(0,ss,min(bs,ss)):
                    if (lba*ss+skip)//bs not in journal:
                        home+=struct.pack('<QI',lba*ss+skip,min(bs,ss))+value[skip:skip+min(bs,ss)]
            assert digest(home)==key;audits+=1
        assert len(home_keys)==entry['outcomes']['linux_audits']
        records.append({'label':label,'cases':count,'ledger_sha256':hashlib.sha256((folder/'cases.jsonl').read_bytes()).hexdigest()})
        checked+=count
    work.unlink();assert checked==manifest['total']
    (out/'ledger-verification.json').write_text(json.dumps({'cases':checked,'reconstructed_input_samples':samples,
        'verified_linux_deltas':audits,'records':records},indent=2)+'\n')
    print(f'Ledger PASS: {checked} exact schedules, {samples} reconstructed inputs, {audits} retained Linux deltas')


if __name__=='__main__':main()
