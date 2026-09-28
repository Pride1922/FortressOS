from pathlib import Path
import os, subprocess, tempfile, sys
assert sys.argv[1:] in ([], ['--signals'], ['--stops'], ['--groups'], ['--orphans'])
stops = sys.argv[1:] == ['--stops']
groups = sys.argv[1:] == ['--groups']
source = 'tests/process_group_host.c' if groups else ('tests/s8_stop_host.c' if stops else ('tests/signal_host.c' if sys.argv[1:] == ['--signals'] else 'tests/process_table_host.c'))
if sys.argv[1:] == ['--orphans']: source = 'tests/s8_orphan_host.c'
repo=Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix='fortress-process-') as tmp:
    exe=Path(tmp)/'test'
    subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-g','-pthread',
        '-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie',
        '-DTEST_SMP_MEMORY','-Isrc/include','-Isrc/kernel',source,
        *([] if stops or groups else ['src/kernel/process_table.c']),'-o',str(exe)],cwd=repo,check=True,timeout=60)
    subprocess.run([str(exe)],check=True,timeout=60,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})
