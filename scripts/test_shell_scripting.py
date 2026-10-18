#!/usr/bin/env python3
"""Disk-free BIOS/UEFI scripting acceptance using a disposable ISO only.

--base points to a previously built release checkout. Its files are read-only
inputs; the new shell and test scripts are injected into a temporary initramfs.
"""
import argparse,hashlib,io,json,re,selectors,shutil,subprocess,tarfile,tempfile,time
from pathlib import Path
ROOT=Path(__file__).resolve().parent.parent
CODE=Path('/usr/share/OVMF/OVMF_CODE_4M.fd')
VARS=Path('/usr/share/OVMF/OVMF_VARS_4M.fd')

def fixture(base,tmp):
    tree=tmp/'iso';shutil.copytree(base/'build/iso_root',tree)
    output=tmp/'initramfs.tar';shell=(ROOT/'build/shell.elf').read_bytes()
    scripts={
        'examples/args.sh':b'# parameters\r\nNAME=hello\r\necho "$NAME:$0:$#:$1"\r\nprintf "<%s>\\n" "$@"\r\nshift\r\necho "SHIFT:$#:$1"\r\necho END_ARGS',
        'examples/stdin.sh':b'cat\necho AFTER_CAT\n',
        'examples/input.txt':b'CHILD_STDIN_CONTENT\n',
        'examples/bad.sh':b'echo BEFORE_BAD\n| echo bad\necho SHOULD_NOT_RUN\n',
        'examples/incomplete.sh':b'echo "unfinished',
        'examples/over.sh':b'echo SHOULD_NOT_PRINT '+b'x'*4096+b'\n',
        'examples/continuation.sh':b'echo JOIN_a\\\nb\necho "MULTI\nLINE"\n',
        'examples/background.sh':b'sleep 30 &\necho BACKGROUND_DONE\n',
        'examples/nested.sh':b'echo NESTED\n'*20,
        'examples/concurrent.sh':b'sh /examples/nested.sh &\nsh /examples/nested.sh\nsleep 0.1\necho CONCURRENT_DONE\n',
    }
    with tarfile.open(base/'bin/initramfs.tar') as src,tarfile.open(output,'w',format=tarfile.USTAR_FORMAT) as dst:
        for m in src.getmembers():
            if m.name in ('bin/shell','bin/sh'):continue
            dst.addfile(m,src.extractfile(m) if m.isfile() else None)
        for name in ('bin/shell','bin/sh'):
            m=tarfile.TarInfo(name);m.mode=0o755;m.size=len(shell);dst.addfile(m,io.BytesIO(shell))
        m=tarfile.TarInfo('examples');m.type=tarfile.DIRTYPE;m.mode=0o755;dst.addfile(m)
        for name,data in scripts.items():
            m=tarfile.TarInfo(name);m.mode=0o644;m.size=len(data);dst.addfile(m,io.BytesIO(data))
    for path in ('boot/initramfs.tar','initramfs.tar'):shutil.copyfile(output,tree/path)
    iso=tmp/'test.iso'
    subprocess.run(['xorriso','-as','mkisofs','-b','boot/limine/limine-bios-cd.bin','-no-emul-boot',
                    '-boot-load-size','4','-boot-info-table','--efi-boot','boot/limine/limine-uefi-cd.bin',
                    '-efi-boot-part','--efi-boot-image','--protective-msdos-label',str(tree),'-o',str(iso)],
                    check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    subprocess.run([str(base/'limine/limine'),'bios-install',str(iso)],check=True,
                   stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    return iso

def boot(iso,tmp,firmware,smp):
    argv=['qemu-system-x86_64','-M','q35','-m','512M','-smp',str(smp),'-cpu','max,+rdrand,+rdseed',
          '-accel','tcg','-display','none','-monitor','none','-no-reboot','-serial','stdio',
          '-boot','d','-cdrom',str(iso),'-net','none']
    if firmware=='uefi':
        var=tmp/f'vars-{smp}.fd';shutil.copyfile(VARS,var)
        argv+=['-drive',f'if=pflash,format=raw,unit=0,readonly=on,file={CODE}',
               '-drive',f'if=pflash,format=raw,unit=1,file={var}']
    proc=subprocess.Popen(argv,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL)
    sel=selectors.DefaultSelector();sel.register(proc.stdout,selectors.EVENT_READ);data=bytearray();count=0
    def send(s):proc.stdin.write(s.encode());proc.stdin.flush()
    def wait(predicate,start=0,timeout=90):
        end=time.monotonic()+timeout;text=''
        while time.monotonic()<end:
            for key,_ in sel.select(.1):data.extend(key.fileobj.read1(65536))
            text=re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]','',data[start:].decode(errors='replace')).replace('\r','')
            if predicate(text):return text
            assert proc.poll() is None,text[-1500:]
        raise AssertionError('timeout: '+text[-2000:])
    def command(cmd,expected,absent=()):
        nonlocal count
        start=len(data);send(cmd+'\n');out=wait(lambda s:'\n' in s and s.endswith('$ '),start,30)
        # Ignore the editor's command echo: assertions concern command output.
        body=out.split('\n',1)[1] if '\n' in out else ''
        for value in expected:assert value in body,(cmd,body)
        for value in absent:assert value not in body,(cmd,body)
        count+=1;return body
    try:
        wait(lambda s:s.endswith('FortressOS login: '));send('operator\n');wait(lambda s:s.endswith('$ '))
        command("sh -c 'echo SIMPLE'",['SIMPLE'],['Welcome to FortressOS','[PROCESS]'])
        command("sh -c 'exit 7'; echo CODE:$?",['CODE:7'])
        command("sh -c 'exit 0'; echo CODE:$?",['CODE:0'])
        command("sh -c 'missing_command'; echo CODE:$?",['CODE:127'])
        command("sh /examples/args.sh 'a b' '' last",['hello:/examples/args.sh:3:a b','<a b>','<>','<last>','SHIFT:2:','END_ARGS'])
        command("sh -c 'echo NAME:$0:$#:$1' custom value",['NAME:custom:1:value'])
        command("sh -c 'printf \"<%s>\\n\" \"pre$@post\"' custom one two",['<preone>','<twopost>'])
        command("sh -c 'echo A \"$@\" Z'",['A Z'])
        command("sh /examples/stdin.sh < /examples/input.txt",['CHILD_STDIN_CONTENT','AFTER_CAT'])
        command("printf 'echo PIPE_INPUT\\n' | sh",['PIPE_INPUT'])
        command("sh /examples/continuation.sh",['JOIN_ab','MULTI\nLINE'])
        command("sh /examples/bad.sh; echo CODE:$?",['BEFORE_BAD','syntax error','CODE:2'],['SHOULD_NOT_RUN'])
        command("sh /examples/incomplete.sh; echo CODE:$?",['unexpected EOF','CODE:2'])
        command("sh /examples/over.sh; echo CODE:$?",['exceeds 4096','CODE:2'],['SHOULD_NOT_PRINT'])
        command("sh /examples/background.sh",['BACKGROUND_DONE'])
        command("sh -c 'cd /examples; pwd'; pwd",['/examples','/run/user/1000'])
        command("sh -c 'echo hello | wc -c'",['6'])
        command("sh -c 'exit 256; echo STILL_HERE'",['STILL_HERE'])
        command("sh -c 'shift 2; echo SHIFT_CODE:$?' custom one",['SHIFT_CODE:1'])
        command("sh -c 'echo ${10}:${#}' custom a b c d e f g h i ten",['ten:10'])
        command("sh -c 'echo ${bad:-default}'; echo CODE:$?",['Expansion exceeds shell limits','CODE:1'])
        command("sh -c 'if echo WRONG; echo SHOULD_NOT_RUN'; echo CODE:$?",['compound syntax','CODE:2'],['SHOULD_NOT_RUN'])
        command("sh -c 'echo MISSING:$1'",['MISSING:'])
        command("sh -c 'echo \"${@}\"' custom 'space value'",['space value'])
        command("sh /examples/concurrent.sh",['NESTED','CONCURRENT_DONE'],['Unable to load','foreground wait failed'])
        start=len(data);send("sh -c 'sleep 30'; echo CANCEL_CODE:$?\n")
        # Give the foreground script time to enter its wait before Ctrl+C.
        deadline=time.monotonic()+1
        while time.monotonic()<deadline:
            for key,_ in sel.select(.1):data.extend(key.fileobj.read1(65536))
        send('\x03');out=wait(lambda s:s.endswith('$ '),start,15)
        assert 'CANCEL_CODE:130' in out,out;count+=1
        command('echo INTERACTIVE_OK',['INTERACTIVE_OK'])
        print(f'PASS {firmware} SMP={smp}: {count} scripting/interactive cases (disk-free)',flush=True)
        return {'firmware':firmware,'smp':smp,'cases':count,'status':'PASS'}
    finally:
        proc.terminate()
        try:proc.wait(timeout=5)
        except subprocess.TimeoutExpired:proc.kill();proc.wait()
        sel.close()

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--base',type=Path,required=True)
    parser.add_argument('--smp',type=int,nargs='+',default=[1,4]);args=parser.parse_args()
    base=args.base.resolve()
    inputs=[base/'bin/fortress.img',base/'bin/fortress.iso',base/'bin/initramfs.tar',base/'build/iso_root/boot/fortress.elf']
    digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
    before={str(p):digest(p) for p in inputs};results=[]
    with tempfile.TemporaryDirectory(prefix='fortress-scripting-') as tmp:
        tmp=Path(tmp);iso=fixture(base,tmp)
        for firmware in ('bios','uefi'):
            for smp in args.smp:results.append(boot(iso,tmp,firmware,smp))
    assert before=={str(p):digest(p) for p in inputs},'base release inputs changed during acceptance'
    report=ROOT/'build/shell-scripting-result.json'
    report.write_text(json.dumps({'cases':results,'base_inputs_unchanged':before,'shell_sha256':digest(ROOT/'build/shell.elf')},indent=2)+'\n')
    print('PASS release input preservation; '+str(report),flush=True)
if __name__=='__main__':main()
