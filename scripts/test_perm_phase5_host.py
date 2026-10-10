from pathlib import Path
import os,subprocess
root=Path(__file__).resolve().parent.parent
out=root/'build/permissions-phase5';out.mkdir(parents=True,exist_ok=True)
common=['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
        '-no-pie','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
        '-Itests/pipe_host','-Itests/host','-Isrc/include','-Isrc/fs','-Isrc/drivers',
        '-Isrc/mm','-Isrc/kernel','-Isrc/arch/x86_64']
for name,sources in [('tar-fuzz',['tests/perm_tar_fuzz_host.c']),
                     ('syscall-fuzz',['tests/perm_phase5_host.c','src/kernel/creds.c','src/fs/permission_values.c'])]:
    exe=out/name
    subprocess.run(common+sources+['-o',str(exe)],cwd=root,check=True)
    subprocess.run([str(exe)],cwd=root,check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})
