#!/usr/bin/env python3
"""Retain exact actual-code sanitizer commands/results and source identities."""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
def main():
    parent=ROOT/'build/network-smp-phase1'; parent.mkdir(parents=True,exist_ok=True)
    out=Path(tempfile.mkdtemp(prefix='host-',dir=parent)); print(out,flush=True)
    sources=['src/kernel/handoff.c','src/kernel/handoff.h','tests/handoff_host.c','tests/net_lock_host.c','src/include/spinlock.h','src/kernel/thread.h']
    digest=lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    manifest={'result':'FAIL','sources':{n:digest(ROOT/n) for n in sources},'runs':[]}
    try:
        for sanitizer in ('address,undefined','thread'):
            name='asan-ubsan' if sanitizer.startswith('address') else 'tsan'
            exe=out/name
            argv=['gcc','-std=c11','-O1','-g','-DTEST_SMP_MEMORY','-pthread','-fsanitize='+sanitizer,'-no-pie','-Wall','-Wextra','-Werror','-Isrc/include','-Isrc/kernel','-Isrc/drivers','-Isrc/arch/x86_64','-Isrc/mm','tests/handoff_host.c','tests/net_lock_host.c','src/kernel/handoff.c','-o',str(exe)]
            run_argv=['setarch','x86_64','-R',str(exe)] if sanitizer=='thread' else [str(exe)]
            record={'build_argv':argv,'run_argv':run_argv}; manifest['runs'].append(record)
            with (out/(name+'.build.log')).open('w') as log:
                subprocess.run(argv,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=90)
            record['binary_sha256']=digest(exe)
            with (out/(name+'.log')).open('w') as log:
                subprocess.run(run_argv,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=90)
            record['result']='PASS'; print((out/(name+'.log')).read_text(),flush=True)
        assert all(digest(ROOT/n)==sha for n,sha in manifest['sources'].items())
        manifest['result']='PASS'
    except Exception as error:
        manifest['failure']=repr(error); raise
    finally: (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
if __name__=='__main__': main()
