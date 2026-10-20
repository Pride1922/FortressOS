"""EH-5 measurement; actual mounted code, disposable disks, Linux audits."""
from pathlib import Path
import hashlib,json,subprocess,tempfile
from test_ext4_time_host import fixture
from create_ext4_fixtures import ROOT,run

def main():
    out=ROOT/'build/ext4-latency-host';out.mkdir(parents=True,exist_ok=True)
    paths=list((ROOT/'src/fs').iterdir())+[ROOT/p for p in (
        'tests/ext4_latency_host.c','tests/ext4_mount_host.c','tests/ext4_host/spinlock.h',
        'tests/ext4_fault_disk.c','tests/ext4_fault_disk.h','tests/ext4_time_clock.c',
        'src/include/syscall_abi.h','src/kernel/creds.c','scripts/test_ext4_latency_host.py')]
    sources={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths if p.is_file()}
    cmd=['gcc','-std=c11','-O1','-g','-fsanitize=address,undefined','-Wall','-Wextra','-Werror','-no-pie','-pthread','-ffunction-sections','-fdata-sections','-Wl,--gc-sections']
    cmd+=['-I'+p for p in ('tests/ext4_host','tests/host','src/include','src/fs','src/drivers','src/mm')]
    cmd+=['tests/ext4_latency_host.c','tests/ext4_time_clock.c','tests/ext4_fault_disk.c','src/fs/permission_values.c','src/kernel/creds.c','-o',str(out/'probe')]
    run(cmd);results=[]
    with tempfile.TemporaryDirectory(prefix='ext4-latency-') as tmp:
        folder=Path(tmp);base=fixture(folder,4096);digest=hashlib.sha256(base.read_bytes()).hexdigest()
        for sector in (512,4096):
            prefix=folder/str(sector)
            p=subprocess.run([str(out/'probe'),str(base),str(sector),str(prefix)],capture_output=True,text=True,timeout=600)
            (out/f'{sector}.log').write_text(p.stdout+p.stderr);assert p.returncode==0,p.stdout+p.stderr
            metrics=[json.loads(line) for line in p.stdout.splitlines() if line.startswith('{')]
            audit=''
            for suffix in ('clean','recovered'):
                image=Path(str(prefix)+f'-{suffix}.img');audit+=run(['e2fsck','-fn',str(image)])
                payload=folder/'payload';run(['debugfs','-R',f'dump /staging {payload}',str(image)])
                assert payload.read_bytes()==b'L'*10485760;payload.unlink()
                run(['debugfs','-R',f'dump /log {payload}',str(image)])
                assert payload.read_bytes()==b'LOG\n';payload.unlink()
            (out/f'{sector}-linux.log').write_text(audit)
            assert hashlib.sha256(base.read_bytes()).hexdigest()==digest
            results.append({'sector':sector,'metrics':metrics,'linux_audits':2});print(json.dumps(results[-1]),flush=True)
    assert all(hashlib.sha256((ROOT/p).read_bytes()).hexdigest()==h for p,h in sources.items())
    (out/'manifest.json').write_text(json.dumps({'results':results,'sources':sources,'command':cmd,
        'compiler':run(['gcc','--version']),'host':run(['uname','-a']),
        'binary_sha256':hashlib.sha256((out/'probe').read_bytes()).hexdigest(),'scope':'host pthread model; sanitizer/memory-copy overhead; no IRQ, scheduler, physical or transport timing claim'},indent=2)+'\n')
if __name__=='__main__':main()
