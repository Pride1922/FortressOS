#!/usr/bin/env python3
"""Disk-free real Watchtower lifecycle + OpenSSH teardown acceptance.

Keys/passwords live only in disposable guest RAM; no secret-bearing disks/logs.
Existing release ISO is read-only. No physical devices are attached.
"""
from pathlib import Path
import argparse,hashlib,os,re,selectors,shutil,socket,subprocess,tempfile,time

ROOT=Path(__file__).resolve().parent.parent
PASSWORD='Lifecycle8492'

def free_port():
    with socket.socket() as s:s.bind(('127.0.0.1',0));return s.getsockname()[1]

class Guest:
    def __init__(self,firmware,smp,tmp,reboot):
        self.port=free_port();self.data=bytearray();self.clients=[]
        args=['qemu-system-x86_64','-M','q35','-m','512M','-smp',str(smp),
              '-accel','tcg','-cpu','max,+rdrand,+rdseed','-display','none',
              '-monitor','none','-serial','stdio','-boot','d','-cdrom',str(ROOT/'bin/fortress.iso'),
              '-netdev',f'user,id=n,hostfwd=tcp:127.0.0.1:{self.port}-:22','-device','e1000e,netdev=n']
        if not reboot:args+=['-no-reboot']
        if firmware=='uefi':
            variables=tmp/'vars.fd';shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
            args+=['-drive','if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
                   '-drive',f'if=pflash,format=raw,unit=1,file={variables}']
        self.proc=subprocess.Popen(args,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL)
        self.sel=selectors.DefaultSelector();self.sel.register(self.proc.stdout,selectors.EVENT_READ)
        ask=tmp/'askpass';ask.write_text('#!/bin/sh\nprintf "%s\\n" "$LIFECYCLE_PASSWORD"\n');ask.chmod(0o700)
        self.env=dict(os.environ,SSH_ASKPASS=str(ask),SSH_ASKPASS_REQUIRE='force',DISPLAY=':99',LIFECYCLE_PASSWORD=PASSWORD)
    def pump(self):
        for key,_ in self.sel.select(.05):self.data.extend(key.fileobj.read1(65536))
    def text(self,start=0):
        return re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]','',self.data[start:].decode(errors='replace')).replace('\r','')
    def wait(self,predicate,start=0,timeout=90):
        end=time.monotonic()+timeout
        while time.monotonic()<end:
            self.pump();text=self.text(start)
            assert PASSWORD.encode() not in self.data,'password echoed'
            assert not re.search(r'\[PROCESS FAULT\][\s\S]{0,200}\(/(?:sbin/watchtower|bin/service|sbin/crond|sbin/sshd)\)',self.text()),'real daemon fault'
            if predicate(text):return text
            assert self.proc.poll() is None,'guest exited unexpectedly: '+text[-1500:]
        raise AssertionError('guest timeout: '+text[-1800:])
    def send(self,text):self.proc.stdin.write((text+'\n').encode());self.proc.stdin.flush()
    def command(self,text,timeout=30):
        start=len(self.data);self.send(text)
        result=self.wait(lambda s:('sudo password: ' in s) or ('\n' in s and s.endswith('$ ')),start,timeout)
        if 'sudo password: ' in result:
            self.send(PASSWORD);result=self.wait(lambda s:s.endswith('$ '),start,timeout)
        return result.split('\n',1)[1]
    def status(self,name):return self.command('sudo service status '+name)
    def running(self,name,previous=None,timeout=25):
        end=time.monotonic()+timeout;last=''
        while time.monotonic()<end:
            last=self.status(name);m=re.search(r'State: running \(PID: (\d+)\)',last)
            if m and (previous is None or int(m[1])!=previous):return int(m[1])
            self.command('sleep 0.2')
        raise AssertionError('service not running: '+last)
    def stopped(self,name):
        end=time.monotonic()+15
        while time.monotonic()<end:
            text=self.status(name)
            if 'State: stopped' in text:return
            self.command('sleep 0.2')
        raise AssertionError('service not stopped: '+text)
    def ssh(self,command,hold=False,ready=b'SSH_HOLD_READY'):
        args=['ssh','-vv','-tt','-p',str(self.port),'-o','ConnectTimeout=8',
              '-o','StrictHostKeyChecking=no','-o','UserKnownHostsFile=/dev/null',
              '-o','PreferredAuthentications=password','-o','PubkeyAuthentication=no',
              '-o','NumberOfPasswordPrompts=1','operator@127.0.0.1']
        if command is not None:args.append(command)
        client=subprocess.Popen(args,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=self.env)
        self.clients.append(client)
        if not hold:
            out,err=client.communicate(timeout=30)
            assert client.returncode==0,(out+err).decode(errors='replace')[-1500:]
            return out
        self.client_wait(client,ready)
        return client
    def client_wait(self,client,ready):
        sel=selectors.DefaultSelector();sel.register(client.stdout,selectors.EVENT_READ);out=b''
        try:
            end=time.monotonic()+20
            while ready not in out:
                assert time.monotonic()<end and client.poll() is None,'SSH hold failed'
                for key,_ in sel.select(.1):out+=key.fileobj.read1(4096)
        finally:sel.close()
        return out
    def disconnected(self,client):
        start=time.monotonic()
        try:out,err=client.communicate(timeout=15)
        except subprocess.TimeoutExpired as failure:
            self.pump()
            output=(failure.stdout or b'')+(failure.stderr or b'')
            print('DISCONNECT TIMEOUT guest tail: '+self.text()[-2200:].replace(PASSWORD,'[redacted]'),flush=True)
            print('CLIENT tail: '+output[-1500:].decode(errors='replace').replace(PASSWORD,'[redacted]'),flush=True)
            raise
        assert b'Received disconnect' in err,(out+err).decode(errors='replace')[-1800:]
        assert time.monotonic()-start<15,'disconnect exceeded bound'
    def close(self):
        for client in self.clients:
            if client.poll() is None:client.kill();client.communicate()
        if self.proc.poll() is None:self.proc.terminate()
        try:self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:self.proc.kill();self.proc.wait()
        self.sel.close()

def case(firmware,smp):
    end_action='reboot' if smp==4 else 'shutdown'
    with tempfile.TemporaryDirectory(prefix='fortress-watchtower-lifecycle-') as tmp:
        g=Guest(firmware,smp,Path(tmp),end_action=='reboot')
        def progress(message):print(f'[{firmware} SMP={smp}] {message}',flush=True)
        try:
            g.wait(lambda s:s.endswith('FortressOS login: '));g.send('operator');g.wait(lambda s:s.endswith('$ '))
            text=g.command('sudo service list')
            if 'supervisor is not running' in text:g.command('sudo -b /sbin/watchtower')
            original=g.running('crond');g.command('sudo service stop crond');g.stopped('crond')
            g.command('sleep 2');g.stopped('crond')
            g.command('sudo service start crond');pid=g.running('crond',original)
            g.command('sudo service restart crond');pid=g.running('crond',pid)
            g.command(f'sudo kill -KILL {pid}');pid=g.running('crond',pid)
            progress('PASS service stop stays stopped, start/restart PID change, crash recovery')
            for expected in (2,3):
                g.command(f'sudo kill -KILL {pid}');pid=g.running('crond',pid)
                assert f'restarts: {expected}/3' in g.status('crond')
            g.command(f'sudo kill -KILL {pid}')
            deadline=time.monotonic()+15
            while time.monotonic()<deadline:
                status=g.status('crond')
                if 'State: failed' in status:break
                g.command('sleep 0.2')
            assert 'State: failed' in status and 'restarts: 3/3' in status,status
            g.command('sleep 2');assert 'State: failed' in g.status('crond')
            g.command('sudo service restart crond');pid=g.running('crond',pid)
            assert 'restarts: 0/3' in g.status('crond')
            g.command(f'sudo kill -STOP {pid}');g.command('sudo service stop crond');g.stopped('crond')
            assert 'signaled (signal 9)' in g.status('crond'),'stop timeout failed to escalate'
            g.command('sudo service start crond');g.running('crond',pid)
            progress('PASS restart budget, failed-state stability, manual reset, stopped-child timeout escalation')
            start=len(g.data);g.send('sudo /bin/passwd --live operator')
            g.wait(lambda s:'New password (8..128 characters): ' in s,start);g.send(PASSWORD)
            g.wait(lambda s:'Confirm new password: ' in s,start);g.send(PASSWORD)
            g.wait(lambda s:'Temporary operator password set' in s and s.endswith('$ '),start)
            assert 'SHA256:' in g.command('sudo /sbin/ssh-keygen --live')
            g.command('sudo service restart sshd');g.running('sshd')
            assert b'operator' in g.ssh('whoami')
            client=g.ssh('echo SSH_HOLD_READY; sleep 60',hold=True)
            g.command('sudo service stop sshd');g.disconnected(client);g.stopped('sshd')
            assert '/sbin/sshd' not in g.command('ps'),'SSH monitor/worker remains after stop'
            g.command('sudo service start sshd');old=g.running('sshd')
            assert b'operator' in g.ssh('whoami')
            client=g.ssh('echo SSH_HOLD_READY; sleep 60',hold=True)
            g.command('sudo service restart sshd');g.disconnected(client);g.running('sshd',old)
            assert b'operator' in g.ssh('whoami')
            progress('PASS supervised SSH stop/restart, wire disconnect, authenticated reconnect')
            # Interactive sudo is fed via SSH PTY; authentication remains askpass.
            client=g.ssh(None,hold=True,ready=b'$ ')
            client.stdin.write(('sudo /sbin/'+end_action+'\n').encode());client.stdin.flush()
            g.client_wait(client,b'sudo password: ')
            start=len(g.data);client.stdin.write((PASSWORD+'\n').encode());client.stdin.flush()
            g.disconnected(client)
            if end_action=='shutdown':
                end=time.monotonic()+30
                while g.proc.poll() is None and time.monotonic()<end:g.pump()
                assert g.proc.poll()==0,'poweroff did not terminate QEMU cleanly'
            else:
                g.wait(lambda s:s.endswith('FortressOS login: '),start,120)
                g.send('operator');g.wait(lambda s:s.endswith('$ '),start)
                assert 'POST_REBOOT_OK' in g.command('echo POST_REBOOT_OK')
            progress('PASS remote '+end_action+' disconnect and '+('new boot/login' if end_action=='reboot' else 'poweroff'))
        finally:g.close()

if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--firmware',nargs='+',choices=['bios','uefi'],default=['bios','uefi'])
    parser.add_argument('--smp',type=int,nargs='+',choices=[1,4],default=[1,4]);args=parser.parse_args()
    inputs=[ROOT/'bin/fortress.img',ROOT/'bin/fortress.iso',ROOT/'bin/initramfs.tar']
    digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
    before={str(p):digest(p) for p in inputs}
    for firmware in args.firmware:
        for smp in args.smp:case(firmware,smp)
    assert before=={str(p):digest(p) for p in inputs},'release inputs changed'
    print('PASS release-image preservation; no secret-bearing guest storage retained',flush=True)
