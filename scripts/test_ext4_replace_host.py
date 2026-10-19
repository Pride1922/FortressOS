#!/usr/bin/env python3
"""Disposable mounted replacement tests with independent Linux audits."""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import argparse,hashlib,json,subprocess,tempfile,re
from test_ext4_time_host import fixture
from create_ext4_fixtures import ROOT,run
from test_jbd2_replay_host import blocks,put,seal

def profile(parent,bs,sector,wrap,quick=False):
    with tempfile.TemporaryDirectory(prefix='ext4-replace-') as tmp:
        folder=Path(tmp);source=fixture(folder,bs)
        if wrap:
            data=bytearray(source.read_bytes());mapping=blocks(source,'<8>');at=mapping[0]*bs
            js=bytearray(data[at:at+1024]);put(js,24,0xffffffff);put(js,88,len(mapping)-2)
            seal(js,252,0xffffffff);data[at:at+1024]=js;source.write_bytes(data)
        original=hashlib.sha256(source.read_bytes()).hexdigest()
        name=f'{bs}-{sector}-'+('wrap' if wrap else 'normal');prefix=folder/name
        command=[str(ROOT/'build/ext4_replace_host'),str(source),str(sector),str(prefix)]+(['wrap'] if wrap else [])
        if quick:command=[str(ROOT/'build/ext4_replace_host'),str(source),str(sector),str(prefix),'wrap' if wrap else 'normal','quick']
        log=parent/(name+'.log')
        with log.open('w') as stream:
            result=subprocess.run(command,stdout=stream,stderr=subprocess.STDOUT,text=True,timeout=1800)
        assert result.returncode==0,(name,log.read_text())
        audit=''
        for suffix,path in [('retained','/b/target'),('same-directory','/a/peer'),('recovered','/b/target')]:
            image=Path(str(prefix)+'-'+suffix+'.img');audit+=run(['e2fsck','-fn',str(image)])
            target=folder/'payload';audit+=run(['debugfs','-R',f'dump {path} {target}',str(image)])
            assert target.read_bytes()==b'N'*2051;target.unlink()
            absent=run(['debugfs','-R','stat /a/source',str(image)]);assert 'File not found' in absent;audit+=absent
        assert hashlib.sha256(source.read_bytes()).hexdigest()==original
        (parent/(name+'-linux.log')).write_text(audit)
        print('PASS replacement '+name,flush=True)
        cuts=int(re.search(r'PASS replace mounted.*cuts=(\d+)',log.read_text())[1])
        return {'block':bs,'sector':sector,'wrap':wrap,'cuts':cuts,'source_sha256':original,'result':'PASS'}

def seal_evidence(parent,results):
    for result in results:
        name=f'{result["block"]}-{result["sector"]}-'+('wrap' if result['wrap'] else 'normal')
        text=(parent/(name+'.log')).read_text()
        result['cuts']=int(re.search(r'PASS replace mounted.*cuts=(\d+)',text)[1])
        result['host_log_sha256']=hashlib.sha256(text.encode()).hexdigest()
        result['linux_log_sha256']=hashlib.sha256((parent/(name+'-linux.log')).read_bytes()).hexdigest()
    paths=['src/fs/ext4.c','src/fs/ext4_replace.inc','src/fs/ext4_transaction.inc','src/fs/ext4_mount_journal.inc',
           'src/fs/ext4_permissions.inc','src/fs/ext4_write.inc','src/fs/vfs.c','src/fs/vfs.h','src/include/syscall_abi.h','src/kernel/syscall.c']
    manifest={'profiles':results,'total_modeled_cuts':sum(r['cuts'] for r in results),
              'source_sha256':{p:hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in paths},
              'binary_sha256':hashlib.sha256((ROOT/'build/ext4_replace_host').read_bytes()).hexdigest(),
              'scope':'host sanitizers, modeled sector-atomic cuts, Linux oracle; no physical claim'}
    (parent/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')

def main():
    p=argparse.ArgumentParser();p.add_argument('--first',action='store_true');p.add_argument('--quick',action='store_true');p.add_argument('--audit-existing',action='store_true');args=p.parse_args()
    label='ext4-replace-quick' if args.quick else 'ext4-replace-first' if args.first else 'ext4-replace'
    parent=ROOT/'build'/label;parent.mkdir(parents=True,exist_ok=True)
    if args.audit_existing:
        results=json.loads((parent/'manifest.json').read_text())['profiles'];assert len(results)==12
        seal_evidence(parent,results)
        assert all(r['cuts']>0 for r in results),'full modeled-cut logs required'
        return
    jobs=[(4096,512,False)] if args.first else [(bs,ss,wrap) for bs in (1024,2048,4096) for ss in (512,4096) for wrap in (False,True)]
    with ThreadPoolExecutor(max_workers=3) as pool:
        results=list(pool.map(lambda x:profile(parent,*x,args.quick),jobs))
    seal_evidence(parent,results)

if __name__=='__main__':main()
