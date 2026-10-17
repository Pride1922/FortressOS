#!/usr/bin/env python3
"""Retained, disk-free BIOS/UEFI x SMP=1/4/8 strong-random acceptance.

QEMU emulates entropy, not physical entropy quality. Ordinary unpinned spawned
workers report CPUID APIC IDs beside samples; host pthreads prove concurrency.
No test seed or guest observer is installed in the release image.
"""
import concurrent.futures
import hashlib
import json
from pathlib import Path
import re
import shutil
import socket
import subprocess
import tempfile
import threading
import time

REPO = Path(__file__).resolve().parent.parent
CODE = Path('/usr/share/OVMF/OVMF_CODE_4M.fd')
VARS = Path('/usr/share/OVMF/OVMF_VARS_4M.fd')
PROMPT = re.compile(r'(?:fortress> |(?:\[-?\d+\] )?fortress:[^\n]* \$ |[A-Za-z0-9_-]+@[A-Za-z0-9-]+:[^\n]*[#$] )\s*\Z')

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def run_case(root, iso, firmware, smp, source):
    name = f'{firmware}-smp{smp}-{source}'
    dest = root / name
    dest.mkdir()
    with tempfile.TemporaryDirectory(prefix='fortress-rng-') as tmp:
        tmp = Path(tmp)
        serial = tmp / 'uart'
        cpu = {'both': 'max,+rdrand,+rdseed', 'rdrand': 'max,+rdrand,-rdseed',
               'absent': 'max,-rdrand,-rdseed'}[source]
        argv = ['qemu-system-x86_64', '-M', 'q35', '-accel', 'tcg', '-cpu', cpu,
                '-m', '512M', '-smp', str(smp), '-display', 'none', '-net', 'none',
                '-no-reboot', '-boot', 'd', '-cdrom', str(iso),
                '-chardev', f'socket,id=uart,path={serial},server=on,wait=off,logfile={dest / "serial.log"}',
                '-serial', 'chardev:uart']
        if firmware == 'uefi':
            assert CODE.is_file() and VARS.is_file(), 'paired OVMF required'
            shutil.copyfile(VARS, dest / 'vars.fd')
            argv += ['-drive', f'if=pflash,format=raw,unit=0,readonly=on,file={CODE}',
                     '-drive', f'if=pflash,format=raw,unit=1,file={dest / "vars.fd"}']
        # Final argv whitelist: no data disks, physical devices or hidden configs.
        assert '-nodefaults' not in argv and '-hda' not in argv
        for i, value in enumerate(argv):
            if value == '-drive':
                assert argv[i+1].startswith('if=pflash,format=raw,unit=')
        (dest / 'argv.json').write_text(json.dumps(argv, indent=2))
        err = (dest / 'qemu.stderr').open('w')
        proc = subprocess.Popen(argv, cwd=REPO, stderr=err)
        uart = socket.socket(socket.AF_UNIX)
        stop = threading.Event()
        drain_thread = None
        def text():
            p = dest / 'serial.log'
            raw = p.read_text(errors='replace') if p.exists() else ''
            return re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]', '', raw).replace('\r', '')
        def wait(predicate, timeout=90):
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                data = text()
                if predicate(data):
                    return data
                assert proc.poll() is None, f'{name}: QEMU exited {proc.returncode}'
                time.sleep(.05)
            raise AssertionError(f'{name}: timeout\n{text()[-2000:]}')
        try:
            deadline = time.monotonic()+10
            while True:
                try:
                    uart.connect(str(serial)); break
                except (FileNotFoundError, ConnectionRefusedError):
                    assert time.monotonic()<deadline
                    time.sleep(.05)
            uart.settimeout(.2)
            def drain():
                while not stop.is_set():
                    try:
                        if not uart.recv(65536): return
                    except socket.timeout:
                        pass
                    except OSError:
                        return
            drain_thread = threading.Thread(target=drain, daemon=True)
            drain_thread.start()
            wait(lambda s: 'FortressOS login: ' in s or PROMPT.search(s))
            if not PROMPT.search(text()):
                uart.sendall(b'operator\n')
            wait(lambda s: PROMPT.search(s))
            def command(cmd, marker):
                pos = len(text())
                uart.sendall(cmd.encode()+b'\n')
                return wait(lambda s: marker in s[pos:] and PROMPT.search(s[pos:]))[pos:]
            if source == 'absent':
                command('/bin/getrandom-probe --unavailable', 'RANDOM unavailable PASS')
                assert '[RANDOM] unavailable;' in text()
                samples = []
            else:
                command('/bin/getrandom-probe', 'RANDOM probe PASS')
                command('/bin/getrandom-probe --workers', 'RANDOM workers PASS')
                samples = re.findall(r'RANDOM sample cpu=(\d+) ([0-9a-f]{64})', text())
                assert len(samples)==258, len(samples)
                assert len({s for _, s in samples})==len(samples)
                assert all(int(s,16) for _, s in samples)
                cpus = sorted({int(c) for c,_ in samples})
                if smp>1:
                    assert len(cpus)>1, f'No AP execution observed: {cpus}'
            # Prove the shell survives success/failure and has finite shutdown.
            command('echo RANDOM_RECOVERY_OK', 'RANDOM_RECOVERY_OK')
            uart.sendall(b'sudo shutdown\n')
            proc.wait(timeout=20)
            assert proc.returncode==0
            result = {'case': name, 'status': 'PASS', 'samples': len(samples),
                      'cpus': sorted({int(c) for c,_ in samples})}
            (dest / 'result.json').write_text(json.dumps(result, indent=2))
            print(json.dumps(result), flush=True)
            return result, [s for _,s in samples]
        except BaseException as exc:
            (dest / 'failure.txt').write_text(repr(exc))
            raise
        finally:
            stop.set()
            uart.close()
            if drain_thread:
                drain_thread.join(timeout=1)
            if proc.poll() is None:
                proc.terminate()
                try: proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill(); proc.wait(timeout=5)
            err.close()

def main():
    root = REPO / '.codex-remote-attachments' / ('random-' + time.strftime('%Y%m%d-%H%M%S'))
    root.mkdir(parents=True)
    iso = root / 'fortress.iso'
    shutil.copyfile(REPO / 'bin/fortress.iso', iso)
    protected = [REPO / p for p in ('src/kernel/accounts.c', 'src/net/net_tcp.c',
                  'src/kernel/time.c', 'src/arch/x86_64/syscall_entry.asm')]
    before = {str(p.relative_to(REPO)): sha(p) for p in protected}
    metadata = {'iso_sha256': sha(iso), 'protected_sha256': before,
                'source_sha256': {p: sha(REPO / p) for p in (
                    'src/kernel/random.c','src/lib/hmac_drbg.c','src/lib/hmac_drbg.h',
                    'src/include/random.h','src/include/syscall_abi.h',
                    'src/kernel/syscall.c','src/kernel/main.c','user/tools/digest.c',
                    'user/tools/getrandom-probe.c','scripts/test_getrandom.py')},
                'compiler': subprocess.check_output(['gcc','--version'],text=True),
                'qemu': subprocess.check_output(['qemu-system-x86_64','--version'],text=True),
                'source_quality': 'emulated; no physical entropy assurance'}
    (root / 'manifest.json').write_text(json.dumps(metadata, indent=2))
    cases = [(f,n,s) for f in ('bios','uefi') for n in (1,4,8) for s in ('both','absent')]
    cases += [(f,4,'rdrand') for f in ('bios','uefi')]
    results, all_samples = [], []
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            futures = [pool.submit(run_case,root,iso,*c) for c in cases]
            for future in concurrent.futures.as_completed(futures):
                result,samples = future.result()
                results.append(result); all_samples += samples
        assert len(all_samples)==len(set(all_samples)), 'repeated stream across boots'
        assert before=={str(p.relative_to(REPO)): sha(p) for p in protected}
        assert sha(iso)==metadata['iso_sha256']
        (root / 'summary.json').write_text(json.dumps(results,indent=2))
        print(f'PASS {len(results)}/{len(cases)} boots; evidence: {root}',flush=True)
    finally:
        print(f'Artifacts retained: {root}',flush=True)

if __name__=='__main__':
    main()
