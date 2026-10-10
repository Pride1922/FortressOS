"""Phase3 isolated BIOS/UEFI x SMP=1/4, ISO only, paired disposable OVMF.

Random test credentials are generated through host libcrypt, never committed
or exposed as command arguments. Preserves the normal raw disk image.
"""
from pathlib import Path
import ctypes,hashlib,io,json,os,re,secrets,selectors,shutil,subprocess,tarfile,tempfile,time
ROOT=Path(__file__).resolve().parent.parent
(ROOT/'build/permissions-phase3').mkdir(parents=True,exist_ok=True)
out=Path(tempfile.mkdtemp(prefix='login-',dir=ROOT/'build/permissions-phase3'))
image=ROOT/'bin/fortress.img'
before=hashlib.sha256(image.read_bytes()).hexdigest() if image.is_file() else None
workspace=Path(subprocess.check_output(['python3','scripts/create_ext4_guest_workspace.py'],cwd=ROOT,text=True).strip())
assert workspace.is_relative_to(ROOT/'.codex-remote-attachments/ext4-phase9')
password=secrets.token_hex(12)
lib=ctypes.CDLL('libcrypt.so.1');lib.crypt.argtypes=[ctypes.c_char_p,ctypes.c_char_p];lib.crypt.restype=ctypes.c_char_p
hash_value=lib.crypt(password.encode(),('$5$'+secrets.token_hex(8)).encode()).decode()
hashfile=out/'test-operator.hash';hashfile.write_text(hash_value+'\n');hashfile.chmod(0o600)
env={**os.environ,'FORTRESS_OPERATOR_HASH_FILE':str(hashfile)}
with (out/'build.log').open('w') as log:
    subprocess.run(['make','-j4','LOGIN_TEST=1','bin/fortress.elf','bin/initramfs.tar','bin/fortress.iso','build/perm_phase3_user.elf'],cwd=workspace,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
def iso_for(label,bypass=False,passwordless=False):
    root=out/(label+'-iso');shutil.copytree(workspace/'build/iso_root',root)
    archive=out/(label+'.tar')
    with tarfile.open(workspace/'bin/initramfs.tar') as src,tarfile.open(archive,'w',format=tarfile.USTAR_FORMAT) as dst:
        for m in src.getmembers():
            if passwordless and m.name=='etc/shadow':
                data=b'root:!:::::::\noperator::::::::\n';m.size=len(data);dst.addfile(m,io.BytesIO(data))
            else:dst.addfile(m,src.extractfile(m) if m.isfile() else None)
        data=(workspace/'build/perm_phase3_user.elf').read_bytes();m=tarfile.TarInfo('bin/phase3-probe');m.size=len(data);m.mode=0o755;dst.addfile(m,io.BytesIO(data))
    for p in (root/'boot/initramfs.tar',root/'initramfs.tar'):shutil.copyfile(archive,p)
    config='timeout: 0\n/FortressOS Login Gate\n    protocol: limine\n    kernel_path: boot():/boot/fortress.elf\n    module_path: boot():/boot/initramfs.tar\n'
    if bypass:config+='    kernel_cmdline: login=0\n'
    for p in (root/'limine.conf',root/'boot/limine.conf',root/'boot/limine/limine.conf'):p.write_text(config)
    iso=out/(label+'.iso')
    subprocess.run(['xorriso','-as','mkisofs','-b','boot/limine/limine-bios-cd.bin','-no-emul-boot','-boot-load-size','4','-boot-info-table','--efi-boot','boot/limine/limine-uefi-cd.bin','-efi-boot-part','--efi-boot-image','--protective-msdos-label',str(root),'-o',str(iso)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    subprocess.run([str(workspace/'limine/limine'),'bios-install',str(iso)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    return iso
iso=iso_for('password');bypass_iso=iso_for('bypass',True);empty_iso=iso_for('passwordless',passwordless=True)
cases=[]
def run(mode,cpus,bootiso,kind):
    label=f'{kind}-{mode}-{cpus}';variables=out/(label+'-vars.fd')
    cmd=['qemu-system-x86_64','-M','q35','-m','2G','-accel','tcg','-smp',str(cpus),'-display','none','-monitor','none','-no-reboot','-boot','d','-cdrom',str(bootiso),'-serial','stdio','-net','none']
    if mode=='uefi':
        shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
        cmd+=['-drive','if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd','-drive',f'if=pflash,format=raw,unit=1,file={variables}']
    def preflight(actual):
        assert actual==cmd and bootiso.is_file() and bootiso.parent==out
        assert '-blockdev' not in actual and '-snapshot' not in actual
        assert actual.count('-drive')==(2 if mode=='uefi' else 0)
    preflight(cmd)
    for bad in (['-drive','file=/dev/sda'],['-blockdev','driver=host_device']):
        try:preflight(cmd+bad)
        except AssertionError:pass
        else:raise AssertionError('extra storage admitted')
    (out/(label+'-argv.json')).write_text(json.dumps(cmd,indent=2))
    transcript=bytearray()
    err=(out/(label+'.stderr')).open('wb');proc=subprocess.Popen(cmd,cwd=workspace,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=err)
    selector=selectors.DefaultSelector();selector.register(proc.stdout,selectors.EVENT_READ)
    def clean(data):return re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]','',data.decode(errors='replace')).replace('\r','')
    def wait(expected,start=0,timeout=180):
        end=time.monotonic()+timeout
        while time.monotonic()<end:
            for key,_ in selector.select(.1):
                b=key.fileobj.read1(65536)
                if b:
                    transcript.extend(b)
                    (out/(label+'.log')).write_bytes(transcript)
            text=clean(transcript[start:])
            assert not any(x in clean(transcript) for x in ('PHASE3 FAIL','CPU EXCEPTION KERNEL PANIC','[FATAL]')),clean(transcript[-6000:])
            if expected(text):return text
            assert proc.poll() is None,clean(transcript[-6000:])
        raise TimeoutError(clean(transcript[-6000:]))
    def send(s):proc.stdin.write((s+'\n').encode());proc.stdin.flush()
    def prompt(t):return re.search(r'(?:^|\n)[#$] $',t)
    def execute(command,expected):
        start=len(transcript);send(command);return wait(lambda t:expected in t and prompt(t),start)
    try:
        if kind=='bypass':
            wait(prompt);execute('/bin/phase3-probe --root','PHASE3 ROOT ABI/DROP PASS')
            execute('id','uid=0 gid=0 groups=0')
        else:
            wait(lambda t:'FortressOS login: ' in t)
            if kind=='password':
                send('operator');wait(lambda t:t.endswith('Password: '))
                start=len(transcript);begin=time.monotonic();send('wrong-password')
                text=wait(lambda t:'Login incorrect\nFortressOS login: ' in t,start)
                assert time.monotonic()-begin>=1.8 and 'wrong-password' not in text
                start=len(transcript);send('root');wait(lambda t:t.endswith('Password: '),start);send(password)
                wait(lambda t:'Login incorrect\nFortressOS login: ' in t,start)
                start=len(transcript);send('operator');wait(lambda t:t.endswith('Password: '),start);send(password)
            else:
                assert 'WARNING: live-media operator login is passwordless; home is temporary.' in clean(transcript)
                send('operator')
            wait(prompt)
            assert password not in clean(transcript)
            execute('/bin/phase3-probe','PHASE3 OPERATOR uid=1000 euid=1000 caps=0 groups=1000,10,44,104 PASS')
            start=len(transcript);proc.stdin.write(b'\x03');proc.stdin.flush();wait(prompt,start)
            assert 'FortressOS login: ' not in clean(transcript[start:])
            execute('id','uid=1000 gid=1000 groups=1000,10,44,104')
            execute('whoami','operator\n')
            execute('ls -l /run/user','drwx------ operator operator 0 1000')
            text=execute('env','HOME=/run/user/1000');assert all(x in text for x in ('USER=operator','LOGNAME=operator','SHELL=/bin/shell','PATH=/bin'))
            execute('pwd','/run/user/1000')
            execute('echo live > file; cat file','live\n')
            start=len(transcript);send('exit');wait(lambda t:'FortressOS login: ' in t,start)
            send('operator')
            if kind=='password':wait(lambda t:t.endswith('Password: '),start);send(password)
            wait(prompt,start);execute('/bin/phase3-probe','PHASE3 OPERATOR uid=1000 euid=1000 caps=0 groups=1000,10,44,104 PASS')
            execute('chmod 0777 /run/user/1000; ls -l /run/user','drwxrwxrwx operator operator')
            start=len(transcript);send('exit');wait(lambda t:'FortressOS login: ' in t,start);send('operator')
            if kind=='password':wait(lambda t:t.endswith('Password: '),start);send(password)
            wait(lambda t:'login: unsafe runtime directory' in t and t.count('FortressOS login: ')>=2,start)
        cases.append({'firmware':mode,'smp':cpus,'kind':kind,'result':'PASS','argv':label+'-argv.json',
            'kernel_sha256':hashlib.sha256((workspace/'bin/fortress.elf').read_bytes()).hexdigest()})
        print('PASS',label,flush=True)
    finally:
        proc.terminate()
        try:proc.wait(timeout=5)
        except subprocess.TimeoutExpired:proc.kill();proc.wait(timeout=5)
        selector.close();err.close();(out/(label+'.log')).write_bytes(transcript)
try:
    for cpus in (1,4):
        for mode in ('bios','uefi'):run(mode,cpus,iso,'password')
    for mode in ('bios','uefi'):run(mode,1,bypass_iso,'bypass')
    run('bios',1,empty_iso,'passwordless')
    test_workspace=workspace
    workspace=Path(subprocess.check_output(['python3','scripts/create_ext4_guest_workspace.py'],cwd=ROOT,text=True).strip())
    assert workspace.is_relative_to(ROOT/'.codex-remote-attachments/ext4-phase9')
    with (out/'production-build.log').open('w') as log:
        normal_env={k:v for k,v in os.environ.items() if k!='FORTRESS_OPERATOR_HASH_FILE'}
        subprocess.run(['make','-j4','bin/fortress.elf','bin/initramfs.tar','bin/fortress.iso','build/perm_phase3_user.elf'],cwd=workspace,env=normal_env,stdout=log,stderr=subprocess.STDOUT,check=True)
    symbols=subprocess.check_output(['nm',str(workspace/'bin/fortress.elf')],text=True)
    assert 'sys_test_setcreds' not in symbols
    assert b'login=0' not in (workspace/'bin/fortress.elf').read_bytes()
    # Normal kernel must ignore login=0 even when an ISO explicitly asks for it.
    normal_iso=iso_for('production-login0',bypass=True)
    for mode in ('bios','uefi'):run(mode,1,normal_iso,'normal')
finally:
    hashfile.unlink()
    after=hashlib.sha256(image.read_bytes()).hexdigest() if image.is_file() else None
    assert before==after,'normal disk image changed'
    (out/'manifest.json').write_text(json.dumps({'workspace':str(workspace),'test_workspace':str(locals().get('test_workspace',workspace)),'cases':cases,'normal_image_sha256':after,
        'kernel_sha256':hashlib.sha256((workspace/'bin/fortress.elf').read_bytes()).hexdigest(),
        'probe_sha256':hashlib.sha256((workspace/'build/perm_phase3_user.elf').read_bytes()).hexdigest(),
        'source_manifest':'workspace-manifest.json','scope':'ISO-only finite Ring3 BIOS/UEFI login; no physical claim'},indent=2))
print('Evidence:',out,flush=True)
