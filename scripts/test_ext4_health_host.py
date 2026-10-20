"""EH-3 actual mounted allocator/health; only temporary regular-file disks."""
from pathlib import Path
import hashlib,json,subprocess,tempfile
from test_ext4_time_host import fixture
from create_ext4_fixtures import ROOT,run

def main():
    evidence=ROOT/'build/ext4-health-recovery';evidence.mkdir(parents=True,exist_ok=True);results=[]
    with tempfile.TemporaryDirectory(prefix='ext4-health-') as tmp:
        folder=Path(tmp)
        for bs in (1024,2048,4096):
            base=fixture(folder,bs,inodes=256);digest=hashlib.sha256(base.read_bytes()).hexdigest()
            for ss in (512,4096):
                prefix=folder/f'result-{bs}-{ss}'
                with (evidence/f'{bs}-{ss}.log').open('w') as log:
                    result=subprocess.run([str(ROOT/'build/ext4_health_host'),str(base),str(ss),str(prefix)],stdout=log,stderr=subprocess.STDOUT,timeout=600)
                assert result.returncode==0,(evidence/f'{bs}-{ss}.log').read_text()
                audit=''
                for suffix in ('logs-short','pinned','close-retry','flush-recovery','block-floor','inode-floor','replace-health'):
                    image=Path(str(prefix)+f'-{suffix}.img');audit+=run(['e2fsck','-fn',str(image)])
                    if suffix=='logs-short':
                        payload=folder/'payload';audit+=run(['debugfs','-R',f'dump /short {payload}',str(image)])
                        assert payload.read_bytes()==b'S'*65536;payload.unlink()
                        audit+=run(['debugfs','-R',f'dump /log {payload}',str(image)])
                        data=payload.read_bytes();assert len(data)==4096
                        records=[data[i:i+32] for i in range(0,len(data),32)]
                        assert all(records.count(bytes([c])*32)==32 for c in b'ABCD');payload.unlink()
                (evidence/f'{bs}-{ss}-linux.log').write_text(audit)
                assert hashlib.sha256(base.read_bytes()).hexdigest()==digest
                results.append({'block_size':bs,'sector_size':ss,'result':'PASS','linux_audits':7})
                print(results[-1],flush=True)
    paths=['tests/ext4_health_host.c','scripts/test_ext4_health_host.py','src/fs/ext4.c','src/fs/ext4_mutate.inc','src/fs/ext4_accounts.inc','src/fs/ext4_mount_journal.inc','src/include/syscall_abi.h']
    (evidence/'manifest.json').write_text(json.dumps({'results':results,'sources':{p:hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in paths},'binary_sha256':hashlib.sha256((ROOT/'build/ext4_health_host').read_bytes()).hexdigest(),'scope':'actual mounted host adapters; trusted-root policy selected in host; no physical or sector-tear evidence'},indent=2)+'\n')
if __name__=='__main__':main()
