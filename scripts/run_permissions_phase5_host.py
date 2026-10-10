"""Final host gates with before/after exact source and binary provenance."""
from pathlib import Path
import hashlib,json,os,subprocess
root=Path(__file__).resolve().parent.parent;out=root/'build/permissions-phase5'
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def inputs():
    names=subprocess.check_output(['git','ls-files','--cached','--others','--exclude-standard','-z'],cwd=root).decode().split('\0')
    return {name:sha(root/name) for name in names if name and (root/name).is_file() and not name.startswith('docs/')}
before=inputs();commands=[['make','test-host','test-perm-phase5-host','test-perm-db-host','test-perm-spawn-host',
 'test-perm-creds-host','test-perm-syscalls-host','test-perm-runfs-host','test-perm-privileges-host',
 'test-perm-matrix-host','test-perm-fs-host','test-net-ifconfig-host','test-usb-mount-host','test-xhci-bot-host'],
 ['python3','scripts/test_runner_host.py'],['python3','scripts/audit_permissions_wiring.py'],['python3','scripts/audit_permissions_phase5.py']]
results=[]
try:
    with (out/'final-host-verified.log').open('w') as log:
        for cmd in commands:
            log.write(json.dumps(cmd)+'\n');log.flush()
            result=subprocess.run(cmd,cwd=root,stdout=log,stderr=subprocess.STDOUT,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})
            results.append({'argv':cmd,'returncode':result.returncode});assert result.returncode==0,cmd
    assert before==inputs(),'source inputs changed during host campaign'
finally:
    binaries=[root/'build/permissions-phase5/tar-fuzz',root/'build/permissions-phase5/syscall-fuzz',
              root/'build/permissions-phase3/userdb-host',root/'build/permissions-phase3/login-host',
              root/'build/permissions-phase4/spawn-host',root/'build/permissions-phase4/sudo-host']
    (out/'host-manifest.json').write_text(json.dumps({'base_commit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),
        'sources_before':before,'sources_unchanged':before==inputs(),'commands':results,
        'binaries':{str(q.relative_to(root)):sha(q) for q in binaries if q.is_file()},
        'log_sha256':sha(out/'final-host-verified.log'),
        'environment':{'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'},
        'scope':'actual finite code plus explicit host adapters; no IRQ/scheduler/physical claim'},indent=2)+'\n')
print('PASS final host campaign with unchanged source hashes; evidence',out)
