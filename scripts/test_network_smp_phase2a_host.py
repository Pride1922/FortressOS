#!/usr/bin/env python3
"""Retain exact actual-code sanitizer commands/results and source identities."""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
def main():
    parent=ROOT/'build/network-smp-phase2a'; parent.mkdir(parents=True,exist_ok=True)
    out=Path(tempfile.mkdtemp(prefix='host-',dir=parent)); print(out,flush=True)
    sources=['src/kernel/handoff.c','src/kernel/handoff.h','tests/net_control_host.c','tests/net_lock_host.c','src/include/spinlock.h','src/kernel/thread.h','src/net/net_control.c','src/net/net_control.h','src/net/net.c','src/net/net_ping.c','src/net/net_ipv4.c','tests/net_ifconfig_host.c','user/ifup.c','user/ifconfig.c','user/netconf.c','user/netconf.h','src/include/ping_abi.h','src/include/trace_abi.h','src/include/netctl_abi.h','src/include/syscall_abi.h','src/net/net.h','src/net/net_ping.h','src/net/net_ipv4.h','scripts/test_network_smp_phase2a_host.py']
    digest=lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    manifest={'result':'FAIL','sources':{n:digest(ROOT/n) for n in sources},'runs':[]}
    try:
        for sanitizer in ('address,undefined','thread'):
            name='asan-ubsan' if sanitizer.startswith('address') else 'tsan'
            exe=out/name
            argv=['gcc','-std=c11','-O1','-g','-DTEST_SMP_MEMORY','-pthread','-fsanitize='+sanitizer,'-no-pie','-Wall','-Wextra','-Werror','-Isrc/include','-Isrc/kernel','-Isrc/drivers','-Isrc/arch/x86_64','-Isrc/mm','-Isrc/net','tests/net_control_host.c','tests/net_lock_host.c','src/kernel/handoff.c','src/net/net_control.c','src/net/net.c','src/net/net_ping.c','src/net/net_ipv4.c','src/net/udp.c','src/net/icmp.c','src/net/checksum.c','src/net/ipv4.c','src/net/eth.c','src/net/arp.c','-o',str(exe)]
            run_argv=['setarch','x86_64','-R',str(exe)] if sanitizer=='thread' else [str(exe)]
            record={'build_argv':argv,'run_argv':run_argv}; manifest['runs'].append(record)
            with (out/(name+'.build.log')).open('w') as log:
                subprocess.run(argv,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=180)
            record['binary_sha256']=digest(exe)
            with (out/(name+'.log')).open('w') as log:
                subprocess.run(run_argv,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=180)
            record['result']='PASS'; print((out/(name+'.log')).read_text(),flush=True)
        assert all(digest(ROOT/n)==sha for n,sha in manifest['sources'].items())
        manifest['result']='PASS'
    except Exception as error:
        manifest['failure']=repr(error); raise
    finally: (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
if __name__=='__main__': main()
