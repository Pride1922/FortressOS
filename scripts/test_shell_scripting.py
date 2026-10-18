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
    runner=(ROOT/'build/sh-builtin.elf').read_bytes()
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
        'examples/source.sh':b'echo SOURCE:$0:$#:$1\nprintf "ARG:<%s>\\n" "$@"\nSOURCED=yes\ncd /examples\nreturn 7\necho AFTER_RETURN_BAD\n',
        'examples/shift-source.sh':b'shift\necho SHIFT_SOURCE:$#:$1\n',
        'examples/return-default.sh':b'false\nreturn\necho RETURN_BAD\n',
        'examples/return-bad.sh':b'return 256\necho BAD_RETURN_STATUS:$?\nreturn 9\n',
        'examples/source-loop.sh':b'for z in c d; do echo INNER:$z; done\nreturn 5\n',
        'examples/source-outer.sh':b'for y in a b; do . /examples/source-loop.sh; echo MID:$y:$?; done\nreturn 6\n',
        'examples/source-depth.sh':b'. /examples/source-depth.sh\nreturn $?\n',
        'examples/source-output.sh':b'echo SOURCE_OUTPUT\n',
        'examples/source-empty.sh':b'',
        'examples/source-cat.sh':b'cat\necho SOURCE_CAT_DONE\n',
        'examples/source-scope.sh':b'echo SCOPED:$TMP\nOTHER=kept\nTMP=changed\n',
        'examples/source-break.sh':b'break\necho SOURCE_BREAK:$?\n',
        'examples/source-cancel.sh':b'. /examples/source-spin.sh\n',
        'examples/source-spin.sh':b'while true; do true; done\n',
        'examples/source-exit.sh':b'exit 11\n',
        'examples/source-reserved.sh':b'echo BAD 20>&-\necho RESERVED_CODE:$?\n',
        'examples/source-redir.sh':b'. /examples/source-output.sh > /run/user/1000/inner.log\necho OUTER_OUTPUT\n',
        'examples/source-negreturn.sh':b'! return 7\necho NEG_RETURN_BAD\n',
        'examples/source-reader-pipe.sh':b'cat <&20 | cat\necho READER_PIPE_CODE:$?\n',
        'examples/source-bare-return.sh':b'# preserve incoming status\n\nreturn\n',
        'examples/source-comments.sh':b'# only comments\n\n',
        'examples/until-source.sh':b'until false; do . /examples/return-default.sh; echo UNTIL_SOURCE:$?; break; done\n',
        'examples/conditions.sh':b'if false; then\necho WRONG\nelif test 3 -gt 2; then\necho BRANCH_OK\nelse\necho WRONG\nfi\n',
        'examples/loops.sh':b'for x in a "b c" ""; do\nprintf "FOR:<%s>\\n" "$x"\ndone\nX=go\nwhile [ "$X" = go ]; do\necho WHILE_ONCE\nX=stop\ndone\n',
        'examples/badcompound.sh':b'if true; then\necho COMPOUND_SHOULD_NOT_RUN\nelse else\nfi\n',
        'examples/operator-newline.sh':b'echo OP_FIRST &&\n# continuation comment\necho OP_SECOND\necho pipeline |\nwc -c\n',
    }
    for level in range(1,9):
        scripts[f'examples/depth{level}.sh']=(f'. /examples/depth{level+1}.sh\nreturn $?\n' if level<8 else 'echo EXACT_DEPTH8_OK\nreturn 8\n').encode()
    with tarfile.open(base/'bin/initramfs.tar') as src,tarfile.open(output,'w',format=tarfile.USTAR_FORMAT) as dst:
        for m in src.getmembers():
            if m.name in ('bin/shell','bin/sh','bin/sh-builtin'):continue
            dst.addfile(m,src.extractfile(m) if m.isfile() else None)
        for name in ('bin/shell','bin/sh'):
            m=tarfile.TarInfo(name);m.mode=0o755;m.size=len(shell);dst.addfile(m,io.BytesIO(shell))
        m=tarfile.TarInfo('bin/sh-builtin');m.mode=0o755;m.size=len(runner);dst.addfile(m,io.BytesIO(runner))
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
        command("sh -c 'case value in x; echo BAD; esac'; echo CODE:$?",['unsupported control keyword','CODE:2'])
        command("sh -c 'until true; do echo WRONG; done; echo UNTIL_EMPTY:$?'",['UNTIL_EMPTY:0'],['WRONG'])
        command("sh -c 'DONE=no; until [ \"$DONE\" = yes ]; do echo UNTIL_ONCE; DONE=yes; done'",['UNTIL_ONCE'])
        command("sh -c 'for x in a b; do until false; do echo UNTIL:$x; continue 2; done; done'",['UNTIL:a','UNTIL:b'])
        command("sh /examples/until-source.sh",['UNTIL_SOURCE:1'])
        command("sh -c 'until true; echo WRONG; done'; echo UNTIL_SYNTAX:$?",['UNTIL_SYNTAX:2'],['WRONG'])
        command("until false; do echo INTERACTIVE_UNTIL; break; done",['INTERACTIVE_UNTIL'])
        command("sh -c 'echo MISSING:$1'",['MISSING:'])
        command("sh -c 'echo \"${@}\"' custom 'space value'",['space value'])
        command("sh /examples/concurrent.sh",['NESTED','CONCURRENT_DONE'],['Unable to load','foreground wait failed'])
        command("sh /examples/conditions.sh",['BRANCH_OK'],['WRONG'])
        command("sh /examples/loops.sh",['FOR:<a>','FOR:<b c>','FOR:<>','WHILE_ONCE'])
        command("sh /examples/badcompound.sh; echo CODE:$?",['CODE:2'],['COMPOUND_SHOULD_NOT_RUN'])
        command("sh -c 'for x in a b; do if [ \"$x\" = a ]; then continue; fi; echo KEEP:$x; done'",['KEEP:b'],['KEEP:a'])
        command("sh -c 'for x in a b; do for y in c d; do echo ONCE; break 2; done; done'",['ONCE'])
        command("sh -c 'while true; do true && break && echo WRONG; done; echo BREAK_OK'",['BREAK_OK'],['WRONG'])
        command("sh -c '[ 9223372036854775808 -gt 0 ]; echo CODE:$?'",['invalid expression','CODE:2'])
        command("sh -c '[ -f /examples/args.sh ]; echo FILE:$?; [ -d /examples ]; echo DIR:$?'",['FILE:0','DIR:0'])
        command("sh -c 'test 3 -gt 2 | cat; echo PIPE_TEST:$?'",['PIPE_TEST:0'])
        command("sh -c 'if false || true; then echo SHORT_CIRCUIT_OK; fi'",['SHORT_CIRCUIT_OK'])
        command("sh -c 'for x; do printf \"DEFAULT:<%s>\\n\" \"$x\"; done' custom 'a b' ''",['DEFAULT:<a b>','DEFAULT:<>'])
        command("sh -c '[ -n x; echo MISSING_BRACKET:$?'",['MISSING_BRACKET:2'])
        command("if true; then echo INTERACTIVE_BRANCH; else echo WRONG; fi",['INTERACTIVE_BRANCH'],['WRONG'])
        command("sh /examples/operator-newline.sh",['OP_FIRST','OP_SECOND','9'])
        command("sh -c 'while false; do echo WRONG; done; echo EMPTY_LOOP:$?'",['EMPTY_LOOP:0'],['WRONG'])
        command("sh -c 'for x in; do echo WRONG; done; echo EMPTY_FOR:$?'",['EMPTY_FOR:0'],['WRONG'])
        command("sh -c 'echo BAD && if true; then echo WRONG; fi'; echo CODE:$?",['compound commands after list operators','CODE:2'])
        command("sh -c '. /examples/source.sh \"a b\" \"\"; echo SRC_CODE:$?; echo RESTORE:$0:$#:$1:$SOURCED; pwd' caller original",['SOURCE:caller:2:a b','ARG:<a b>','ARG:<>','SRC_CODE:7','RESTORE:caller:1:original:yes','/examples'],['AFTER_RETURN_BAD'])
        command("sh -c 'source /examples/source-empty.sh && echo EMPTY_SOURCE_OK'",['EMPTY_SOURCE_OK'])
        command("sh -c '. /examples/return-default.sh; echo DEFAULT_RETURN:$?'",['DEFAULT_RETURN:1'],['RETURN_BAD'])
        command("sh -c 'false; . /examples/source-bare-return.sh; echo INHERITED_RETURN:$?'",['INHERITED_RETURN:1'])
        command("sh -c 'false; . /examples/source-comments.sh; echo COMMENT_SOURCE:$?'",['COMMENT_SOURCE:0'])
        command("sh -c '. /examples/return-bad.sh; echo EXPLICIT_RETURN:$?'",['BAD_RETURN_STATUS:2','EXPLICIT_RETURN:9'])
        command("sh -c 'return 7; echo OUTSIDE:$?'",['outside sourced file','OUTSIDE:2'])
        command("sh -c '. /examples/shift-source.sh; echo SHARED_ARGS:$#:$1' caller first second",['SHIFT_SOURCE:1:second','SHARED_ARGS:1:second'])
        command("sh -c '. /examples/shift-source.sh x y; echo RESTORED_ARGS:$#:$1' caller first second",['SHIFT_SOURCE:1:y','RESTORED_ARGS:2:first'])
        command("sh -c 'for x in one two; do . /examples/source-outer.sh; echo OUTER:$x:$?; done'",['INNER:c','INNER:d','MID:a:5','MID:b:5','OUTER:one:6','OUTER:two:6'])
        command("sh -c '. /examples/source-depth.sh; echo DEPTH:$?; . /examples/source-empty.sh; echo RECOVER:$?'",['nesting exceeds eight','DEPTH:2','RECOVER:0'])
        command("sh -c '. /missing || echo MISSING_SOURCE:$?; . /examples || echo DIRECTORY_SOURCE:$?'",['MISSING_SOURCE:1','DIRECTORY_SOURCE:1'])
        command("sh -c '. /examples/incomplete.sh || echo INCOMPLETE_SOURCE:$?; . /examples/source-empty.sh && echo SOURCE_RECOVERED'",['INCOMPLETE_SOURCE:2','SOURCE_RECOVERED'])
        command("sh -c '. /examples/badcompound.sh; echo BAD_SOURCE:$?'",['BAD_SOURCE:2'],['COMPOUND_SHOULD_NOT_RUN'])
        command("sh -c '! . /examples/return-default.sh && echo NEGATE_SOURCE_OK'",['NEGATE_SOURCE_OK'])
        command("sh -c '. /examples/source-negreturn.sh; echo NEG_RETURN:$?'",['NEG_RETURN:7'],['NEG_RETURN_BAD'])
        command("sh -c '. /examples/return-default.sh && echo WRONG || echo SOURCE_OR_OK'",['SOURCE_OR_OK'],['WRONG'])
        command("sh -c '. /examples/source-output.sh > /run/user/1000/source.log; echo SOURCE_REDIR_RESTORED; cat /run/user/1000/source.log'",['SOURCE_REDIR_RESTORED','SOURCE_OUTPUT'])
        command("sh -c '. /examples/source-cat.sh < /examples/input.txt'",['CHILD_STDIN_CONTENT','SOURCE_CAT_DONE'])
        command("sh -c 'TMP=old; TMP=local . /examples/source-scope.sh; echo SCOPE:$TMP:$OTHER'",['SCOPED:local','SCOPE:old:kept'])
        command("sh -c 'for x in a b; do . /examples/source-break.sh; echo CALLER_LOOP:$x; done'",['SOURCE_BREAK:1','CALLER_LOOP:a','CALLER_LOOP:b'])
        command("sh -c 'command source /examples/source-output.sh; echo COMMAND_SOURCE:$?'",['SOURCE_OUTPUT','COMMAND_SOURCE:0'])
        command("sh -c '. /examples/source-exit.sh; echo AFTER_EXIT_BAD'; echo SOURCE_EXIT:$?",['SOURCE_EXIT:11'],['AFTER_EXIT_BAD'])
        command("sh -c 'for x in a b c d e f g h; do for y in a b c d e f g h; do . /examples/source-empty.sh; done; done; echo REPEATED_SOURCE:$?'",['REPEATED_SOURCE:0'],['cannot open','no reader'])
        command("source /examples/source-output.sh; echo INTERACTIVE_SOURCE:$?",['SOURCE_OUTPUT','INTERACTIVE_SOURCE:0'])
        command("sh -c '. /examples/depth1.sh; echo EXACT_DEPTH:$?'",['EXACT_DEPTH8_OK','EXACT_DEPTH:8'])
        command("sh /examples/source-reserved.sh",['descriptor reserved','RESERVED_CODE:2'],['BAD'])
        command("sh /examples/source-reader-pipe.sh",['descriptor reserved','READER_PIPE_CODE:2'])
        command("sh -c '. /examples/incomplete.sh x; echo RESTORE_ERROR:$#:$1' caller original",['RESTORE_ERROR:1:original'])
        command("sh -c '. /examples/source-redir.sh > /run/user/1000/outer.log; echo REDIR_DONE; cat /run/user/1000/inner.log; cat /run/user/1000/outer.log'",['REDIR_DONE','SOURCE_OUTPUT','OUTER_OUTPUT'])
        command("sh -c '. /examples/source-output.sh | cat; echo SOURCE_PIPE:$?'",["cannot run as a pipeline stage",'SOURCE_PIPE:1'],['SOURCE_OUTPUT'])
        command("sh -c '. /examples/source-output.sh &'; echo SOURCE_BG:$?",["cannot run as a pipeline stage",'SOURCE_BG:1'],['SOURCE_OUTPUT'])
        command("sh -c '. /examples/source-output.sh > /run/user/1000/highfd.log 20>&-; echo HIGHFD_RESTORED; cat /run/user/1000/highfd.log'",['HIGHFD_RESTORED','SOURCE_OUTPUT'])
        command("sh -c '. /examples/source-output.sh "+' '.join(['x']*32)+"; echo SOURCE_ARG_LIMIT:$#:$1:$?' caller original",['argument budget exceeded','SOURCE_ARG_LIMIT:1:original:2'],['SOURCE_OUTPUT'])
        start=len(data);send('. /examples/source-cancel.sh\n')
        deadline=time.monotonic()+.5
        while time.monotonic()<deadline:
            for key,_ in sel.select(.1):data.extend(key.fileobj.read1(65536))
        send('\x03');wait(lambda s:s.endswith('$ '),start,15);count+=1
        command('echo SOURCE_CANCEL:$?',['SOURCE_CANCEL:130'])
        command('. /examples/source-output.sh; echo AFTER_CANCEL:$?',['SOURCE_OUTPUT','AFTER_CANCEL:0'])
        start=len(data);send("sh -c 'sleep 30'; echo CANCEL_CODE:$?\n")
        # Give the foreground script time to enter its wait before Ctrl+C.
        deadline=time.monotonic()+1
        while time.monotonic()<deadline:
            for key,_ in sel.select(.1):data.extend(key.fileobj.read1(65536))
        send('\x03');out=wait(lambda s:s.endswith('$ '),start,15)
        assert 'CANCEL_CODE:130' in out,out;count+=1
        start=len(data);send('while true; do true; done\n')
        deadline=time.monotonic()+.5
        while time.monotonic()<deadline:
            for key,_ in sel.select(.1):data.extend(key.fileobj.read1(65536))
        send('\x03');wait(lambda s:s.endswith('$ '),start,15);count+=1
        command('echo LOOP_CANCEL:$?',['LOOP_CANCEL:130'])
        start=len(data);send('until false; do true; done\n')
        deadline=time.monotonic()+.5
        while time.monotonic()<deadline:
            for key,_ in sel.select(.1):data.extend(key.fileobj.read1(65536))
        send('\x03');wait(lambda s:s.endswith('$ '),start,15);count+=1
        command('echo UNTIL_CANCEL:$?',['UNTIL_CANCEL:130'])
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
