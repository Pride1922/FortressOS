"""Phase4 isolated ISO and disposable read-only GPT/USB, finite Ring3 gates."""
from pathlib import Path
import ctypes,hashlib,io,json,os,re,secrets,selectors,shutil,subprocess,tarfile,tempfile,time
from test_ext4_read import gpt
ROOT=Path(__file__).resolve().parent.parent
(ROOT/'build/permissions-phase4').mkdir(parents=True,exist_ok=True)
out=Path(tempfile.mkdtemp(prefix='sudo-',dir=ROOT/'build/permissions-phase4'))
normal=ROOT/'bin/fortress.img'
before=hashlib.sha256(normal.read_bytes()).hexdigest() if normal.is_file() else None
workspace=Path(subprocess.check_output(['python3','scripts/create_ext4_guest_workspace.py'],cwd=ROOT,text=True).strip())
assert workspace.is_relative_to(ROOT/'.codex-remote-attachments/ext4-phase9')
password=secrets.token_hex(12)
lib=ctypes.CDLL('libcrypt.so.1');lib.crypt.argtypes=[ctypes.c_char_p,ctypes.c_char_p];lib.crypt.restype=ctypes.c_char_p
hashfile=out/'operator.hash';hashfile.write_text(lib.crypt(password.encode(),('$5$'+secrets.token_hex(8)).encode()).decode()+'\n');hashfile.chmod(0o600)
cases=[]
try:
    with (out/'build.log').open('w') as log:
        subprocess.run(['make','-j4','LOGIN_TEST=1','bin/fortress.elf','bin/initramfs.tar','bin/fortress.iso','build/perm_phase4_user.elf'],cwd=workspace,
            env={**os.environ,'FORTRESS_OPERATOR_HASH_FILE':str(hashfile)},stdout=log,stderr=subprocess.STDOUT,check=True)
    probe=workspace/'build/perm_phase4_user.elf'
    # Linux builds an ext2 RO fixture independently; no production writes needed.
    fixture=out/'source.ext2'
    with fixture.open('xb') as f:f.truncate(32*1024*1024)
    with (out/'fixture.log').open('w') as log:
        subprocess.run(['mke2fs','-q','-t','ext2','-b','4096','-I','256','-O','none,filetype,sparse_super,large_file','-m','0',str(fixture)],check=True,stdout=log,stderr=subprocess.STDOUT)
        for command in (f'write {probe} /nosuid-probe','set_inode_field /nosuid-probe mode 0104755'):
            subprocess.run(['debugfs','-w','-R',command,str(fixture)],check=True,stdout=log,stderr=subprocess.STDOUT)
        subprocess.run(['e2fsck','-fn',str(fixture)],check=True,stdout=log,stderr=subprocess.STDOUT)
    def iso_for(kind):
        root=out/(kind+'-iso');shutil.copytree(workspace/'build/iso_root',root)
        archive=out/(kind+'.tar')
        with tarfile.open(workspace/'bin/initramfs.tar') as src,tarfile.open(archive,'w',format=tarfile.USTAR_FORMAT) as dst:
            for m in src.getmembers():
                data=src.extractfile(m).read() if m.isfile() else None
                if kind=='passwordless' and m.name=='etc/shadow':data=b'root:!:::::::\noperator::::::::\n'
                if kind=='nonwheel' and m.name=='etc/group':data=data.replace(b'wheel:x:10:operator',b'wheel:x:10:')
                if data is not None:m.size=len(data)
                dst.addfile(m,io.BytesIO(data) if data is not None else None)
            for name,mode,gid in [('phase4-probe',0o755,0),('suid-probe',0o4755,0),('sgid-probe',0o2755,44),('secure-shell',0o4755,0)]:
                data=(workspace/'build/shell.elf').read_bytes() if name=='secure-shell' else probe.read_bytes()
                m=tarfile.TarInfo('bin/'+name);m.size=len(data);m.mode=mode;m.gid=gid;dst.addfile(m,io.BytesIO(data))
        for p in (root/'boot/initramfs.tar',root/'initramfs.tar'):shutil.copyfile(archive,p)
        config='timeout: 0\n/FortressOS sudo gate\n    protocol: limine\n    kernel_path: boot():/boot/fortress.elf\n    module_path: boot():/boot/initramfs.tar\n    kernel_cmdline: usb_data=PARTUUID=11223344-5566-7788-99aa-bbccddeeff00 usb_data_mode=ro'
        if kind=='bypass':config+=' login=0'
        config+='\n'
        for p in (root/'limine.conf',root/'boot/limine.conf',root/'boot/limine/limine.conf'):p.write_text(config)
        iso=out/(kind+'.iso')
        subprocess.run(['xorriso','-as','mkisofs','-b','boot/limine/limine-bios-cd.bin','-no-emul-boot','-boot-load-size','4','-boot-info-table','--efi-boot','boot/limine/limine-uefi-cd.bin','-efi-boot-part','--efi-boot-image','--protective-msdos-label',str(root),'-o',str(iso)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        subprocess.run([str(workspace/'limine/limine'),'bios-install',str(iso)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        return iso
    isos={kind:iso_for(kind) for kind in ('password','nonwheel','passwordless','bypass')}
    def run(mode,cpus,kind):
        label=f'{kind}-{mode}-{cpus}';disk=out/(label+'.img');gpt(fixture,disk)
        disk_before=hashlib.sha256(disk.read_bytes()).hexdigest();variables=out/(label+'-vars.fd')
        cmd=['qemu-system-x86_64','-M','q35','-m','2G','-accel','tcg','-smp',str(cpus),'-display','none','-monitor','none','-no-reboot','-boot','d','-cdrom',str(isos[kind]),'-serial','stdio','-net','none',
             '-drive',f'file={disk},if=none,id=usb,format=raw','-device','qemu-xhci,id=xhci,p2=4,p3=0','-device','usb-storage,drive=usb,bus=xhci.0']
        if mode=='uefi':
            shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
            cmd+=['-drive','if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd','-drive',f'if=pflash,format=raw,unit=1,file={variables}']
        def preflight(actual):
            assert actual==cmd and disk.is_file() and disk.parent==out and isos[kind].parent==out
            assert '-blockdev' not in actual and '-snapshot' not in actual
            assert actual.count('-drive')==(3 if mode=='uefi' else 1)
        preflight(cmd)
        for bad in (['-drive','file=/dev/sda'],['-blockdev','driver=host_device'],['-snapshot']):
            try:preflight(cmd+bad)
            except AssertionError:pass
            else:raise AssertionError('extra storage accepted')
        (out/(label+'-argv.json')).write_text(json.dumps(cmd,indent=2))
        transcript=bytearray();err=(out/(label+'.stderr')).open('wb')
        proc=subprocess.Popen(cmd,cwd=workspace,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=err)
        selector=selectors.DefaultSelector();selector.register(proc.stdout,selectors.EVENT_READ)
        def clean(data):return re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]','',data.decode(errors='replace')).replace('\r','')
        def prompt(t):return re.search(r'fortress:[^\n]* \$ $',t)
        def wait(pred,start=0,timeout=180):
            end=time.monotonic()+timeout
            while time.monotonic()<end:
                for key,_ in selector.select(.1):
                    b=key.fileobj.read1(65536)
                    if b:transcript.extend(b);(out/(label+'.log')).write_bytes(transcript)
                text=clean(transcript[start:])
                assert not any(s in clean(transcript) for s in ('CPU EXCEPTION KERNEL PANIC','[FATAL]')),clean(transcript[-6000:])
                assert not re.search(r'PHASE4 FAIL[^\n]*\n',clean(transcript)),clean(transcript[-6000:])
                if pred(text):return text
                assert proc.poll() is None,clean(transcript[-6000:])
            raise TimeoutError(clean(transcript[-6000:]))
        def send(s):proc.stdin.write((s+'\n').encode());proc.stdin.flush()
        def execute(command,expected):
            start=len(transcript);send(command);return wait(lambda t:expected in t and prompt(t),start)
        def sudo(command,expected,wrong=False):
            start=len(transcript);send('sudo '+command)
            if kind!='passwordless':
                wait(lambda t:t.endswith('sudo password: '),start);send('wrong-password' if wrong else password)
            text=wait(lambda t:expected in t and prompt(t),start)
            assert password not in text and 'wrong-password' not in text
            return text
        try:
            if kind=='bypass':wait(prompt);execute('/bin/phase4-probe --root-drop','PHASE4 ROOT DROP PASS')
            else:
                wait(lambda t:'FortressOS login: ' in t);send('operator')
                if kind!='passwordless':wait(lambda t:t.endswith('Password: '));send(password)
                wait(prompt)
                execute('cat /etc/shadow','cat: cannot open /etc/shadow')
                if kind=='nonwheel':execute('sudo id','sudo: user is not authorized')
                else:
                    if kind!='passwordless':sudo('id','sudo: authentication failed',True)
                    sudo('id','uid=0 gid=0 groups=0')
                    text=sudo('cat /etc/shadow','root:!:::::::');assert ('operator::::::::' if kind=='passwordless' else 'operator:$5$') in text
                    text=sudo('/bin/sh-builtin env','HOME=/root');assert 'PATH=/bin' in text and 'USER=root' in text and '/evil' not in text
                    sudo('/bin/sh-builtin false','[PROCESS] Exit status 1')
                    text=execute('/bin/phase4-probe','PHASE4 SPAWN/FDS/STAGED/NOSUID PASS')
                    assert 'PHASE5 DENIAL/FUZZ PASS' in text
                    sudo('dmesg','-->')
                    # Secure shell drops hostile imported PATH/HOME and retains entry ABI.
                    execute('export PATH=/evil; export HOME=/evil; /bin/secure-shell','FortressOS shell (Ring 3)')
                    text=execute('env','PATH=/bin');assert 'HOME=/evil' not in text and 'PATH=/evil' not in text
                    execute('exit','fortress:')
                execute('/bin/id','uid=1000 gid=1000')
                execute('/bin/cat /etc/shadow','cat: cannot open /etc/shadow')
            cases.append({'firmware':mode,'smp':cpus,'kind':kind,'result':'PASS','argv':label+'-argv.json'})
            print('PASS',label,flush=True)
        finally:
            proc.terminate()
            try:proc.wait(timeout=5)
            except subprocess.TimeoutExpired:proc.kill();proc.wait(timeout=5)
            selector.close();err.close();(out/(label+'.log')).write_bytes(transcript)
            assert hashlib.sha256(disk.read_bytes()).hexdigest()==disk_before,'RO USB fixture changed'
    for cpus in (1,4):
        for mode in ('bios','uefi'):run(mode,cpus,'password')
    for kind in ('nonwheel','passwordless','bypass'):
        for mode in ('bios','uefi'):run(mode,1,kind)
finally:
    hashfile.unlink()
    after=hashlib.sha256(normal.read_bytes()).hexdigest() if normal.is_file() else None
    assert before==after,'normal disk image changed'
    (out/'manifest.json').write_text(json.dumps({'workspace':str(workspace),'cases':cases,'normal_image_sha256':after,
        'kernel_sha256':hashlib.sha256((workspace/'bin/fortress.elf').read_bytes()).hexdigest() if (workspace/'bin/fortress.elf').is_file() else None,
        'probe_sha256':hashlib.sha256(probe.read_bytes()).hexdigest() if 'probe' in locals() else None,
        'source_manifest':'workspace-manifest.json','scope':'finite Ring3 BIOS/UEFI; guest RO disposable USB with unchanged-image audit; no physical claim'},indent=2))
    print('Evidence:',out,flush=True)
