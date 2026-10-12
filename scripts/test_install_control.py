#!/usr/bin/env python3
"""Actual control API and verbatim production descriptor functions, host adapters."""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]


def main():
    parent=ROOT/'build/root-phase4'
    parent.mkdir(parents=True,exist_ok=True)
    out=Path(tempfile.mkdtemp(prefix='leases-',dir=parent))
    sources={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()
             for base in ('src','tests','scripts','user') for p in (ROOT/base).rglob('*')
             if p.is_file() and p.suffix in ('.c','.h','.inc','.py','.asm')}
    manifest={'scope':'actual installer control/descriptor host adapters','sources':sources,'commands':[],'result':'FAIL'}
    try:
        thread=(ROOT/'src/kernel/thread.c').read_text()
        syscall=(ROOT/'src/kernel/syscall.c').read_text()
        segments={'install_fds.inc':thread[thread.index('int fd_init_std('):thread.index('void process_exit(')],
                  'install_fcntl.inc':syscall[syscall.index('static int64_t sys_fcntl('):syscall.index('static int64_t sys_read(')],
                  'install_errors.inc':syscall[syscall.index('int64_t syscall_from_vfs_error('):syscall.index('static int64_t sys_write(')]}
        for name,text in segments.items():(out/name).write_text(text)
        command=['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-no-pie',
                 '-Isrc/include','-Isrc/fs','-Isrc/kernel','-Isrc/mm','-Isrc/drivers','-Isrc/arch/x86_64',
                 '-I'+str(out),'tests/install_control_host.c','user/tools/digest.c','-o',str(out/'host')]
        with (out/'host.log').open('w') as log:
            for cmd in (command,[str(out/'host')]):
                manifest['commands'].append(cmd)
                subprocess.run(cmd,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=120)
            nvme_command=['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-no-pie',
                '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-Isrc/include','-Isrc/fs','-Isrc/kernel','-Isrc/drivers','-Isrc/lib','-Isrc/mm','-Isrc/arch/x86_64',
                'tests/install_nvme_host.c','src/kernel/root_boot.c','src/fs/gpt.c','user/tools/digest.c','-o',str(out/'nvme-host')]
            for cmd in (nvme_command,[str(out/'nvme-host')]):
                manifest['commands'].append(cmd)
                subprocess.run(cmd,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=120)
            inspect_command=nvme_command+['-DTEST_INSTALLER_INSPECT']
            for cmd in (inspect_command,[str(out/'nvme-host')]):
                manifest['commands'].append(cmd)
                subprocess.run(cmd,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=120)
            physical_command=nvme_command+['-DINSTALLER_PHYSICAL']
            for cmd in (physical_command,[str(out/'nvme-host')]):
                manifest['commands'].append(cmd)
                subprocess.run(cmd,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=120)
        assert all(hashlib.sha256((ROOT/p).read_bytes()).hexdigest()==h for p,h in sources.items())
        manifest['result']='PASS';print((out/'host.log').read_text().strip());print(out)
    finally:(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')


if __name__=='__main__':main()
