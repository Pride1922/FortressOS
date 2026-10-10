from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix='fortress-perm-power-') as tmp:
    exe=Path(tmp)/'power'
    subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-g',
      '-DTEST_PERMISSIONS_VALUES','-fsanitize=address,undefined','-fno-omit-frame-pointer',
      '-no-pie','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
      '-Itests/pipe_host','-Itests/host','-Isrc/include','-Isrc/drivers','-Isrc/mm',
      '-Isrc/fs','-Isrc/kernel','-Isrc/arch/x86_64',
      'tests/perm_power_wiring_host.c','src/kernel/creds.c','-o',str(exe)],cwd=root,check=True)
    subprocess.run([str(exe)],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})
