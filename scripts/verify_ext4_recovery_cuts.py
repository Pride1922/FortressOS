"""Verify retained recovery-cut coverage and reconstruct every interrupted state."""
import argparse
import gzip
import hashlib
from itertools import zip_longest
import json
from pathlib import Path
import struct
from ext4_crash_campaign import PROFILES, settings
from ext4_crash_model import Fault, Disconnected
from ext4_crash_sparse import SparseDisk, digest
from test_ext4_recovery_cuts import decode_trace, ROOT
from test_jbd2_replay_host import blocks


def main():
    parser=argparse.ArgumentParser();parser.add_argument('campaign',type=Path);args=parser.parse_args()
    out=args.campaign.resolve();assert out.is_relative_to((ROOT/'.codex-remote-attachments').resolve())
    manifest=json.loads((out/'manifest.json').read_text());assert not manifest['errors']
    inventory=json.loads((Path(manifest['inventory'])/'manifest.json').read_text())
    inputs=Path(inventory['inputs']);fixtures={c['label']:c for c in inventory['cases']};checked=audits=0;records=[]
    for case in manifest['cases']:
        label=case['label'];folder=out/label;fixture=fixtures[label];ss=fixture['sector'];bs=fixture['block']
        base=gzip.decompress((inputs/fixture['image']).read_bytes());assert hashlib.sha256(base).hexdigest()==fixture['sha256']
        first_events,first_payload=decode_trace((folder/'baseline.trace').read_bytes(),ss)
        seed=(folder/'repeat-seed.delta').read_bytes();current=SparseDisk(base,ss)
        for at in range(0,len(seed),4+ss):current.stable[struct.unpack_from('<I',seed,at)[0]]=seed[at+4:at+4+ss]
        secondbase=current.read_stable(0,len(base));prior=dict(current.stable)
        second_events,second_payload=decode_trace((folder/'repeat-baseline.trace').read_bytes(),ss)
        declared=((stage,profile,event,after,events,payload,stable,previous) for stage,events,payload,stable,previous in
            (('first',first_events,first_payload,base,{}),('repeated',second_events,second_payload,secondbase,prior))
            for profile in PROFILES for event in range(len(events)) for after in (False,True))
        homes=set();count=0
        with (folder/'cases.jsonl').open() as stream:
            for desired,line in zip_longest(declared,stream):
                assert desired is not None and line is not None,'missing or extra recovery cut'
                stage,profile,event,after,events,payload,stable,previous=desired;record=json.loads(line)
                assert (record['stage'],record['profile'],record['event'],record['after'])==(stage,profile,event,after)
                result=record['result'];assert result['return_code']==0x100000000-5 and result['events']==event+1
                assert result['phase']==int(events[event]['published'])
                disk=SparseDisk(stable,ss,**settings(events,profile));fault=Fault(event,after=after)
                for item in events[:event+1]:
                    try:disk.apply(item,payload,fault if item['index']==event else None)
                    except Disconnected:break
                combined=dict(previous);combined.update(disk.stable)
                delta=b''.join(struct.pack('<I',lba)+value for lba,value in sorted(combined.items()) if value!=base[lba*ss:(lba+1)*ss])
                assert digest(delta)==record['input_sha256'];homes.add(record['linux_home_sha256']);count+=1
        assert count==sum(case['counts'].values()) and case['counts']=={'first':12*len(first_events),'repeated':12*len(second_events)}
        work=out/'verify-map.img';work.write_bytes(base);journal=set(blocks(work,f'<{struct.unpack_from("<I",base,1248)[0]}>'));work.unlink()
        for key in homes:
            delta=(folder/f'linux-{key}.delta').read_bytes();home=bytearray();assert (folder/f'linux-{key}.log').is_file()
            for at in range(0,len(delta),4+ss):
                lba=struct.unpack_from('<I',delta,at)[0];value=delta[at+4:at+4+ss]
                for skip in range(0,ss,min(bs,ss)):
                    if (lba*ss+skip)//bs not in journal:home+=struct.pack('<QI',lba*ss+skip,min(bs,ss))+value[skip:skip+min(bs,ss)]
            assert digest(home)==key;audits+=1
        assert len(homes)==case['linux_audits'];checked+=count
        records.append({'label':label,'cases':count,'ledger_sha256':hashlib.sha256((folder/'cases.jsonl').read_bytes()).hexdigest()})
    assert checked==manifest['total']
    (out/'ledger-verification.json').write_text(json.dumps({'cases':checked,'reconstructed_inputs':checked,'linux_deltas':audits,'records':records},indent=2)+'\n')
    print(f'Recovery ledger PASS: {checked} exact cuts and reconstructed inputs; {audits} Linux deltas')


if __name__=='__main__':main()
