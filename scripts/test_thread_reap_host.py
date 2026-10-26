#!/usr/bin/env python3
"""Actual final-reaper and join-record cleanup, with host lifetime adapters."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parent.parent
source=(ROOT/'src/kernel/thread.c').read_text()
def function(signature):
    begin=source.index(signature); brace=source.index('{',begin); level=1; end=brace+1
    while level:
        level+=(source[end]=='{')-(source[end]=='}'); end+=1
    return source[begin:end]
out=ROOT/'build/thread-reap-host';out.mkdir(parents=True,exist_ok=True)
join=source[source.index('#define MAX_THREAD_JOIN_RECORDS'):source.index('#define MAX_FUTEX_WAITERS')]
(out/'reap-functions.inc').write_text(join+'\n'+function('void sched_reap_dead(void)'))
subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-O1','-g','-pthread',
    '-fsanitize=address,undefined','-no-pie','-I'+str(out),str(ROOT/'tests/thread_reap_host.c'),
    '-o',str(out/'test')],check=True)
subprocess.run([str(out/'test')],check=True,timeout=60)
