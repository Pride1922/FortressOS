#!/usr/bin/env python3
"""Real kernel supervisor BLOCKED/idle and shell-exit restart regression.
Read-only GDB scheduler inspection; disposable ISO/OVMF, no data disks.
"""
import re
import shutil
import socket
import struct
import subprocess
import tempfile
import threading
import time
from pathlib import Path
from test_net_eth import build_iso
from test_net_pci import REPO, CODE, VARS
from test_nmi_transitions import Remote, QMP, symbols, connect
from test_shell import offsets, scheduler_symbols

def run(mode, cpus):
    with tempfile.TemporaryDirectory(prefix='fortress-supervisor-') as directory:
        tmp=Path(directory); iso=build_iso(tmp, '')
        state,ticks,_,channel,_,_,_,_,name,nxt=offsets(tmp)
        log=REPO/'build'/f'supervisor-{mode}-{cpus}.log'
        error=log.with_suffix('.stderr.log')
        uart_path,qmp_path,gdb_path=[tmp/n for n in ('uart','qmp','gdb')]
        cmd=['qemu-system-x86_64','-M','q35','-m','2G','-smp',str(cpus),
             '-display','none','-monitor','none','-no-reboot','-S','-boot','d',
             '-cdrom',str(iso),'-net','none','-chardev',
             f'socket,id=uart,path={uart_path},server=on,wait=off,logfile={log}',
             '-serial','chardev:uart','-qmp',f'unix:{qmp_path},server=on,wait=off',
             '-gdb',f'unix:{gdb_path},server=on,wait=off']
        if mode=='uefi':
            variables=tmp/'vars.fd';shutil.copyfile(VARS,variables)
            cmd+=['-drive',f'if=pflash,format=raw,unit=0,readonly=on,file={CODE}',
                  '-drive',f'if=pflash,format=raw,unit=1,file={variables}']
        expected=tuple(cmd)
        def preflight(candidate): assert tuple(candidate)==expected,'unauthorized QEMU/storage argv'
        preflight(cmd)
        try: preflight(cmd+['-drive','file=unsafe.img'])
        except AssertionError: pass
        else: raise AssertionError('preflight did not reject data disk')
        with error.open('wb') as errors:
            proc=subprocess.Popen(cmd,cwd=REPO,stdout=subprocess.DEVNULL,stderr=errors)
            uart=None; stop=threading.Event()
            try:
                uart=connect(uart_path);uart.settimeout(.2)
                def drain():
                    while not stop.is_set():
                        try:
                            if not uart.recv(65536):return
                        except socket.timeout:pass
                reader=threading.Thread(target=drain,daemon=True);reader.start()
                qmp=QMP(qmp_path); remote=Remote(gdb_path);sym=symbols()
                remote.request('qSupported');qmp.execute('cont')
                def output():
                    return re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]','',log.read_text(errors='replace')).replace('\r','')
                def wait(predicate,timeout=60):
                    end=time.monotonic()+timeout
                    while time.monotonic()<end:
                        if predicate(output()):return
                        assert proc.poll() is None,error.read_text()
                        time.sleep(.03)
                    raise AssertionError(output()[-3000:])
                def supervisor():
                    qmp.execute('stop')
                    try:
                        scheduler_symbols(remote,sym)
                        node=struct.unpack('<Q',remote.memory(sym['g_blocked_threads'],8))[0]
                        for _ in range(128):
                            if not node:break
                            if remote.memory(node+name,32).split(b'\0')[0]==b'main':
                                assert struct.unpack('<I',remote.memory(node+state,4))[0]==2
                                assert struct.unpack('<Q',remote.memory(node+channel,8))[0]
                                return struct.unpack('<Q',remote.memory(node+ticks,8))[0]
                            node=struct.unpack('<Q',remote.memory(node+nxt,8))[0]
                        raise AssertionError('main supervisor must be BLOCKED')
                    finally:qmp.execute('cont')
                wait(lambda t: re.search(r'fortress:[^\n]* \$ ',t))
                time.sleep(.2);before=supervisor();time.sleep(1);assert supervisor()==before
                for restart in range(1,4):
                    for byte in b'exit\n':uart.sendall(bytes([byte]));time.sleep(.01)
                    wait(lambda t:t.count('[BOOT] Interactive shell ready.')>=restart+1 and
                         re.search(r'fortress:[^\n]* \$ ',t[t.rfind('[BOOT] Interactive shell ready.'):]))
                    time.sleep(.15);supervisor()
                print(f'PASS supervisor {mode} SMP={cpus}: BLOCKED, zero idle tick growth, three exit/restarts',flush=True)
            finally:
                stop.set()
                if proc.poll() is None:proc.terminate()
                try:proc.wait(timeout=10)
                except subprocess.TimeoutExpired:proc.kill();proc.wait(timeout=10)
                if uart:uart.close()

if __name__=='__main__':
    for cpus in (1,4):
        for mode in ('bios','uefi'):run(mode,cpus)
