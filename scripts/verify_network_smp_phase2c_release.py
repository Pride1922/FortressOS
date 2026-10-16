#!/usr/bin/env python3
"""Build an isolated ordinary image and retain its absence/state evidence."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tarfile

ROOT=Path(__file__).resolve().parents[1]
def main():
    out=ROOT/'build/network-smp-phase2c/release-current-verification.json'
    workspace=Path(subprocess.check_output(['python3','scripts/create_ext4_guest_workspace.py'],cwd=ROOT,text=True).strip())
    record={'result':'FAIL','workspace':str(workspace),'build_argv':['make','-j4','bin/fortress.elf','bin/initramfs.tar']}
    try:
        with out.with_suffix('.build.log').open('w') as log:
            subprocess.run(record['build_argv'],cwd=workspace,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=600)
        record['sha256']={n:hashlib.sha256((workspace/n).read_bytes()).hexdigest() for n in ('bin/fortress.elf','bin/initramfs.tar')}
        symbols=subprocess.check_output(['nm','-a',str(workspace/'bin/fortress.elf')],text=True)
        strings=subprocess.check_output(['strings',str(workspace/'bin/fortress.elf')],text=True)
        assert not re.search(r'net_tcp_fixture_|net_control_fixture_|handoff_fixture_|NET SMP[012]',symbols+strings)
        with tarfile.open(workspace/'bin/initramfs.tar') as archive:
            assert not any('net-cpu-probe' in n for n in archive.getnames())
        record['private_probes_absent']=True
        record['size_outputs']={}; record['module_state']={}
        for module in ('net/net_udp_owner.o','net/net_control.o','kernel/handoff.o'):
            raw=subprocess.check_output(['size','-A',str(workspace/'build'/module)],text=True)
            record['size_outputs'][module]=raw
            record['module_state'][module]=sum(int(size) for section,size in re.findall(r'^(\.\S+)\s+(\d+)\s+\d+\s*$',raw,re.M) if section.startswith(('.bss','.data')))
        record['registration_allowance']=64*32
        record['alignment_metadata_allowance']=4096
        record['total_state_bound']=sum(record['module_state'].values())+record['registration_allowance']+record['alignment_metadata_allowance']
        assert record['total_state_bound']<576*1024
        sources=json.loads((workspace/'workspace-manifest.json').read_text())['sources']
        record['source_changes']=[n for n,sha in sources.items() if hashlib.sha256((ROOT/n).read_bytes()).hexdigest()!=sha]
        assert not any(n.startswith(('src/','user/')) or n in ('Makefile','linker.ld','limine.conf') for n in record['source_changes'])
        record['result']='PASS'
    finally:
        out.write_text(json.dumps(record,indent=2)+'\n')
    print(out,flush=True)
if __name__=='__main__': main()
