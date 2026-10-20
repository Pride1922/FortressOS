"""Release-excluded synchronous batch prototype; regular-file disks only."""
from pathlib import Path
import hashlib,json,subprocess,tempfile
from test_ext4_time_host import fixture
from create_ext4_fixtures import ROOT,run
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    out=ROOT/'build/ext4-stage-host';out.mkdir(parents=True,exist_ok=True)
    paths=[p for p in (ROOT/'src/fs').iterdir() if p.is_file()]+[ROOT/p for p in ('tests/ext4_stage_host.c','tests/ext4_mount_host.c','tests/ext4_fault_disk.c','tests/ext4_fault_disk.h','tests/ext4_host/spinlock.h','tests/ext4_time_clock.c','scripts/test_ext4_stage_host.py')]
    before={str(p.relative_to(ROOT)):sha(p) for p in paths}
    cmd=['gcc','-std=c11','-O1','-g','-fsanitize=address,undefined','-Wall','-Wextra','-Werror','-no-pie','-pthread','-ffunction-sections','-fdata-sections','-Wl,--gc-sections']
    cmd+=['-I'+p for p in ('tests/ext4_host','tests/host','src/include','src/fs','src/drivers','src/mm')]
    cmd+=['tests/ext4_stage_host.c','tests/ext4_time_clock.c','tests/ext4_fault_disk.c','src/fs/permission_values.c','src/kernel/creds.c','-o',str(out/'probe')];run(cmd)
    results=[]
    with tempfile.TemporaryDirectory(prefix='ext4-stage-') as tmp:
        folder=Path(tmp)
        for bs in (1024,2048,4096):
            base=fixture(folder,bs);digest=sha(base)
            for ss in (512,4096):
                prefix=folder/f'{bs}-{ss}'
                with (out/f'{bs}-{ss}.log').open('w') as log:
                    p=subprocess.run([str(out/'probe'),str(base),str(ss),str(prefix)],stdout=log,stderr=subprocess.STDOUT,timeout=1200)
                assert p.returncode==0,(out/f'{bs}-{ss}.log').read_text()
                audit=''
                for suffix in ('large','recovered','floor'):
                    image=Path(str(prefix)+f'-{suffix}.img');audit+=run(['e2fsck','-fn',str(image)])
                    if suffix=='floor':continue
                    payload=folder/'payload';path='/stage' if suffix=='large' else '/cut'
                    audit+=run(['debugfs','-R',f'dump {path} {payload}',str(image)])
                    data=payload.read_bytes();payload.unlink()
                    assert len(data)==10485760+32*bs if suffix=='large' else len(data) in (1,32*bs)
                    assert data==b'B'*len(data)
                (out/f'{bs}-{ss}-linux.log').write_text(audit);assert sha(base)==digest
                results.append({'block':bs,'sector':ss,'result':'PASS','linux_audits':3});print(results[-1],flush=True)
    assert all(sha(ROOT/p)==h for p,h in before.items())
    (out/'manifest.json').write_text(json.dumps({'results':results,'sources':before,'binary_sha256':sha(out/'probe'),'command':cmd,'scope':'actual mounted host prototype; four persistence models, wrapped journal and all callback cuts; no production ABI or responsiveness acceptance'},indent=2)+'\n')
if __name__=='__main__':main()
