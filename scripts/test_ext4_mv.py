#!/usr/bin/env python3
"""EH-6: isolated installed NVMe BIOS/UEFI x SMP=1/4, shutdown/reboot/Linux."""
from pathlib import Path
import hashlib,json,re,selectors,shutil,subprocess,tarfile,tempfile,time
import test_ext4_pkg_scale as installed
from test_root_phase2 import extract_root
ROOT=Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def command_run(cmd,**kwargs):return subprocess.run(cmd,check=True,**kwargs)
def boot(work,out,disk,firmware,smp,verify=False):
    case=out/f'{firmware}-{smp}';case.mkdir(exist_ok=True);phase='reboot' if verify else 'first'
    cmd=['qemu-system-x86_64','-M','q35','-accel','tcg','-cpu','max,+rdrand,+rdseed','-m','1G','-smp',str(smp),
         '-display','none','-monitor','none','-no-reboot','-net','none','-serial','stdio',
         '-drive',f'file={disk},if=none,id=data,format=raw','-device','nvme,drive=data,serial=DIRMOVEINSTALLED,bootindex=1']
    if firmware=='uefi':
        variables=disk.parent/f'{phase}-vars.fd';shutil.copyfile(installed.OVMF_VARS,variables)
        cmd+=['-drive',f'if=pflash,format=raw,unit=0,readonly=on,file={installed.OVMF_CODE}',
              '-drive',f'if=pflash,format=raw,unit=1,file={variables}']
    assert disk.is_file() and disk.parent.resolve().is_relative_to(Path(tempfile.gettempdir()).resolve())
    assert cmd.count('-drive')==(3 if firmware=='uefi' else 1)
    assert all('/dev/' not in s and 'host_device' not in s for s in cmd)
    (case/f'{phase}-argv.json').write_text(json.dumps(cmd,indent=2)+'\n');log=bytearray()
    with (case/f'{phase}-stderr.log').open('wb') as err:
        proc=subprocess.Popen(cmd,cwd=work,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=err)
        selector=selectors.DefaultSelector();selector.register(proc.stdout,selectors.EVENT_READ)
        def text():return re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]','',log.decode(errors='replace')).replace('\r','')
        def wait(predicate,timeout=90):
            deadline=time.monotonic()+timeout
            while time.monotonic()<deadline:
                for key,_ in selector.select(.1):
                    chunk=key.fileobj.read1(65536)
                    if chunk:log.extend(chunk);(case/f'{phase}-serial.log').write_bytes(log)
                s=text();assert not any(x in s for x in ('KERNEL PANIC','[PROCESS FAULT]','DIR_RENAME FAIL')),s[-5000:]
                if predicate(s):return s
                assert proc.poll() is None,s[-5000:]
            raise AssertionError(text()[-5000:])
        def send(s):proc.stdin.write(s.encode()+b'\n');proc.stdin.flush()
        try:
            wait(lambda s:'FortressOS login:' in s);send('operator');wait(lambda s:s.endswith('Password: '));send(installed.TEST_PASSWORD)
            wait(lambda s:'operator@fortress:' in s and s.endswith('$ '));pos=len(text());send('sudo /bin/shell')
            wait(lambda s:'sudo password: ' in s[pos:]);send(installed.TEST_PASSWORD)
            wait(lambda s:'root@fortress:' in s and s.endswith('# '));pos=len(text())
            send('/bin/dir-rename-probe'+(' verify' if verify else ''))
            marker='DIR_RENAME REBOOT PASS' if verify else 'DIR_RENAME Ring3 PASS'
            wait(lambda s:marker in s[pos:] and s.endswith('# '))
            if not verify:
                pos=len(text());send('/bin/mv /opt/tool-dir /var/tool-dir');wait(lambda s:s.endswith('# ') and len(s)>pos)
                assert 'operation failed' not in text()[pos:],text()[pos:]
            send('shutdown');proc.wait(timeout=40);assert proc.returncode==0
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:proc.wait(timeout=5)
                except subprocess.TimeoutExpired:proc.kill();proc.wait(timeout=5)
            log.extend(proc.stdout.read());selector.close();(case/f'{phase}-serial.log').write_bytes(log)
    return {'phase':phase,'result':'PASS'}
def audit(out,disk,firmware,smp,phase):
    image=disk.parent/(phase+'-audit.ext4');extract_root(disk,image)
    p=subprocess.run(['e2fsck','-fn',str(image)],capture_output=True,text=True);assert p.returncode==0,p.stdout+p.stderr
    log=p.stdout+p.stderr
    def debug(command):
        p=subprocess.run(['debugfs','-R',command,str(image)],capture_output=True,text=True);assert p.returncode==0
        return p.stdout+p.stderr
    parent=debug('stat /var');ino=int(re.search(r'Inode:\s+(\d+)',parent)[1]);log+=parent
    for path in ('/var/moveddir','/var/tool-dir'):
        listing=debug('ls -l '+path);log+=listing
        assert re.search(rf'^\s*{ino}\s+.*\s\.\.\s*$',listing,re.M),listing
        payload=disk.parent/'payload';suffix='/sub/nested/data' if path.endswith('moveddir') else '/sub/data'
        log+=debug(f'dump {path}{suffix} {payload}');assert payload.read_bytes()==b'DIRECTORY-RENAME-PAYLOAD';payload.unlink()
    for path in ('/opt/testdir','/opt/renameddir','/opt/tool-dir'):
        missing=debug('stat '+path);assert 'Inode:' not in missing;log+=missing
    (out/f'{firmware}-{smp}'/f'{phase}-linux.log').write_text(log)
def main():
    out=ROOT/'.codex-remote-attachments'/('ext4-mv-'+time.strftime('%Y%m%d-%H%M%S'));out.mkdir();work=out/'workspace'
    shutil.copytree(ROOT,work,ignore=shutil.ignore_patterns('.git','.codex-remote-attachments','build','bin','dist','__pycache__','*.pyc','*.iso','*.img'))
    checkpoint=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()
    # Freeze only unrelated concurrent user commands in this private copy.
    frozen=subprocess.check_output(['git','ls-tree','-r','--name-only',checkpoint,'--','user'],cwd=ROOT,text=True).splitlines()
    for name in frozen:(work/name).write_bytes(subprocess.check_output(['git','show',checkpoint+':'+name],cwd=ROOT))
    with (work/'Makefile').open('a') as f:f.write('\n$(BUILD_DIR)/fs/ext4.o: CFLAGS += -fstack-usage\n')
    paths=[p for p in (ROOT/'src/fs').iterdir() if p.is_file()]+[ROOT/p for p in ('tests/dir_rename_probe.c','scripts/test_ext4_mv.py','scripts/test_ext4_pkg_scale.py')]
    sources={str(p.relative_to(ROOT)):sha(p) for p in paths};media={str(p.relative_to(ROOT)):sha(p) for p in (ROOT/'bin').iterdir() if p.is_file()}
    assert all(sha(work/p)==h for p,h in sources.items())
    with (out/'build.log').open('w') as log:command_run(['make','-j4','bin/fortress.elf','bin/initramfs.tar'],cwd=work,stdout=log,stderr=subprocess.STDOUT,timeout=600)
    frames={}
    for line in (work/'build/fs/ext4.su').read_text().splitlines():
        location,size,kind=line.split('\t');name=location.rsplit(':',1)[-1]
        if name.startswith(('e4_move_','e4_rename','e4_jrename','ext4_engine_namespace_rename')):
            frames[name]=int(size);assert int(size)<=1024,(name,size)
    assert 'e4_rename_plan' in frames and 'e4_move_tree_check' in frames
    (out/'rename-stack.json').write_text(json.dumps(frames,indent=2)+'\n')
    probe=work/'build/dir-rename-probe.elf';obj=work/'build/dir-rename-probe.o';start=work/'build/dir-rename-start.o'
    cmd=['gcc','-std=c11','-Os','-ffreestanding','-fno-pie','-fno-stack-protector','-fstack-usage','-m64','-mno-red-zone','-mgeneral-regs-only','-Wall','-Wextra','-Werror','-Wframe-larger-than=1024','-Isrc/include','-Isrc/fs','-Iuser/tools','-c','tests/dir_rename_probe.c','-o',str(obj)]
    command_run(cmd,cwd=work);command_run(['nasm','-f','elf64','-DTOOL_ENTRY=dir_rename_probe_main','user/tools/start.asm','-o',str(start)],cwd=work)
    command_run(['ld','-m','elf_x86_64','-nostdlib','-static','-z','noexecstack','-T','tests/replace_probe.ld',str(start),str(obj),str(work/'build/tool-common.o'),'-o',str(probe)],cwd=work)
    with tarfile.open(work/'bin/initramfs.tar') as archive:assert not any('dir-rename-probe' in m.name for m in archive)
    installed.REPO=work;installed.fixture.REPO=work;results=[]
    for firmware in ('bios','uefi'):
        for smp in (1,4):
            with tempfile.TemporaryDirectory(prefix='ext4-installed-mv-') as tmp:
                folder=Path(tmp);disk,_=installed.prepare_installed_gpt_disk(folder,probe,'dir-rename-probe');boots=[]
                for verify in (False,True):
                    boots.append(boot(work,out,disk,firmware,smp,verify));audit(out,disk,firmware,smp,'reboot' if verify else 'first')
                result={'firmware':firmware,'smp':smp,'boots':boots,'linux_audits':2,'result':'PASS'};results.append(result);print(result,flush=True)
    changes=[p for p,h in sources.items() if sha(ROOT/p)!=h];changed_media=[p for p,h in media.items() if not (ROOT/p).is_file() or sha(ROOT/p)!=h]
    (out/'manifest.json').write_text(json.dumps({'results':results,'sources':sources,'source_changes_during_gate':changes,'shared_outputs_changed_during_gate':changed_media,'unrelated_frozen_checkpoint':checkpoint,'unrelated_frozen_paths':frozen,'kernel_sha256':sha(work/'bin/fortress.elf'),'probe_sha256':sha(probe),'scope':'installed NVMe disposable regular files only; no physical or sector-tear claim'},indent=2)+'\n');print(out,flush=True)
if __name__=='__main__':main()
