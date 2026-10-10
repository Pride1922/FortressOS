from pathlib import Path
import subprocess
root=Path(__file__).resolve().parent.parent
out=root/'build/permissions-phase4';out.mkdir(parents=True,exist_ok=True)
source=(root/'src/kernel/thread.c').read_text()
function=source[source.index('int process_setup_user_stack('):source.index('static bool permissions_spawn_test;')]
(out/'stack_function.inc').write_text(function)
flags=['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-no-pie',
       '-Isrc/include','-Isrc/kernel','-Isrc/fs','-Isrc/mm','-Isrc/arch/x86_64','-I'+str(out)]
subprocess.run(flags+['tests/perm_spawn_host.c','-o',str(out/'spawn-host')],cwd=root,check=True)
subprocess.run([str(out/'spawn-host')],cwd=root,check=True)
subprocess.run(flags+['-DTOOL_HOST_TEST','tests/sudo_host.c','user/tools/common.c','user/tools/userdb.c',
                     'user/tools/digest.c','-o',str(out/'sudo-host')],cwd=root,check=True)
subprocess.run([str(out/'sudo-host')],cwd=root,check=True)
