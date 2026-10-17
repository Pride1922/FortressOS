#!/usr/bin/env python3
"""Installed Ring 3 provision/reload/reboot gates. Disposable regular NVMe only.
No generated private key or secret-bearing disk is retained in evidence.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import shutil
import subprocess
import tempfile
import test_root_phase0 as fixture
import test_root_phase2 as guest_gate

ROOT=fixture.REPO
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--quick',action='store_true');parser.add_argument('--runtime',type=Path);parser.add_argument('--smp',default='1,4,8');args=parser.parse_args()
    cpus=[1] if args.quick else [int(x) for x in args.smp.split(',')]
    assert cpus and len(set(cpus))==len(cpus) and all(x in (1,4,8) for x in cpus)
    build=ROOT/'build/ssh-hostkeys-qemu';build.mkdir(parents=True,exist_ok=True)
    out=Path(tempfile.mkdtemp(prefix='gate-',dir=build))
    manifest={'status':'FAIL','scope':'disposable installed-root Ring 3; no physical acceptance','cases':[]}
    preserved={str(p):fixture.digest(p) for p in [ROOT/'bin/fortress.elf',ROOT/'bin/initramfs.tar',ROOT/'bin/fortress.iso',ROOT/'bin/fortress.img']}
    run=lambda cmd:subprocess.run(list(map(str,cmd)),cwd=ROOT,check=True,capture_output=True,text=True)
    try:
        # Produces a key-free installed target through the actual runtime, not
        # a hand-assembled approximation of installed-root readiness.
        if args.runtime:
            runtime=args.runtime.resolve()
        else:
            generated=run(['python3','scripts/test_install_runtime.py','--username','operator','--hostname','ssh-fixture'])
            runtime=Path(generated.stdout.strip().splitlines()[-1])
        assert runtime.is_relative_to(ROOT/'build/root-phase4')
        assert json.loads((runtime/'manifest.json').read_text())['result']=='PASS'
        manifest['runtime']=str(runtime.relative_to(ROOT))
        fingerprints={}
        with tempfile.TemporaryDirectory(prefix='fortress-ssh-key-qemu-') as tmp:
            native=Path(tmp)
            try:
                guest_gate.PHASE=3 # existing runner's explicit -cpu max entropy profile
                for firmware in ('bios','uefi'):
                    for smp in cpus:
                        case=native/f'{firmware}-{smp}';case.mkdir()
                        disk=case/'target.img';run(['cp','--sparse=always',runtime/'target.img',disk])
                        key=f'{firmware}-{smp}'
                        def hook(boot,password,send,wait,command,prompt,transcript):
                            command('/sbin/ssh-keygen --fingerprint','permission denied')
                            command('cat /etc/ssh/ssh_host_ed25519_key','permission denied' if boot else 'no such file')
                            def privileged(cmd,expected):
                                start=len(transcript);send('sudo '+cmd)
                                wait(lambda t:t.endswith('sudo password: '),start);send(password)
                                wait(lambda t:expected in t and prompt(t),start)
                                return transcript[start:].decode(errors='replace').replace('\r','')
                            text=privileged('/sbin/ssh-keygen --provision','SHA256:')
                            value=re.findall(r'SHA256:[A-Za-z0-9+/]{43}',text);assert len(value)==1
                            if key in fingerprints:assert fingerprints[key]==value[0]
                            else:fingerprints[key]=value[0]
                            text=privileged('/sbin/ssh-keygen --fingerprint','SHA256:')
                            assert fingerprints[key] in text
                            text=privileged('/sbin/ssh-keygen --public','ssh-ed25519 ')
                            assert re.search(r'ssh-ed25519 [A-Za-z0-9+/=]+ FortressOS host key',text)
                            return password
                        guest_gate.SESSION_HOOK=hook
                        for boot in range(3):
                            guest_gate.guest(case,disk,firmware,smp,boot,'a changed account password',
                                             'unused-test-recovery',manifest)
                        root=case/'independent-root.img';guest_gate.extract_root(disk,root)
                        run(['e2fsck','-fn',root])
                        private=case/'private-key'
                        run(['debugfs','-R',f'dump /etc/ssh/ssh_host_ed25519_key {private}',root]);private.chmod(0o600)
                        public=run(['ssh-keygen','-y','-f',private]).stdout
                        pub=case/'public-key.pub';pub.write_text(public)
                        assert fingerprints[key] in run(['ssh-keygen','-lf',pub]).stdout
                        meta=run(['debugfs','-R','stat /etc/ssh/ssh_host_ed25519_key',root]).stdout
                        assert 'Mode:  0600' in meta and 'User:     0' in meta and 'Group:     0' in meta
                        print(f'PASS {key}: provision/reload/three boots/OpenSSH/Linux audit',flush=True)
                assert len(set(fingerprints.values()))==len(fingerprints),'fresh installations reused identity'
            finally:
                # Keep diagnostics/public evidence on failure too, never private
                # disk images, dumped containers, credentials or process memory.
                for path in native.rglob('*'):
                    if path.is_file() and (path.name.endswith(('-argv.json','-serial.log','-stderr.log','-fsck.log')) or path.name=='public-key.pub'):
                        dest=out/path.relative_to(native);dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(path,dest)
        assert all(fixture.digest(Path(p))==h for p,h in preserved.items())
        manifest['fingerprints']=fingerprints;manifest['preserved']=preserved;manifest['status']='PASS'
        print(out,flush=True)
    finally:
        guest_gate.SESSION_HOOK=None
        (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
if __name__=='__main__':main()
