#!/usr/bin/env python3
"""Real live-media supervisor/CLI smoke; no data disks or private keys."""
from pathlib import Path
import re,selectors,shutil,subprocess,tempfile,time

ROOT=Path(__file__).resolve().parent.parent

def boot(mode,smp,tmp):
    argv=['qemu-system-x86_64','-M','q35','-m','512M','-smp',str(smp),
          '-accel','tcg','-cpu','max,+rdrand,+rdseed','-display','none',
          '-monitor','none','-serial','stdio','-no-reboot','-boot','d',
          '-cdrom',str(ROOT/'bin/fortress.iso'),'-netdev','user,id=n0',
          '-device','e1000e,netdev=n0']
    if mode=='uefi':
        variables=tmp/f'vars-{smp}.fd'
        shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
        argv+=['-drive','if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
               '-drive',f'if=pflash,format=raw,unit=1,file={variables}']
    proc=subprocess.Popen(argv,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL)
    selector=selectors.DefaultSelector();selector.register(proc.stdout,selectors.EVENT_READ)
    data=bytearray()
    def read_until(predicate,start=0,timeout=90):
        end=time.monotonic()+timeout
        while time.monotonic()<end:
            for key,_ in selector.select(.1):data.extend(key.fileobj.read1(65536))
            text=re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]','',data[start:].decode(errors='replace')).replace('\r','')
            # Early kernel acceptance intentionally faults isolated test users.
            # Reject faults in the real daemons/control CLI under this test.
            assert not re.search(r'\[PROCESS FAULT\][\s\S]{0,200}\(/(?:sbin/watchtower|bin/service|sbin/crond)\)',
                                 data.decode(errors='replace')),data[-3000:].decode(errors='replace')
            if predicate(text):return text
            assert proc.poll() is None,text[-3000:]
        raise AssertionError('timeout: '+text[-3000:])
    def send(text):proc.stdin.write(text.encode());proc.stdin.flush()
    def command(text):
        start=len(data);send(text+'\n')
        result=read_until(lambda s:'\n' in s and s.endswith('$ '),start,30)
        return result.split('\n',1)[1]
    try:
        read_until(lambda s:s.endswith('FortressOS login: '));send('operator\n')
        read_until(lambda s:s.endswith('$ '))
        result=command('sudo service list')
        if 'supervisor is not running' in result:
            command('sudo -b /sbin/watchtower')
            result=command('sudo service list')
        assert 'dhcpc' in result and 'sshd' in result and 'crond' in result,result
        assert 'not running' not in result and 'timed out' not in result,result
        result=command('sleep 2; sudo service list')
        assert 'crond' in result and 'timed out' not in result,result
        result=command('echo WATCHTOWER_BOOT_OK')
        assert 'WATCHTOWER_BOOT_OK' in result,result
        print(f'PASS {mode} SMP={smp}: real boot, supervisor, repeated CLI, pipe drain, shell alive',flush=True)
    finally:
        proc.terminate()
        try:proc.wait(timeout=5)
        except subprocess.TimeoutExpired:proc.kill();proc.wait()
        selector.close()

if __name__=='__main__':
    with tempfile.TemporaryDirectory(prefix='fortress-watchtower-boot-') as tmp:
        for mode in ('bios','uefi'):
            for smp in (1,4):boot(mode,smp,Path(tmp))
