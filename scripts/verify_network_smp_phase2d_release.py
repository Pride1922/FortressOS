#!/usr/bin/env python3
"""Strict ordinary snapshot build, fixture absence and realized static ledger."""
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile

ROOT=Path(__file__).resolve().parents[1]
def digest(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    parent=ROOT/'build/network-smp-phase2d'; parent.mkdir(parents=True,exist_ok=True)
    out=Path(tempfile.mkdtemp(prefix='release-',dir=parent)); print(out,flush=True)
    result={'result':'FAIL'}
    media={str(p.relative_to(ROOT)):digest(p) for p in (ROOT/'bin').glob('fortress.*') if p.suffix in ('.iso','.img')}
    try:
        workspace=Path(subprocess.check_output(['python3','scripts/create_ext4_guest_workspace.py'],cwd=ROOT,text=True).strip())
        result['workspace']=str(workspace)
        result['sources']=json.loads((workspace/'workspace-manifest.json').read_text())
        argv=['make','-j4','bin/fortress.elf','bin/initramfs.tar']; result['build_argv']=argv
        with (out/'build.log').open('w') as log:
            subprocess.run(argv,cwd=workspace,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=600)
        elf=workspace/'bin/fortress.elf'; archive=workspace/'bin/initramfs.tar'
        result['sha256']={str(p.relative_to(workspace)):digest(p) for p in (elf,archive)}
        symbols=subprocess.check_output(['nm',str(elf)],text=True)
        strings=subprocess.check_output(['strings',str(elf)],text=True)
        for private in ('fixture_accept','net_tcp_fixture','net_socket_fixture','net-cpu-probe','NET SMP2D','NET SMP2C','NET SMP0 ENTRY'):
            assert private not in symbols and private not in strings,private
        with tarfile.open(archive) as tar:
            assert not any('net-cpu-probe' in m.name for m in tar)
        ledger={}
        for name in ('net_udp_owner','net_control','handoff'):
            sections=subprocess.check_output(['size','-A',str(workspace/f'build/{"kernel" if name=="handoff" else "net"}/{name}.o')],text=True)
            # Every writable static section, including compiler-split .data.*.
            ledger[name]=sum(int(row.split()[1]) for row in sections.splitlines() if row.startswith(('.bss','.data')))
            (out/(name+'.sections')).write_text(sections)
        measurement=subprocess.check_output(['gdb','-batch',str(elf),'-ex','p sizeof(udp_request_t)', '-ex',"p sizeof('net_udp_owner.c'::values)"],text=True,stderr=subprocess.STDOUT)
        (out/'record-size.log').write_text(measurement)
        ledger.update(registrations=2048,alignment_metadata=4096)
        result['static_ledger_bytes']=ledger
        result['static_total_bytes']=sum(ledger.values())
        assert result['static_total_bytes']<589824
        for name,sha in result['sources']['sources'].items():
            assert digest(ROOT/name)==sha,name
        assert all(digest(ROOT/name)==sha for name,sha in media.items())
        result['shared_release_media_unchanged']=media
        result['private_probes_absent']=True; result['result']='PASS'
    except Exception as error:
        result['failure']=repr(error); raise
    finally: (out/'manifest.json').write_text(json.dumps(result,indent=2)+'\n')
if __name__=='__main__': main()
