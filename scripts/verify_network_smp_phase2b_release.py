#!/usr/bin/env python3
"""Fresh strict kernel/initramfs only; never rebuild shared release media."""
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile

ROOT=Path(__file__).resolve().parents[1]

def main():
    out=ROOT/'build/network-smp-phase2b'
    out.mkdir(parents=True,exist_ok=True)
    report={'result':'FAIL'}
    digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
    try:
        workspace=Path(subprocess.check_output(['python3','scripts/create_ext4_guest_workspace.py'],cwd=ROOT,text=True).strip())
        report['workspace']=str(workspace)
        argv=['make','-j4','bin/fortress.elf','bin/initramfs.tar']
        report['build_argv']=argv
        with (out/'release-build.log').open('w') as log:
            subprocess.run(argv,cwd=workspace,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=600)
        symbols=subprocess.check_output(['nm','-S',str(workspace/'bin/fortress.elf')],text=True)
        (out/'release-symbols.log').write_text(symbols)
        for marker in ('net_control_fixture','net_udp_fixture','handoff_fixture','handoff_test_exhaust'):
            assert marker not in symbols,marker
        image=(workspace/'bin/fortress.elf').read_bytes()
        for marker in (b'[NET SMP0',b'[NET SMP2A',b'[NET SMP2B',b'HANDOFF CALL FAIL'):
            assert marker not in image,marker
        assert '0x7ff2' not in (workspace/'src/kernel/syscall.c').read_text()
        with tarfile.open(workspace/'bin/initramfs.tar') as archive:
            names=archive.getnames()
            assert not any('net-cpu-probe' in n or 'handoff-probe' in n for n in names)
        report.update(result='PASS',fixture_absence='PASS',build_sha256={
            n:digest(workspace/n) for n in ('bin/fortress.elf','bin/initramfs.tar')},
            sources=digest(workspace/'workspace-manifest.json'))
    except Exception as error:
        report['failure']=repr(error)
        raise
    finally:
        (out/'release-verification.json').write_text(json.dumps(report,indent=2)+'\n')
        print(json.dumps(report),flush=True)

if __name__=='__main__': main()
