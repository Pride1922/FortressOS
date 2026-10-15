"""Installed /var policy through real guest credentials; regular-file copies only."""
import argparse
import json
from pathlib import Path
import tempfile
import test_root_phase2 as gate


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--disk',type=Path,required=True)
    parser.add_argument('--username',default='fabio');args=parser.parse_args()
    source=args.disk.resolve();assert source.is_file() and source.is_relative_to(gate.fixture.REPO/'build/root-phase4')
    out=Path(tempfile.mkdtemp(prefix='var-guest-',dir=gate.fixture.REPO/'build/root-phase4'))
    manifest={'result':'FAIL','scope':'BIOS/UEFI installed /var permissions; disposable regular files','cases':[]}
    original=gate.fixture.digest(source)
    def check(boot,password,send,wait,command,prompt,transcript):
        for cmd in ('ls /var','cat /var/log/fortress-install.log',
                    'echo denied > /var/log/fortress-install.log',
                    'echo denied > /var/new-file','rm /var/log/fortress-install.log'):
            start=len(transcript);send(cmd)
            wait(lambda t:'permission denied' in t.lower() and prompt(t),start)
        start=len(transcript);send('sudo cat /var/log/fortress-install.log')
        wait(lambda t:t.endswith('sudo password: '),start);send(password)
        wait(lambda t:'result=installation complete' in t and prompt(t),start)
        command('cat /etc/hostname','dell-test')
        return password
    gate.SESSION_HOOK=check
    try:
        for firmware in ('bios','uefi'):
            disk=out/(firmware+'.img')
            gate.fixture.run(['cp','--sparse=always',str(source),str(disk)],out/'copy.log')
            for boot in (0,1,2):
                gate.guest(out,disk,firmware,1,boot,'a changed account password',
                    'unused-recovery-secret',manifest,accel='kvm',username=args.username)
        assert gate.fixture.digest(source)==original
        manifest['result']='PASS';print(out)
    finally:
        gate.SESSION_HOOK=None
        (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')


if __name__=='__main__':main()
