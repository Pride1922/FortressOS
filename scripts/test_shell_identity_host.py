"""Actual prompt rendering with syscall adapters, ASan/UBSan."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];parent=root/'build/root-phase4';parent.mkdir(parents=True,exist_ok=True)
out=Path(tempfile.mkdtemp(prefix='prompt-',dir=parent))
command=['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-no-pie',
    '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-Isrc/include','-Isrc/fs','-Iuser',
    'tests/shell_prompt_host.c','user/tools/userdb.c','user/tools/digest.c','-o',str(out/'host')]
with (out/'host.log').open('w') as log:
    subprocess.run(command,cwd=root,stdout=log,stderr=subprocess.STDOUT,check=True)
    subprocess.run([str(out/'host')],cwd=root,stdout=log,stderr=subprocess.STDOUT,check=True)
print((out/'host.log').read_text().strip());print(out)
