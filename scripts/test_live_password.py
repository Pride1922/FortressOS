#!/usr/bin/env python3
"""Disk-free live password + runtime host-key SSH acceptance; no secrets retained."""
import os,re,selectors,shutil,subprocess,tempfile,time
from pathlib import Path
import test_ssh as fixture

def case(firmware):
    with tempfile.TemporaryDirectory(prefix='fortress-live-password-') as tmp:
        tmp=Path(tmp);port=fixture.find_free_port()
        argv=['qemu-system-x86_64','-M','q35','-m','512M','-smp','1','-cpu','max,+rdrand,+rdseed',
              '-accel','tcg','-display','none','-monitor','none','-no-reboot','-serial','stdio',
              '-boot','d','-cdrom',str(fixture.ISO),'-netdev',f'user,id=n,hostfwd=tcp:127.0.0.1:{port}-:22',
              '-device','e1000,netdev=n']
        if firmware=='uefi':
            var=tmp/'vars.fd';shutil.copyfile(fixture.VARS,var)
            argv+=['-drive',f'if=pflash,format=raw,unit=0,readonly=on,file={fixture.CODE}',
                   '-drive',f'if=pflash,format=raw,unit=1,file={var}']
        p=subprocess.Popen(argv,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL)
        sel=selectors.DefaultSelector();sel.register(p.stdout,selectors.EVENT_READ);data=bytearray()
        password='Test8492'
        def send(s):p.stdin.write((s+'\n').encode());p.stdin.flush()
        def wait(needle,start=0,timeout=100):
            end=time.monotonic()+timeout
            while time.monotonic()<end:
                for key,_ in sel.select(.1):data.extend(key.fileobj.read1(65536))
                text=re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]','',data[start:].decode(errors='replace')).replace('\r','')
                if needle in text:return text
                assert p.poll() is None,'guest exited'
            raise AssertionError('timeout waiting for '+needle+'; tail='+text[-1600:])
        try:
            wait('FortressOS login: ');send('operator');wait('$ ')
            start=len(data);send('sudo /bin/passwd --live operator')
            wait('New password (8..128 characters): ',start);send(password)
            wait('Confirm new password: ',start);send(password)
            wait('Temporary operator password set',start)
            assert password.encode() not in data,'password echoed on serial'
            start=len(data);send('sudo /sbin/ssh-keygen --live')
            wait('sudo password: ',start);send(password);wait('SHA256:',start)
            start=len(data);send('sudo -b /sbin/sshd')
            wait('sudo password: ',start);send(password);wait('Listening on port 22',start)
            ask=tmp/'askpass';ask.write_text('#!/bin/sh\nprintf "%s\\n" "$LIVE_TEST_PASSWORD"\n');ask.chmod(0o700)
            env=dict(os.environ,SSH_ASKPASS=str(ask),SSH_ASKPASS_REQUIRE='force',DISPLAY=':99',LIVE_TEST_PASSWORD=password)
            command=['ssh','-p',str(port),'-o','StrictHostKeyChecking=no','-o','UserKnownHostsFile=/dev/null',
                     '-o','PreferredAuthentications=password','-o','PubkeyAuthentication=no','-o','NumberOfPasswordPrompts=1',
                     'operator@127.0.0.1','whoami']
            result=subprocess.run(command,stdin=subprocess.DEVNULL,capture_output=True,env=env,timeout=30)
            assert result.returncode==0 and b'operator' in result.stdout,'SSH password authentication failed'
            env['LIVE_TEST_PASSWORD']='incorrect-test-password'
            result=subprocess.run(command,stdin=subprocess.DEVNULL,capture_output=True,env=env,timeout=30)
            assert result.returncode!=0,'wrong password accepted'
            assert password.encode() not in data,'password echoed'
            print('PASS '+firmware+': live password, hidden input, sudo reload, RAM host key, OpenSSH login, wrong-password rejection',flush=True)
        finally:
            p.terminate()
            try:p.wait(timeout=5)
            except subprocess.TimeoutExpired:p.kill();p.wait()
            sel.close()

if __name__=='__main__':
    for firmware in ('bios','uefi'):case(firmware)
