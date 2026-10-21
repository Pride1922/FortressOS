#!/usr/bin/env python3
"""Private disk-free PKG-1 Ring 3 fixtures; never builds shared release media.

Copies current sources/dependencies, runs host gates in that copy, and stages
only private test executables and public-key fixtures. No production trust.
"""
from pathlib import Path
import hashlib
import json
import shutil
import subprocess
import time
import re
import socket
import tempfile
import threading
from test_getrandom import CODE, VARS, PROMPT
from test_pkg_cli_host import INCLUDES

ROOT = Path(__file__).resolve().parent.parent


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def run(args, work, log):
    subprocess.run(args, cwd=work, stdout=log, stderr=subprocess.STDOUT,
                   check=True, timeout=300)


def case(out,work,firmware,smp,checks):
    dest=out/(firmware+'-smp'+str(smp));dest.mkdir()
    with tempfile.TemporaryDirectory(prefix='fortress-pkg-cli-') as tmp:
        sockpath=Path(tmp)/'uart'
        cpu='max'
        cmd=['qemu-system-x86_64','-M','q35','-accel','tcg','-cpu',cpu,'-m','512M','-smp',str(smp),
             '-display','none','-net','none','-no-reboot','-boot','d','-cdrom',str(work/'bin/fortress.iso'),
             '-chardev',f'socket,id=uart,path={sockpath},server=on,wait=off,logfile={dest/"serial.log"}',
             '-serial','chardev:uart']
        if firmware=='uefi':
            assert CODE.is_file() and VARS.is_file()
            shutil.copyfile(VARS,dest/'vars.fd')
            cmd+=['-drive',f'if=pflash,format=raw,unit=0,readonly=on,file={CODE}',
                  '-drive',f'if=pflash,format=raw,unit=1,file={dest/"vars.fd"}']
        # Final command is constructed here; only optical media and pflash exist.
        assert not any(p in cmd for p in ('-hda','-hdb','-blockdev','-readconfig'))
        for i,c in enumerate(cmd):
            if c=='-drive':assert cmd[i+1].startswith('if=pflash,format=raw,unit=')
        (dest/'argv.json').write_text(json.dumps(cmd,indent=2))
        with (dest/'qemu.stderr').open('w') as err:
            p=subprocess.Popen(cmd,cwd=work,stderr=err)
            uart=socket.socket(socket.AF_UNIX);stop=threading.Event();thread=None
            def text():
                path=dest/'serial.log'
                s=path.read_text(errors='replace') if path.exists() else ''
                return re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]','',s).replace('\r','')
            def wait(pred,timeout=90):
                deadline=time.monotonic()+timeout
                while time.monotonic()<deadline:
                    s=text()
                    if pred(s):return s
                    assert p.poll() is None,('QEMU exited',p.returncode)
                    time.sleep(.05)
                raise AssertionError('timeout: '+text()[-3000:])
            try:
                deadline=time.monotonic()+10
                while True:
                    try:uart.connect(str(sockpath));break
                    except (FileNotFoundError,ConnectionRefusedError):
                        assert time.monotonic()<deadline;time.sleep(.05)
                uart.settimeout(.2)
                def drain():
                    while not stop.is_set():
                        try:
                            if not uart.recv(65536):return
                        except socket.timeout:pass
                        except OSError:return
                thread=threading.Thread(target=drain,daemon=True);thread.start()
                wait(lambda s:'FortressOS login: ' in s or PROMPT.search(s))
                if not PROMPT.search(text()):uart.sendall(b'operator\n')
                wait(lambda s:PROMPT.search(s))
                def command(c,marker):
                    pos=len(text());uart.sendall(c.encode()+b'\n')
                    # Match an output line, never the terminal's echoed command
                    # or a transient redraw that happens to end in '$ '.
                    output=re.compile(r'(?m)^(?:pkg: |Read-only verification\. )?'+re.escape(marker))
                    return wait(lambda s:output.search(s[pos:]) and PROMPT.search(s[pos:]))[pos:]
                for cmd, expected in checks:
                    s=command(cmd, expected)
                    assert expected in s,s
                    status=command('echo PKG_RC=$?', 'PKG_RC=')
                    code=0 if expected in ('Installation is unavailable.', 'integrity OK; signatures not checked',
                                           'catalog policy verified; platform/install admission not checked', 'STACK PASS') else 1
                    assert re.search(r'(?m)^PKG_RC='+str(code)+r'\s*$', status),status
                    if 'OK;' not in expected and 'policy verified;' not in expected and expected != 'STACK PASS':
                        assert 'integrity OK;' not in s and 'catalog policy verified;' not in s,s
                    assert 'FAULT' not in s and 'PANIC' not in s,s
                uart.sendall(b'sudo shutdown\n');p.wait(timeout=20);assert p.returncode==0
                result={'case':dest.name,'status':'PASS'}
                result['stack_metrics'] = re.findall(r'(?m)^STACK ([a-z_]+)=(\d+)\s*$', text())
                (dest/'result.json').write_text(json.dumps(result));print(json.dumps(result),flush=True)
                return result
            except BaseException as exc:
                (dest/'failure.txt').write_text(repr(exc));raise
            finally:
                stop.set();uart.close()
                if thread:thread.join(timeout=1)
                if p.poll() is None:
                    p.terminate()
                    try:p.wait(timeout=5)
                    except subprocess.TimeoutExpired:p.kill();p.wait(timeout=5)

def main(prepare=None):
    out = ROOT/'.codex-remote-attachments'/('pkg-cli-qemu-'+time.strftime('%Y%m%d-%H%M%S'))
    out.mkdir(parents=True)
    # Keep the workspace alongside evidence: temporary WSL paths may disappear
    # between tool sessions and would prevent replay of the exact fixture.
    work = out/'workspace'
    sources = {str(p.relative_to(ROOT)): sha(p) for prefix in ('src', 'user', 'tests', 'scripts')
               for p in (ROOT/prefix).rglob('*') if p.is_file()
               and not set(('build', 'bin', 'dist', '__pycache__')).intersection(p.relative_to(ROOT).parts)
               and p.suffix not in ('.pyc', '.iso', '.img')}
    sources['Makefile'] = sha(ROOT/'Makefile')
    media = {str(p.relative_to(ROOT)): sha(p) for p in (ROOT/'bin').glob('*') if p.is_file()}
    manifest = {'sources': sources, 'shared_media': media, 'workspace': str(work)}
    manifest['head_before'] = subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()
    manifest['status_before'] = subprocess.check_output(['git','status','--short'],cwd=ROOT,text=True)
    (out/'manifest.json').write_text(json.dumps(manifest, indent=2))
    shutil.copytree(ROOT, work, dirs_exist_ok=True, ignore=shutil.ignore_patterns(
        '.git', '.VSCodeCounter', '.codex-remote-attachments', 'build', 'bin', 'dist', '__pycache__', '*.pyc', '*.iso', '*.img'))
    assert (work/'limine/limine').is_file()
    for name, digest in sources.items():
        assert sha(work/name) == digest, ('source changed while copying', name)
    with (out/'build.log').open('w') as log:
        run(['make', 'test-pkg-cli-host'], work, log)
        run(['make', '-j8', 'bin/fortress.elf', 'bin/initramfs.tar'], work, log)
        # Regenerate a long-lived signed TEST catalog; kernel RTC is used by CLI.
        import sys
        sys.path.insert(0, str(work/'scripts'))
        import os
        old = Path.cwd()
        os.chdir(work)
        try:
            import pkg_catalog_vectors as vectors
            good = vectors.catalog(issued=1767225600, expires=4102444800)
            public = vectors.PUB
        finally:
            os.chdir(old)
        def cbytes(data):
            return '{'+','.join(str(b) for b in data)+'}'
        wrapper = work/'build/pkg-cli/test-policy.c'
        wrapper.write_text('#include "user/pkg/cli.h"\n'
            '/* PUBLIC TEST keys only, private fixture entry. */\n'
            'static const fpk_catalog_policy p={.repository='+cbytes(bytes(range(16)))+
            ',.keys={'+','.join(cbytes(k) for k in public)+'},.key_count=3,.threshold=2,.root_version=1};\n'
            'int pkg_test_main(int argc,char **argv){return pkg_cli(argc,argv,&p);}\n')
        run(['gcc', '-Os', '-Wall', '-Wextra', '-Werror', '-ffreestanding', '-fno-builtin',
             '-fno-pie', '-fno-stack-protector', '-mno-red-zone', '-mno-sse', '-mno-mmx',
             '-msoft-float', '-Wframe-larger-than=1024', *INCLUDES, '-c', str(wrapper),
             '-o', 'build/pkg-cli/test-policy.o'], work, log)
        run(['nasm', '-f', 'elf64', '-DTOOL_ENTRY=pkg_test_main', 'user/tools/start.asm',
             '-o', 'build/pkg-cli/test-start.o'], work, log)
        objects = sorted(str(p) for p in (work/'build/pkg-cli').glob('unit-*.o'))
        run(['ld', '-nostdlib', '-static', '-z', 'noexecstack', '--gc-sections', '-T',
             'user/shell.ld', 'build/pkg-cli/test-start.o', 'build/pkg-cli/test-policy.o',
             *objects, '-o', 'build/pkg-cli/pkg-test.elf'], work, log)
        stage = work/'build/initramfs'
        shutil.copyfile(work/'build/pkg-cli/pkg.elf', stage/'bin/pkg')
        shutil.copyfile(work/'build/pkg-cli/pkg-test.elf', stage/'bin/pkg-test')
        fixture = work/'build/pkg-format/fixture.fpk'
        shutil.copyfile(fixture, stage/'docs/fixture.fpk')
        bad = bytearray(fixture.read_bytes()); bad[-1] ^= 1
        (stage/'docs/bad.fpk').write_bytes(bad)
        (stage/'docs/catalog').write_bytes(good)
        bad = bytearray(good); bad[-1] ^= 1
        (stage/'docs/bad-catalog').write_bytes(bad)
        if prepare:
            manifest['extra_checks'] = prepare(work, log, stage)
        run(['python3', 'scripts/create_initramfs.py', str(stage), 'bin/initramfs.tar'], work, log)
        run(['make', 'iso'], work, log)
    campaign(out, work, manifest)


def campaign(out, work, manifest):
    sources, media = manifest['sources'], manifest['shared_media']
    fixture = work/'build/pkg-format/fixture.fpk'
    manifest['private_iso'] = sha(work/'bin/fortress.iso')
    manifest['default_cli'] = sha(work/'build/pkg-cli/pkg.elf')
    manifest['test_cli'] = sha(work/'build/pkg-cli/pkg-test.elf')
    manifest['runner_sha256'] = sha(Path(__file__))
    (out/'manifest.json').write_text(json.dumps(manifest, indent=2))
    length, digest = fixture.stat().st_size, sha(fixture)
    commands = [
        ('/bin/pkg --help', 'Installation is unavailable.'),
        (f'/bin/pkg inspect /docs/fixture.fpk {length} {digest}', 'integrity OK; signatures not checked'),
        ('/bin/pkg verify /docs/catalog demo /docs/fixture.fpk', 'no provisioned catalog trust policy'),
        ('/bin/pkg-test verify /docs/catalog demo /docs/fixture.fpk', 'catalog policy verified; platform/install admission not checked'),
        ('/bin/pkg-test verify /docs/bad-catalog demo /docs/fixture.fpk', 'catalog rejected or package absent'),
        ('/bin/pkg-test verify /docs/catalog missing /docs/fixture.fpk', 'catalog rejected or package absent'),
        ('/bin/pkg-test verify /docs/catalog demo /docs/bad.fpk', 'verification failed'),
        (f'/bin/pkg inspect /docs/bad.fpk {length} {digest}', 'verification failed'),
        (f'/bin/pkg inspect /bin {length} {digest}', 'cannot open regular artifact'),
        ('/bin/pkg install x x x', 'unsupported command'),
    ] + [tuple(c) for c in manifest.get('extra_checks', [])]
    results = [case(out, work, fw, smp, commands) for fw in ('bios', 'uefi') for smp in (1, 4)]
    changed = [name for name, digest in sources.items() if not (ROOT/name).is_file() or sha(ROOT/name) != digest]
    manifest['concurrent_source_changes'] = changed
    manifest['changed_shared_media'] = {
        name: {'before': digest, 'after': sha(ROOT/name) if (ROOT/name).is_file() else None}
        for name, digest in media.items() if not (ROOT/name).is_file() or sha(ROOT/name) != digest}
    manifest['source_preservation'] = 'FAIL' if changed else 'PASS'
    manifest['media_preservation'] = 'FAIL' if manifest['changed_shared_media'] else 'PASS'
    manifest['preservation'] = 'FAIL' if changed or manifest['changed_shared_media'] else 'PASS'
    manifest['head_after'] = subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()
    manifest['status_after'] = subprocess.check_output(['git','status','--short'],cwd=ROOT,text=True)
    (out/'manifest.json').write_text(json.dumps(manifest, indent=2))
    (out/'summary.json').write_text(json.dumps(results, indent=2))
    assert manifest['preservation']=='PASS', 'preservation guard changed; boot results retained separately'
    print('PASS 4/4 private PKG CLI boots; artifacts:', out, flush=True)


if __name__ == '__main__':
    import sys
    if len(sys.argv) == 3 and sys.argv[1] == '--replay':
        previous = Path(sys.argv[2])
        manifest = json.loads((previous/'manifest.json').read_text())
        out = ROOT/'.codex-remote-attachments'/('pkg-cli-qemu-'+time.strftime('%Y%m%d-%H%M%S'))
        out.mkdir(parents=True)
        manifest['replay_of'] = str(previous)
        campaign(out, Path(manifest['workspace']), manifest)
    else:
        assert len(sys.argv) == 1, 'usage: test_pkg_cli_qemu.py [--replay ARTIFACT_DIR]'
        main()
