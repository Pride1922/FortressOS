#!/usr/bin/env python3
"""Phase3 extends the audited isolated Phase2 NVMe runner. No physical paths,
installer media or raw writes. Ephemeral chosen-test passwords live in memory.
"""
import secrets,re,tempfile
from pathlib import Path
import test_root_phase2 as gate
NEW=secrets.token_hex(16)
USER=secrets.token_hex(16)
real_guest=gate.guest
def guest(*args,**kwargs):
    values=list(args)
    if values[4] in (1,2):values[5]=NEW
    return real_guest(*values,**kwargs)
def host(out,root,run,manifest):
    run(['make','test-account-redo-host'],300)
    binary=out/'account-ext4-host'
    run(['gcc','-std=c11','-O1','-g','-fsanitize=address,undefined','-Wall','-Wextra','-Werror','-no-pie','-pthread',
         '-Itests/ext4_host','-Itests/host','-Isrc/include','-Isrc/fs','-Isrc/drivers','-Isrc/mm','-Isrc/kernel','-Isrc/arch/x86_64',
         'tests/account_ext4_host.c','tests/ext4_fault_disk.c','src/lib/account_redo.c','user/tools/userdb.c','user/tools/digest.c','-o',str(binary)])
    with tempfile.TemporaryDirectory(prefix='fortress-accounts-',dir='/tmp') as directory:
        native=Path(directory);baseline=native/'baseline.img'
        run(['cp','--sparse=always',str(root),str(baseline)])
        def account_bytes(image):
            values=[]
            for name in ('passwd','group','shadow'):
                target=native/name
                run(['debugfs','-R',f'dump /etc/{name} {target}',str(image)])
                values.append(target.read_bytes());target.unlink()
            return tuple(values)
        old=account_bytes(baseline)
        for sector in (512,4096):
            disk=native/f'accounts-host-{sector}.img';interrupted=native/f'account-cut-{sector}.img'
            try:
                run(['cp','--sparse=always',str(baseline),str(disk)])
                result=run([str(binary),str(disk),str(sector)],600);run(['e2fsck','-fn',str(disk)])
                new=account_bytes(disk)
                boundaries=int(re.search(r'ACCOUNT_EVENTS flush=(\d+)',result)[1])
                for boundary in range(boundaries):
                    for after in (False,True):
                        run(['cp','--sparse=always',str(baseline),str(interrupted)])
                        env=['env',f'ACCOUNT_FLUSH_CUT={boundary}']
                        if after:env.append('ACCOUNT_CUT_AFTER=1')
                        run(env+[str(binary),str(interrupted),str(sector)],600)
                        run(['env','ACCOUNT_RECOVER_ONLY=1',str(binary),str(interrupted),str(sector)],600)
                        run(['e2fsck','-fn',str(interrupted)])
                        recovered=account_bytes(interrupted);assert recovered in (old,new),'mixed account set after restart'
                        manifest['cases'].append({'account_flush_cut':boundary,'after':after,'sector':sector,'set':'old' if recovered==old else 'new','result':'PASS'})
                    if boundary%25==0:print(f'PASS account recovery sector={sector} flush={boundary}/{boundaries}',flush=True)
                run(['cp','--sparse=always',str(baseline),str(interrupted)])
                run(['env','ACCOUNT_WRITE_CUT=0',str(binary),str(interrupted),str(sector)],600)
                run(['env','ACCOUNT_RECOVER_ONLY=1',str(binary),str(interrupted),str(sector)],600)
                run(['e2fsck','-fn',str(interrupted)])
                assert account_bytes(interrupted) in (old,new)
                manifest['cases'].append({'account_host_sector':sector,'write_failure':True,'result':'PASS'})
            finally:
                # Preserve the last interrupted image even when a gate fails.
                for image in (disk,interrupted):
                    if image.exists():run(['cp','--sparse=always',str(image),str(out/image.name)])
def session(boot,operator,send,wait,command,prompt,transcript):
    def interactive(cmd,steps,expected):
        start=len(transcript);send(cmd)
        for marker,password in steps:
            wait(lambda t:marker in t,start);start=len(transcript);send(password)
        wait(lambda t:expected in t and prompt(t),start)
    if boot==0:
        interactive('passwd',[('Current password: ',operator),('New password (12..128 characters): ',NEW),('Confirm new password: ',NEW)],'Password updated')
        operator=NEW
        interactive('sudo useradd phase3user 1100',[('sudo password: ',operator),('New password (12..128 characters): ',USER),('Confirm new password: ',USER)],'Account created')
        interactive('sudo hostname phase3-machine',[('sudo password: ',operator)],'')
        interactive('sudo /bin/shell',[('sudo password: ',operator)],'')
        # The parent is still waiting: its launcher entry must already be on
        # disk, because this nested shell can reboot without returning to it.
        command('cat /home/operator/.fortress-history','sudo /bin/shell')
        command('mkdir /etc/network','')
        command('echo address=10.0.2.15/24 > /etc/network/network.conf','')
        command('echo nameserver 10.0.2.3 > /etc/resolv.conf','')
        command('exit','')
        command('id','uid=1000 gid=1000')
        interactive('sudo !!',[('sudo password: ',operator)],'uid=0 gid=0')
        command('sudo','usage: sudo command [args] | sudo -s')
        interactive('sudo -s',[('sudo password: ',operator)],'')
        command('id','uid=0 gid=0')
        command('exit','')
        command('ifup --dry-run','valid (dry-run)')
        command('echo phase3-history-proof','phase3-history-proof');command('history save','History saved')
    else:
        command('hostname','phase3-machine')
        command('ifup --dry-run','valid (dry-run)')
        command('cat /etc/resolv.conf','nameserver 10.0.2.3')
        command('cat .fortress-history','phase3-history-proof')
        # Independent login proves persisted account/password/home ownership.
        start=len(transcript);send('exit');wait(lambda t:'FortressOS login: ' in t,start)
        send('phase3user');wait(lambda t:t.endswith('Password: '));send(USER);wait(prompt)
        command('pwd','/home/phase3user');command('id','uid=1100 gid=1100');command('sudo id','not authorized')
        start=len(transcript);send('exit');wait(lambda t:'FortressOS login: ' in t,start)
        send('operator');wait(lambda t:t.endswith('Password: '));send(operator);wait(prompt)
    assert NEW.encode() not in transcript and USER.encode() not in transcript,'secret echoed'
    return operator
def audit(out,label,root):
    log=out/(label+'-account-audit.log')
    def dump(path,name):
        target=out/(label+'-'+name);gate.fixture.run(['debugfs','-R',f'dump {path} {target}',str(root)],log);return target.read_bytes()
    assert dump('/etc/hostname','hostname')==b'phase3-machine\n'
    assert dump('/etc/network/network.conf','network.conf')==b'address=10.0.2.15/24\n'
    assert dump('/etc/resolv.conf','resolv.conf')==b'nameserver 10.0.2.3\n'
    assert b'phase3user:x:1100:1100::/home/phase3user:/bin/shell\n' in dump('/etc/passwd','passwd')
    shadow=dump('/etc/shadow','shadow');assert shadow.startswith(b'root:!:::::::\n') and b'phase3user:$5$rounds=100000$' in shadow
    for name,password in [('operator',NEW),('phase3user',USER)]:
        value=next(line.split(b':')[1].decode() for line in shadow.splitlines() if line.startswith(name.encode()+b':'))
        assert gate.fixture.crypt_hash(password,value)==value,'independent password hash mismatch'
    for path,uid,gid,mode in [('/etc/shadow',0,0,'0600'),('/home/phase3user',1100,1100,'0700'),('/home/operator/.fortress-history',1000,1000,'0600')]:
        st=gate.fixture.run(['debugfs','-R','stat '+path,str(root)],log)
        assert re.search(r'User:\s+'+str(uid)+r'\s+Group:\s+'+str(gid)+r'\b',st) and re.search(r'Mode:\s+'+mode+r'\b',st),(path,st)
    pending=gate.fixture.run(['debugfs','-R','stat /var/lib/fortress/accounts/pending',str(root)],log)
    assert 'File not found' in pending
gate.PHASE=3;gate.guest=guest;gate.SESSION_HOOK=session;gate.HOST_HOOK=host;gate.AUDIT_HOOK=audit
if __name__=='__main__':gate.main()
