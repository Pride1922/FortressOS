"""Private disk-free kernel signal/exit/reaper binding validation, BIOS/UEFI."""
import datetime
import hashlib
import io
import json
import pathlib
import re
import shutil
import socket
import subprocess
import tarfile
import tempfile
import threading
import time
from pkg_call_overlay import tcb_overlay
from test_pkg_service_integration import function
from test_getrandom import CODE,VARS,PROMPT

ROOT=pathlib.Path(__file__).resolve().parent.parent
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()


def case(out,work,iso,fw,smp):
    dest=out/f'{fw}-{smp}';dest.mkdir()
    with tempfile.TemporaryDirectory(prefix='fortress-call-') as tmp:
        sock=pathlib.Path(tmp)/'uart'
        cmd=['qemu-system-x86_64','-M','q35','-accel','tcg','-cpu','max','-m','512M','-smp',str(smp),'-display','none','-net','none','-no-reboot',
             '-boot','d','-cdrom',str(iso),'-chardev',f'socket,id=uart,path={sock},server=on,wait=off,logfile={dest/"serial.log"}','-serial','chardev:uart']
        if fw=='uefi':
            shutil.copyfile(VARS,dest/'vars.fd');cmd+=['-drive',f'if=pflash,format=raw,unit=0,readonly=on,file={CODE}','-drive',f'if=pflash,format=raw,unit=1,file={dest/"vars.fd"}']
        assert not any(c in cmd for c in ('-hda','-hdb','-blockdev','-readconfig'))
        for i,c in enumerate(cmd):
            if c=='-drive':assert cmd[i+1].startswith('if=pflash,format=raw,unit=')
        (dest/'argv.json').write_text(json.dumps(cmd,indent=2));stop=threading.Event();uart=socket.socket(socket.AF_UNIX);drainer=None
        with (dest/'stderr.log').open('w') as err:
            p=subprocess.Popen(cmd,cwd=work,stderr=err)
            def text():
                f=dest/'serial.log';s=f.read_text(errors='replace') if f.exists() else ''
                return re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]','',s).replace('\r','')
            def wait(test,seconds=120):
                end=time.monotonic()+seconds
                while time.monotonic()<end:
                    s=text()
                    if test(s):return s
                    assert p.poll() is None,('QEMU exit',p.returncode,s[-2500:]);time.sleep(.05)
                raise AssertionError('timeout '+text()[-3500:])
            try:
                end=time.monotonic()+10
                while True:
                    try:uart.connect(str(sock));break
                    except (FileNotFoundError,ConnectionRefusedError):assert time.monotonic()<end;time.sleep(.05)
                uart.settimeout(.2)
                def drain():
                    while not stop.is_set():
                        try:
                            if not uart.recv(65536):return
                        except socket.timeout:pass
                        except OSError:return
                drainer=threading.Thread(target=drain,daemon=True);drainer.start()
                wait(lambda s:'FortressOS login: ' in s or PROMPT.search(s))
                if not PROMPT.search(text()):uart.sendall(b'operator\n')
                wait(lambda s:PROMPT.search(s));start=len(text());uart.sendall(b'/bin/pkg-call-kernel\n')
                result=wait(lambda s:('CALL KERNEL PASS children=33 exits=30 reaped=33 rounds=3' in s[start:] or 'CALL KERNEL FAIL' in s[start:]) and PROMPT.search(s[start:]))[start:]
                assert 'CALL KERNEL FAIL' not in result and 'FAULT' not in result and 'PANIC' not in result,result
                mask=int(re.search(r'(?m)^CALL KERNEL CPU_MASK=(\d+)',result)[1]);assert mask and (smp==1 or mask&~1),mask
                start=len(text());uart.sendall(b'echo CALL_RC=$?\n');wait(lambda s:re.search(r'(?m)^CALL_RC=0\s*$',s[start:]) and PROMPT.search(s[start:]))
                uart.sendall(b'sudo shutdown\n');p.wait(timeout=25);assert p.returncode==0
                r=dict(firmware=fw,smp=smp,result='PASS',children=33,exit_cleanups=30,reaped=33,binding_cpu_mask=mask);(dest/'result.json').write_text(json.dumps(r,indent=2));print(r,flush=True);return r
            except BaseException as e:(dest/'failure.txt').write_text(repr(e));raise
            finally:
                stop.set();uart.close()
                if drainer:drainer.join(timeout=1)
                if p.poll() is None:
                    p.terminate()
                    try:p.wait(timeout=5)
                    except subprocess.TimeoutExpired:p.kill();p.wait(timeout=5)


def main():
    out=ROOT/'.codex-remote-attachments'/('pkg-call-kernel-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'));work=out/'workspace';out.mkdir(parents=True)
    media={str(p.relative_to(ROOT)):sha(p) for p in (ROOT/'bin').glob('*') if p.is_file()}
    sources={str(p.relative_to(ROOT)):sha(p) for folder in ('src','user','tests','scripts') for p in (ROOT/folder).rglob('*') if p.is_file() and p.suffix in ('.c','.h','.inc','.py','.asm') and '__pycache__' not in p.parts}
    sources['Makefile']=sha(ROOT/'Makefile')
    shutil.copytree(ROOT,work,ignore=shutil.ignore_patterns('.git','.codex-remote-attachments','build','bin','dist','__pycache__','*.pyc','*.iso','*.img','.VSCodeCounter'))
    assert all(sha(work/p)==h for p,h in sources.items());overlays=[]
    def replace(name,before,after):
        p=work/name;s=p.read_text();assert s.count(before)==1,(name,before);p.write_text(s.replace(before,after));overlays.append(dict(file=name,before=before,after=after))
    tcb_overlay(work,replace)
    replace('src/kernel/thread.h','struct pc_call *package_call;', 'struct pc_call *package_call; bool package_call_seen;')
    declarations='\nvoid call_kernel_exit(void);\nbool call_kernel_reap(tcb_t *);\nvoid call_kernel_reaped(bool);\n'
    replace('src/kernel/thread.c','#include "thread.h"','#include "thread.h"'+declarations)
    for signature in ('void process_exit(uint64_t exit_code) {','void thread_exit(void) {'):
        replace('src/kernel/thread.c',signature,signature+'\n    call_kernel_exit();')
    p='src/kernel/thread.c';old=function((work/p).read_text(),'void sched_reap_dead(void) {')
    new=old.replace('tcb_t *next = dead->next;','tcb_t *next = dead->next;\n            bool call_tracked=call_kernel_reap(dead);').replace('            kfree(dead);','            kfree(dead);\n            call_kernel_reaped(call_tracked);')
    replace(p,old,new)
    replace('src/kernel/syscall.c','#include "thread.h"','#include "thread.h"\nint64_t call_kernel_test(uint64_t,uint64_t);')
    old=function((work/'src/kernel/syscall.c').read_text(),'int syscall_dispatch(') if 'int syscall_dispatch(' in (work/'src/kernel/syscall.c').read_text() else None
    needle='        case SYS_SIGRETURN:'
    replace('src/kernel/syscall.c',needle,'        case 200: result=call_kernel_test(frame->rdi,frame->rsi);break;\n\n'+needle)
    (work/'src/kernel/pkg_call_private.c').write_bytes((work/'tests/pkg_call_kernel.inc').read_bytes())
    with (work/'Makefile').open('a') as f:f.write('\nCFLAGS += -Itests\n')
    m={'result':'INCOMPLETE','sources':sources,'shared_media':media,'overlays':overlays,'cases':[],
       'scope':'private copied kernel and syscall 200, disk-free actual Ring 3 signals/exit/reaper; no filesystem recovery/production/release-media acceptance'}
    def run(cmd,label,timeout=600):
        r=subprocess.run(cmd,cwd=work,text=True,capture_output=True,timeout=timeout);(out/(label+'.log')).write_text(r.stdout+r.stderr);assert not r.returncode,(label,r.stdout[-1000:],r.stderr[-2500:])
    try:
        run(['make','-j8','bin/fortress.elf'],'kernel-build')
        flags=['gcc','-Os','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-fno-pie','-fno-stack-protector','-mno-red-zone','-mno-sse','-mno-mmx','-msoft-float','-Wframe-larger-than=1024','-Isrc/include']
        run(flags+['-c','tests/pkg_call_ring3.c','-o','build/pkg-call.o'],'user-compile')
        run(['nasm','-f','elf64','-DTOOL_ENTRY=pkg_call_main','user/tools/start.asm','-o','build/pkg-call-start.o'],'user-start')
        run(['ld','-nostdlib','-static','-z','noexecstack','-T','user/shell.ld','build/pkg-call-start.o','build/pkg-call.o','-o','build/pkg-call.elf'],'user-link')
        template=ROOT/'bin/initramfs.tar';m['initramfs_template_sha256']=sha(template)
        with tarfile.open(template) as src,tarfile.open(out/'initramfs.tar','w',format=tarfile.USTAR_FORMAT) as dst:
            for member in src.getmembers():dst.addfile(member,src.extractfile(member) if member.isfile() else None)
            dst.add(work/'build/pkg-call.elf',arcname='bin/pkg-call-kernel')
        iso_root=out/'fixture-root';(iso_root/'boot/limine').mkdir(parents=True);(iso_root/'EFI/BOOT').mkdir(parents=True)
        shutil.copyfile(work/'bin/fortress.elf',iso_root/'boot/fortress.elf');shutil.copyfile(out/'initramfs.tar',iso_root/'boot/initramfs.tar')
        for name in ('limine-bios.sys','limine-bios-cd.bin','limine-uefi-cd.bin'):shutil.copyfile(work/'limine'/name,iso_root/'boot/limine'/name)
        shutil.copyfile(work/'limine/BOOTX64.EFI',iso_root/'EFI/BOOT/BOOTX64.EFI')
        config='timeout: 0\n/Private retained call validation\n    protocol: limine\n    kernel_path: boot():/boot/fortress.elf\n    module_path: boot():/boot/initramfs.tar\n'
        (iso_root/'boot/limine/limine.conf').write_text(config)
        iso=out/'private-call.iso'
        run(['xorriso','-as','mkisofs','-b','boot/limine/limine-bios-cd.bin','-no-emul-boot','-boot-load-size','4','-boot-info-table','--efi-boot','boot/limine/limine-uefi-cd.bin','-efi-boot-part','--efi-boot-image','--protective-msdos-label',str(iso_root),'-o',str(iso)],'private-fixture-iso')
        run([str(work/'limine/limine'),'bios-install',str(iso)],'private-bios-install')
        m['compiled_inputs']={p:sha(work/p) for p in sources if (work/p).exists()};m['private_iso_sha256']=sha(iso)
        for fw in ('bios','uefi'):
            for smp in (1,4):m['cases'].append(case(out,work,iso,fw,smp))
        m['result']='PASS'
    finally:
        m['changed_compiled_copies']=[p for p,h in m.get('compiled_inputs',{}).items() if sha(work/p)!=h]
        m['changed_repository_sources']=[p for p,h in sources.items() if not (ROOT/p).is_file() or sha(ROOT/p)!=h]
        m['changed_shared_media']=[p for p,h in media.items() if not (ROOT/p).is_file() or sha(ROOT/p)!=h]
        if m['changed_compiled_copies'] or m['changed_repository_sources'] or m['changed_shared_media']:m['preservation']='FAIL'
        else:m['preservation']='PASS'
        (out/'manifest.json').write_text(json.dumps(m,indent=2)+'\n')
    assert m['result']=='PASS' and m['preservation']=='PASS';print('Evidence:',out)


if __name__=='__main__':main()
