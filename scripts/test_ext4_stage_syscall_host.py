"""Actual private syscall helper; mocked pointer/descriptor admission only."""
from pathlib import Path
import hashlib,json,tempfile
from create_ext4_fixtures import ROOT,run
from test_ext4_time_host import fixture
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    out=ROOT/'build/ext4-stage-syscall-host';out.mkdir(parents=True,exist_ok=True)
    paths=[p for p in (ROOT/'src/fs').iterdir() if p.is_file()]+[ROOT/p for p in ('src/kernel/ext4_stage_test.inc','tests/ext4_stage_syscall_host.c','scripts/test_ext4_stage_syscall_host.py','tests/ext4_mount_host.c','tests/ext4_fault_disk.c','tests/ext4_host/spinlock.h')]
    before={str(p.relative_to(ROOT)):sha(p) for p in paths}
    cmd=['gcc','-std=c11','-O1','-g','-fsanitize=address,undefined','-Wall','-Wextra','-Werror','-no-pie','-pthread','-ffunction-sections','-fdata-sections','-Wl,--gc-sections']
    cmd+=['-I'+p for p in ('tests/ext4_host','tests/host','src/include','src/fs','src/drivers','src/mm')]
    cmd+=['tests/ext4_stage_syscall_host.c','tests/ext4_time_clock.c','tests/ext4_fault_disk.c','src/fs/permission_values.c','src/kernel/creds.c','-o',str(out/'probe')];run(cmd)
    with tempfile.TemporaryDirectory(prefix='stage-syscall-') as tmp:
        base=fixture(Path(tmp),4096);digest=sha(base);log=run([str(out/'probe'),str(base)]);assert sha(base)==digest
    (out/'test.log').write_text(log);assert all(sha(ROOT/p)==h for p,h in before.items())
    (out/'manifest.json').write_text(json.dumps({'result':'PASS','sources':before,'command':cmd,'binary_sha256':sha(out/'probe'),'scope':'actual syscall-helper body with mocked fd, pointer, credential and error translation adapters; no Ring 3 pointer proof'},indent=2)+'\n');print(log)
if __name__=='__main__':main()
