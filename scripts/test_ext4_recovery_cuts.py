"""Actual recovery interruptions, calibrated media checks and repeated cuts."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import shutil
import tempfile
import time
from ext4_crash_campaign import PROFILES, settings
from ext4_crash_model import Fault, Disconnected
from ext4_crash_sparse import SparseDisk, digest
from test_ext4_crash_inventory import ROOT, OPS, linux_snapshot
from test_ext4_crash_recovery import read_exact, write_image
from test_jbd2_replay_host import blocks


def decode_trace(raw,ss):
    events=[];payload=bytearray();at=0
    while at<len(raw):
        kind,lba,published,length=struct.unpack_from('<IIII',raw,at);at+=16
        assert kind in (0,1) and length==(ss if kind else 0) and published<2
        events.append({'index':len(events),'kind':'write' if kind else 'flush','lba':lba,
            'published':bool(published),'payload_offset':len(payload)})
        payload+=raw[at:at+length];at+=length
    assert at==len(raw)
    return events,bytes(payload)


def main():
    parser=argparse.ArgumentParser();parser.add_argument('inventory',type=Path);parser.add_argument('--label');args=parser.parse_args()
    allowed=(ROOT/'.codex-remote-attachments').resolve();source=args.inventory.resolve();assert source.is_relative_to(allowed)
    inventory=json.loads((source/'manifest.json').read_text());assert not inventory['errors']
    inputs=Path(inventory['inputs']);evidence=allowed/'ext4-phase9'
    out=Path(tempfile.mkdtemp(prefix='recovery-cuts-',dir=evidence));binary=evidence/'bin/ext4_recovery_cut_host'
    snapshot=out/'source-snapshot';snapshot.mkdir();hashes={}
    for path in [Path(__file__),ROOT/'tests/ext4_recovery_cut_host.c',
                 *sorted((ROOT/'tests').glob('ext4*.*')),*sorted((ROOT/'src/fs').glob('ext4*')),
                 ROOT/'src/fs/jbd2.c',ROOT/'src/fs/jbd2.h',ROOT/'scripts/ext4_crash_sparse.py',
                 ROOT/'scripts/ext4_crash_campaign.py',ROOT/'scripts/test_ext4_crash_inventory.py']:
        relative=path.relative_to(ROOT);target=snapshot/relative;target.parent.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(path,target);hashes[str(relative)]=hashlib.sha256(path.read_bytes()).hexdigest()
    manifest={'scope':'9.3 six-profile atomic recovery cuts; one repeated-interruption seed per label',
        'inventory':str(source),'argv':__import__('sys').argv,'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),
        'sources':hashes,'cases':[],'errors':[]};total=0;started=time.monotonic()
    try:
        for case in inventory['cases']:
            label=case['label']
            if args.label and label!=args.label:continue
            folder=out/label;folder.mkdir();ss=case['sector'];bs=case['block'];op=OPS.index(case['operation'])
            base=gzip.decompress((inputs/case['image']).read_bytes());assert hashlib.sha256(base).hexdigest()==case['sha256']
            basefile=folder/'base.img';basefile.write_bytes(base);work=folder/'working.img'
            journal=set(blocks(basefile,f'<{struct.unpack_from("<I",base,1248)[0]}>'))
            stderr=(folder/'worker.log').open('wb');worker=subprocess.Popen([str(binary),str(basefile),str(ss)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=stderr)
            ledger=(folder/'cases.jsonl').open('w');healthy={};linux={};initial_cache={};counts={'first':0,'repeated':0}
            def request(delta,cut=0xffffffff,after=False,profile=0):
                worker.stdin.write(struct.pack('<IIIII',len(delta)//(4+ss),op,cut,int(after),profile)+delta);worker.stdin.flush()
                deadline=time.monotonic()+180;header=read_exact(worker.stdout,28,deadline)
                result,phase,events,writes,flushes,count,length=struct.unpack('<IIIIIII',header)
                media=read_exact(worker.stdout,count*(4+ss),deadline);trace=read_exact(worker.stdout,length,deadline)
                return {'return_code':result,'phase':phase,'events':events,'writes':writes,'flushes':flushes},media,trace
            def recover(delta):
                key=digest(delta)
                if key not in healthy:
                    result,media,trace=request(delta);assert result['return_code']==0 and result['phase']==2
                    home=bytearray()
                    for at in range(0,len(media),4+ss):
                        lba=struct.unpack_from('<I',media,at)[0];value=media[at+4:at+4+ss]
                        for skip in range(0,ss,min(bs,ss)):
                            if (lba*ss+skip)//bs not in journal:home+=struct.pack('<QI',lba*ss+skip,min(bs,ss))+value[skip:skip+min(bs,ss)]
                    homekey=digest(home)
                    if homekey not in linux:
                        write_image(work,base,media,ss);linux_snapshot(work,bs,case['operation'],folder/f'linux-{homekey}.log',committed=True)
                        (folder/f'linux-{homekey}.delta').write_bytes(media);linux[homekey]=True
                    healthy[key]=(result,media,trace,homekey)
                return healthy[key]
            def interrupt(initial,events,payload,event,after,profile):
                initialkey=digest(initial)
                if initialkey not in initial_cache:
                    current=SparseDisk(base,ss)
                    for at in range(0,len(initial),4+ss):
                        lba=struct.unpack_from('<I',initial,at)[0];current.stable[lba]=initial[at+4:at+4+ss]
                    initial_cache[initialkey]=(current.read_stable(0,len(base)),dict(current.stable))
                stable,prior=initial_cache[initialkey];model=SparseDisk(stable,ss,**settings(events,PROFILES[profile]))
                fault=Fault(event,after=after)
                for item in events[:event+1]:
                    try:model.apply(item,payload,fault if item['index']==event else None)
                    except Disconnected:break
                combined=dict(prior);combined.update(model.stable)
                expected=b''.join(struct.pack('<I',lba)+value for lba,value in sorted(combined.items()) if value!=base[lba*ss:(lba+1)*ss])
                (folder/'current-initial.delta').write_bytes(initial)
                result,media,trace=request(initial,event,after,profile)
                (folder/'current-output.delta').write_bytes(media)
                assert result['return_code']==(0x100000000-5) and result['phase']<2,result
                assert result['events']==event+1 and media==expected,'native/model persistence disagreement'
                observed,observed_payload=decode_trace(trace,ss)
                assert observed==events[:event+1] and observed_payload==payload[:sum(e['kind']=='write' for e in events[:event+1])*ss]
                return result,media
            try:
                baseline= recover(b'');events,payload=decode_trace(baseline[2],ss)
                (folder/'baseline.trace').write_bytes(baseline[2]);assert len(events)==case['events']
                original=[json.loads(line) for line in (source/f'{label}.events.jsonl').read_text().splitlines()]
                assert events==original and hashlib.sha256(payload).hexdigest()==case['payload_sha256']
                flushes=[e['index'] for e in events if e['kind']=='flush' and not e['published']];assert flushes
                seed=flushes[len(flushes)//2];repeat=None
                for profile in range(6):
                    for event in range(len(events)):
                        for after in (False,True):
                            identity={'stage':'first','profile':PROFILES[profile],'event':event,'after':after}
                            (folder/'current-case.json').write_text(json.dumps(identity)+'\n')
                            result,media=interrupt(b'',events,payload,event,after,profile);final=recover(media)
                            ledger.write(json.dumps(dict(identity,input_sha256=digest(media),result=result,linux_home_sha256=final[3]))+'\n')
                            counts['first']+=1;total+=1
                            if total%100==0:ledger.flush();print(f'progress checks={total} label={label} stage=first',flush=True)
                            if profile==0 and event==seed and not after:repeat=(media,final[2])
                assert repeat is not None
                initial,trace=repeat;second_events,second_payload=decode_trace(trace,ss)
                (folder/'repeat-seed.delta').write_bytes(initial);(folder/'repeat-baseline.trace').write_bytes(trace)
                for profile in range(6):
                    for event in range(len(second_events)):
                        for after in (False,True):
                            identity={'stage':'repeated','seed_event':seed,'profile':PROFILES[profile],'event':event,'after':after}
                            (folder/'current-case.json').write_text(json.dumps(identity)+'\n')
                            result,media=interrupt(initial,second_events,second_payload,event,after,profile);final=recover(media)
                            ledger.write(json.dumps(dict(identity,input_sha256=digest(media),result=result,linux_home_sha256=final[3]))+'\n')
                            counts['repeated']+=1;total+=1
                            if total%100==0:ledger.flush();print(f'progress checks={total} label={label} stage=repeated',flush=True)
                assert counts=={'first':12*len(events),'repeated':12*len(second_events)}
                manifest['cases'].append({'label':label,'counts':counts,'healthy_inputs':len(healthy),'linux_audits':len(linux)})
                print(f'PASS {label}: {counts}, unique healthy recoveries={len(healthy)}, Linux audits={len(linux)}',flush=True)
                basefile.unlink();work.unlink(missing_ok=True)
            finally:
                ledger.close();worker.stdin.close()
                try:code=worker.wait(timeout=10)
                except subprocess.TimeoutExpired:worker.kill();code=worker.wait(timeout=10)
                stderr.close();assert code==0,f'worker exit {code}'
        assert manifest['cases'] and (args.label or len(manifest['cases'])==60)
    except Exception as error:manifest['errors'].append(repr(error));raise
    finally:
        manifest.update(total=total,seconds=time.monotonic()-started)
        (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n');print(f'Evidence: {out}',flush=True)


if __name__=='__main__':main()
