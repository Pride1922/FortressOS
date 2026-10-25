"""Normal /bin/pkg through actual login/sudo/SSH on disposable enrolled roots.

Uses the normal modules and ABI, with the isolated TEST admission adapter.
No canonical media, physical target, production trust or root enrollment.
"""
import argparse,datetime,hashlib,hmac,inspect,json,os,pathlib,re,shutil,socket,struct,subprocess,sys,tempfile,threading,time
import test_pkg_exec_boot as test
from pkg_enroll_records import keys,H
from pkg_enroll_fixture import command,gpt,FILES
from test_getrandom import CODE,VARS,PROMPT
ROOT=test.ROOT
PASSWORD='Pkg-System-Only-42'

def runnable():
    from pkg_artifact_vectors import elf
    data=bytearray(elf()[:120]+bytes.fromhex('31c031ff0f050f0b'))
    struct.pack_into('<Q',data,96,len(data));return bytes(data)

def interactive_overlay(work,replace):
    result=test_original_overlay(work,replace)
    replace('src/kernel/main.c','    const char *initial_program=mx_requested?"/bin/pkg-boot":"/bin/login";',
            '    const char *initial_program="/bin/login";')
    return result

def seed_interactive(image,out,roles,kernel,bootstrap):
    result=original_producer(image,out,roles,kernel,bootstrap)
    commands=out/'interactive.commands'
    commands.write_text('mkdir /home\nmkdir /home/operator\nset_inode_field /home/operator mode 040700\nset_inode_field /home/operator uid 1000\nset_inode_field /home/operator gid 1000\nmkdir /dev\nmkdir /run\nmkdir /tmp\nset_inode_field /tmp mode 041777\nmkdir /var/log\nmkdir /mnt\n')
    (out/'interactive-seed.log').write_text(command(['debugfs','-w','-f',str(commands),str(image)]))
    return result

test_original_overlay=test.overlay
original_producer=test.build

def build():
    before=set((ROOT/'.codex-remote-attachments').glob('pkg-exec-boot-*'))
    old_args=sys.argv;saved_files=dict(FILES)
    hashed=command(['openssl','passwd','-5','-salt','PkgSys42',PASSWORD]).strip()
    FILES.update({'/etc/passwd':b'root:x:0:0:root:/root:/bin/shell\noperator:x:1000:1000:operator:/home/operator:/bin/shell\n',
        '/etc/group':b'root:x:0:\nwheel:x:10:operator\noperator:x:1000:\n',
        '/etc/shadow':f'root:!:::::::\noperator:{hashed}:::::::\n'.encode()})
    test.overlay=interactive_overlay;test.build=seed_interactive;test.PROGRAM=runnable()
    sys.argv=[old_args[0],'--build-only']
    try:test.main()
    finally:sys.argv=old_args;FILES.clear();FILES.update(saved_files)
    added=set((ROOT/'.codex-remote-attachments').glob('pkg-exec-boot-*'))-before;assert len(added)==1
    prior=added.pop();receipt=json.loads((prior/'manifest.json').read_text());assert not receipt.get('error')
    (prior/'interactive-build.json').write_text(json.dumps({'profile':'normal-pkg-login-v1','build_manifest_sha256':test.base.H(prior/'manifest.json')},indent=2)+'\n')
    return prior

class Stream:
    def __init__(self,connection,read_pipe=False,process=None):
        self.connection=connection;self.pipe=read_pipe;self.process=process;self.data=bytearray();self.lock=threading.Lock();self.stop=threading.Event()
        self.worker=threading.Thread(target=self.drain,daemon=True);self.worker.start()
    def drain(self):
        if not self.pipe:self.connection.settimeout(.1)
        while not self.stop.is_set():
            try:data=os.read(self.connection.fileno(),65536) if self.pipe else self.connection.recv(65536)
            except (socket.timeout,BlockingIOError):continue
            except OSError:break
            if not data:break
            with self.lock:self.data.extend(data)
    def text(self):
        with self.lock:raw=bytes(self.data)
        return re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]','',raw.decode(errors='replace')).replace('\r','')
    def send(self,data):
        if self.pipe:self.process.stdin.write(data);self.process.stdin.flush()
        else:self.connection.sendall(data)
    def wait(self,predicate,pump,timeout=90,start=0):
        until=time.monotonic()+timeout
        while time.monotonic()<until:
            pump();text=self.text()[start:]
            assert not any(x in text for x in ('[PANIC]','[FATAL]','RING3 FAIL','CPU EXCEPTION KERNEL PANIC','PKGEXEC HALTED no-fallback')),text[-3000:]
            if predicate(text):return text
            time.sleep(.03)
        raise AssertionError('interactive timeout '+self.text()[-5000:])
    def close(self,path):
        self.stop.set()
        try:self.connection.close()
        except OSError:pass
        self.worker.join(timeout=2)
        with self.lock:path.write_bytes(bytes(self.data))

def audit(out,dest,initial,key):
    raw=(dest/'disk.img').read_bytes();assert raw[:2048*512]==initial[:2048*512] and raw[(2048+65536)*512:]==initial[(2048+65536)*512:]
    root=dest/'root.img';root.write_bytes(raw[2048*512:(2048+65536)*512])
    (dest/'fsck.log').write_text(command(['e2fsck','-fn',str(root)]))
    from pkg_pending_records import observation
    expected=json.loads((out/'expected.json').read_text())
    for i,item in enumerate(expected):
        obj=observation(root,item['path'],i)
        assert obj[48:80].hex()==item['sha256'] and struct.unpack_from('<III',obj,16)==(0,0,1)
    policy=(out/'policy.bin').read_bytes();physical=struct.unpack_from('<Q',policy,360)[0]
    record=root.read_bytes()[physical*4096:physical*4096+8192]
    assert hmac.compare_digest(record[8160:],hmac.new(key,record[:8160],hashlib.sha256).digest())
    assert struct.unpack_from('<QQQ',record,40)==(73,3,7) and struct.unpack_from('<7I',record,64)==(0,1,0,65535,65535,0,0)
    assert not any(record[96:112]+record[144:256])
    for i,path in enumerate(test.BASE_PATHS):assert observation(root,path,i)[48:80]==policy[728+32*i:760+32*i]
    proof=dest/'user-proof';command(['debugfs','-R',f'dump /home/operator/pkg-note {proof}',str(root)])
    assert proof.read_bytes()==b'PKG_USER_NOTE\n'
    result={'result':'PASS','members':16,'base_hashes':len(test.BASE_PATHS),'sequence':struct.unpack_from('<Q',record,24)[0],'generation':struct.unpack_from('<Q',record,32)[0]}
    (dest/'oracle.json').write_text(json.dumps(result,indent=2)+'\n');return result

def boot(out,work,iso,source,fw,smp,label,key_file):
    dest=out/label;dest.mkdir();disk=dest/'disk.img';shutil.copyfile(source,disk);initial=disk.read_bytes()
    assert disk.is_file() and not disk.is_symlink() and len(initial)==64*1024*1024
    sockets=[];ssh_processes=[];streams=[];events=bytearray();actions=[]
    with tempfile.TemporaryDirectory(prefix='fortress-pkg-system-uart-') as temp:
        temp=pathlib.Path(temp);uartpath=temp/'uart';qmppath=temp/'qmp'
        # Native Linux scratch avoids host-share latency for journal barriers.
        # The only writable NVMe remains this fresh regular-file copy.
        active_disk=temp/'disk.img';shutil.copyfile(disk,active_disk)
        assert active_disk.is_file() and not active_disk.is_symlink() and active_disk.stat().st_size==len(initial)
        reserve=socket.socket();reserve.bind(('127.0.0.1',0));port=reserve.getsockname()[1];reserve.close()
        argv=['qemu-system-x86_64','-M','q35','-accel','tcg','-cpu','max','-m','512M','-smp',str(smp),'-display','none','-no-reboot','-boot','d','-rtc','base=2026-10-25T12:00:00',
            '-cdrom',str(iso),'-chardev',f'socket,id=uart,path={uartpath},server=on,wait=off','-serial','chardev:uart',
            '-qmp',f'unix:{qmppath},server=on,wait=off','-netdev',f'user,id=net0,hostfwd=tcp:127.0.0.1:{port}-:22','-device','e1000e,netdev=net0',
            '-drive',f'if=none,format=raw,id=pkg,file={active_disk}','-device','nvme,drive=pkg,serial=FORTRESS-PKG-SYSTEM',
            '-fw_cfg','name=opt/fortress/pkg_boot_nvme,string=private-regular-file-only','-fw_cfg',f'name=opt/fortress/pkg_exec_key,file={key_file}']
        if fw=='uefi':
            shutil.copyfile(VARS,dest/'vars.fd');argv+=['-drive',f'if=pflash,format=raw,unit=0,readonly=on,file={CODE}','-drive',f'if=pflash,format=raw,unit=1,file={dest/"vars.fd"}']
        assert not any(x in argv for x in ('-hda','-hdb','-blockdev','-readconfig'))
        for i,arg in enumerate(argv):
            if arg=='-drive':assert argv[i+1]==f'if=none,format=raw,id=pkg,file={active_disk}' or argv[i+1].startswith('if=pflash,format=raw,unit=')
        (dest/'argv.json').write_text(json.dumps(argv,indent=2)+'\n')
        error=(dest/'stderr.log').open('wb');p=subprocess.Popen(argv,cwd=work,stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=error)
        qmp=socket.socket(socket.AF_UNIX);sockets.append(qmp)
        try:
            until=time.monotonic()+15
            for path,connection in ((qmppath,qmp),(uartpath,socket.socket(socket.AF_UNIX))):
                while True:
                    try:connection.connect(str(path));break
                    except (FileNotFoundError,ConnectionRefusedError):assert time.monotonic()<until;time.sleep(.05)
                if path==uartpath:uart=connection
            qmp.sendall(b'{"execute":"qmp_capabilities"}\n');qmp.setblocking(False)
            console=Stream(uart);streams.append((console,dest/'serial.log'))
            def pump():
                try:
                    while True:
                        chunk=qmp.recv(65536)
                        if not chunk:break
                        events.extend(chunk)
                except (BlockingIOError,OSError):pass
                assert p.poll() is None or p.returncode==0,(p.returncode,console.text()[-4000:])
            counter=0
            def run(stream,cmd,expected=0,sudo=False):
                nonlocal counter
                counter+=1;marker=f'PKGI_{counter}';begin=len(stream.text())
                if cmd.rstrip().endswith('&'):
                    # The shell deliberately refuses asynchronous compound lists.
                    stream.send((cmd+'\n').encode())
                    stream.wait(lambda t:PROMPT.search(t),pump,start=begin)
                    stream.send(f'echo {marker}:$?\n'.encode())
                else:stream.send((cmd+'; '+f'echo {marker}:$?\n').encode())
                if sudo:
                    stream.wait(lambda t:'sudo password: ' in t,pump,start=begin,timeout=30);stream.send((PASSWORD+'\n').encode())
                text=stream.wait(lambda t:re.search(r'(?m)^'+marker+r':\d+\s*$',t),pump,start=begin)
                found=int(re.search(r'(?m)^'+marker+r':(\d+)\s*$',text)[1]);assert found==expected,(cmd,found,text[-3000:])
                actions.append({'command':cmd,'expected_exit':expected,'observed_exit':found})
                print(label,marker,cmd,found,flush=True);return text
            console.wait(lambda t:'FortressOS login: ' in t,pump)
            assert not PROMPT.search(console.text()),'passwordless shell'
            console.send(b'operator\n');console.wait(lambda t:'Password: ' in t,pump)
            console.send(b'wrong-package-password\n');console.wait(lambda t:'Login incorrect' in t,pump)
            begin=len(console.text());console.send(b'operator\n');console.wait(lambda t:'Password: ' in t,pump,start=begin)
            console.send((PASSWORD+'\n').encode());console.wait(lambda t:PROMPT.search(t),pump,start=begin)
            run(console,'/bin/pkg --help');run(console,'/bin/pkg status');run(console,'/bin/pkg install field-kit',1)
            run(console,'echo PKG_USER_NOTE > /home/operator/pkg-note')
            run(console,'sudo /bin/pkg install field-kit',sudo=True)
            run(console,'/usr/bin/field-tool')
            begin=len(console.text());console.send(b'sudo /bin/shell\n');console.wait(lambda t:'sudo password: ' in t,pump,start=begin);console.send((PASSWORD+'\n').encode())
            console.wait(lambda t:PROMPT.search(t),pump,start=begin)
            run(console,'/bin/pkg remove field-kit');run(console,'/bin/pkg recover')
            run(console,'/bin/pkg-boot --hostkey')
            launched=run(console,'/sbin/sshd -D &')
            daemon_job=re.search(r'(?m)^\[(\d+)\] \d+\s*$',launched)
            assert daemon_job,launched
            console.wait(lambda t:'Listening on port 22' in t,pump,timeout=30)
            # Independent OpenSSH client, strict host-key pinning and password auth.
            pub=(out/'records/ssh-host.pub').read_text().split();known=temp/'known_hosts'
            known.write_text(f'[127.0.0.1]:{port} {pub[0]} {pub[1]}\n')
            ask=temp/'askpass';ask.write_text('#!/bin/sh\nprintf "%s\\n" '+"'"+PASSWORD+"'"+'\n');ask.chmod(0o700)
            env=dict(os.environ,SSH_ASKPASS=str(ask),SSH_ASKPASS_REQUIRE='force',DISPLAY='pkg-system-test')
            def connect_ssh(name):
                args=['ssh','-tt','-p',str(port),'-o','StrictHostKeyChecking=yes','-o',f'UserKnownHostsFile={known}',
                    '-o','PreferredAuthentications=password','-o','PubkeyAuthentication=no','-o','NumberOfPasswordPrompts=1','-o','ConnectTimeout=10','operator@127.0.0.1']
                proc=subprocess.Popen(args,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,env=env,start_new_session=True)
                ssh_processes.append(proc)
                stream=Stream(proc.stdout,True,proc);streams.append((stream,dest/(name+'.log')))
                stream.wait(lambda t:PROMPT.search(t),pump,timeout=40);return proc,stream
            ssh,remote=connect_ssh('ssh')
            run(remote,'/bin/pkg status');run(remote,'/bin/pkg install field-kit',1)
            run(remote,'sudo /bin/pkg install field-kit',sudo=True);run(remote,'/usr/bin/field-tool')
            time.sleep(.2);run(remote,'sudo /bin/pkg remove field-kit',sudo=True)
            run(remote,'sudo /bin/pkg recover',sudo=True);run(remote,'sudo /bin/pkg install field-kit',sudo=True)
            remote.send(b'exit\n');ssh.wait(timeout=15);assert ssh.returncode==0
            # A real SSH disconnect while a durable COMMIT is pinned. The pause
            # control belongs only to the measured private probe, never /bin/pkg.
            run(console,'/bin/pkg remove field-kit');run(console,'/bin/pkg-boot --pause')
            dropped,remote_drop=connect_ssh('ssh-disconnect');begin=len(remote_drop.text())
            remote_drop.send(b'sudo /bin/pkg install field-kit\n');remote_drop.wait(lambda t:'sudo password: ' in t,pump,start=begin);remote_drop.send((PASSWORD+'\n').encode())
            until=time.monotonic()+90
            while True:
                # The query returns a status; do not retry a mutation.
                counter+=1;marker=f'PKGI_{counter}';begin=len(console.text());console.send(f'/bin/pkg-boot --waiting; echo {marker}:$?\n'.encode())
                seen=console.wait(lambda t:re.search(r'(?m)^'+marker+r':[01]\s*$',t),pump,start=begin)
                if re.search(r'(?m)^'+marker+r':0\s*$',seen):break
                assert time.monotonic()<until;time.sleep(.1)
            run(console,'/bin/pkg status',1)
            dropped.terminate();dropped.wait(timeout=10);time.sleep(1)
            run(console,'/bin/pkg-boot --release')
            time.sleep(1)
            state=run(console,'/bin/pkg status');assert 'PENDING' in state or 'RETIRING' in state,state
            run(console,'/bin/pkg recover');stable=run(console,'/bin/pkg status')
            run(console,'/bin/pkg recover');again=run(console,'/bin/pkg status')
            assert re.search(r'sequence: (\d+)',stable)[1]==re.search(r'sequence: (\d+)',again)[1]
            run(console,'kill %'+daemon_job[1]+' TERM')
            assert all(PASSWORD not in stream.text() for stream,path in streams)
            console.send(b'/bin/pkg-boot --poweroff\n');p.wait(timeout=60);pump();assert p.returncode==0
            assert any(e.get('event')=='SHUTDOWN' and e.get('data',{}).get('reason')=='guest-shutdown' for e in map(json.loads,events.splitlines()))
            shutil.copyfile(active_disk,disk);oracle=audit(out,dest,initial,test.KEY)
            result={'firmware':fw,'smp':smp,'runtime':'PASS','login_sudo':True,'ssh':True,'ssh_disconnect_pending_recovered':True,'runnable_package':True,'oracle':oracle}
            (dest/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(label,'PASS',flush=True);return result
        finally:
            for child in ssh_processes:
                if child.poll() is None:child.terminate()
                try:child.wait(timeout=5)
                except subprocess.TimeoutExpired:child.kill();child.wait(timeout=5)
            observed=p.poll()
            if p.poll() is None:
                p.terminate()
                try:p.wait(timeout=5)
                except subprocess.TimeoutExpired:p.kill();p.wait(timeout=5)
            shutil.copyfile(active_disk,disk)
            for stream,path in streams:stream.close(path)
            for connection in sockets:connection.close()
            error.close();(dest/'qmp.log').write_bytes(events)
            (dest/'actions.json').write_text(json.dumps(actions,indent=2)+'\n')
            (dest/'process.json').write_text(json.dumps({'observed_returncode':observed,'cleanup_returncode':p.returncode},indent=2)+'\n')

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--smoke',action='store_true');parser.add_argument('--reuse-build',type=pathlib.Path);parser.add_argument('--build-only',action='store_true');args=parser.parse_args()
    prior=args.reuse_build.resolve() if args.reuse_build else build()
    assert prior.is_relative_to((ROOT/'.codex-remote-attachments').resolve())
    profile=json.loads((prior/'interactive-build.json').read_text());assert profile['profile']=='normal-pkg-login-v1'
    assert profile['build_manifest_sha256']==test.base.H(prior/'manifest.json')
    previous=json.loads((prior/'manifest.json').read_text());work=prior/'workspace'
    assert all(test.base.H(work/p)==v for p,v in previous['compiled_inputs'].items())
    assert test.base.H(work/'bin/fortress.elf')==previous['measurements']['kernel']
    assert test.base.H(prior/'bootstrap.tar')==previous['measurements']['bootstrap']
    if args.build_only:print(prior,'INTERACTIVE BUILD ONLY');return
    out=ROOT/'.codex-remote-attachments'/('pkg-system-interactive-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'));out.mkdir()
    m={'build':str(prior),'runtime':'INCOMPLETE','preservation':'INCOMPLETE','cases':[]}
    sources={p:test.base.H(ROOT/p) for p in previous['sources']};guards={p:test.base.H(ROOT/p) for p in previous['guards']}
    saved_files=dict(FILES)
    try:
        stale=[p for p,v in previous['sources'].items() if sources[p]!=v]
        # This host controller is not a compiled input. Permit its fixes when
        # reusing a byte-checked kernel, while recording the exact difference.
        # Every guest input and fixture/overlay producer must still match.
        m['build_source_differences']=stale
        assert set(stale)<={'scripts/test_pkg_system_interactive.py'},('stale interactive build',stale)
        with tempfile.TemporaryDirectory(prefix='fortress-pkg-system-keys-') as temp:
            temp=pathlib.Path(temp);test.KEY=os.urandom(32);key=temp/'key';key.write_bytes(test.KEY);key.chmod(0o600);roles={}
            for name,values in (('selector',(111,112,113)),('catalog',(1,2,3))):
                roles[name]=[]
                for value in values:
                    path=temp/f'{name}-{value}';path.write_bytes(bytes([value])*32);path.chmod(0o600);roles[name].append(path)
                keys(roles[name])
            hashed=command(['openssl','passwd','-5','-salt','PkgSys42',PASSWORD]).strip()
            FILES.update({'/etc/passwd':b'root:x:0:0:root:/root:/bin/shell\noperator:x:1000:1000:operator:/home/operator:/bin/shell\n',
                '/etc/group':b'root:x:0:\nwheel:x:10:operator\noperator:x:1000:\n','/etc/shadow':f'root:!:::::::\noperator:{hashed}:::::::\n'.encode()})
            test.PROGRAM=runnable();shutil.copyfile(prior/'bootstrap.tar',out/'bootstrap.tar');test.BOOTSTRAP=out/'bootstrap.tar'
            root=out/'initial.root';prepared=seed_interactive(root,out/'records',roles,bytes.fromhex(test.base.H(work/'bin/fortress.elf')),H((out/'bootstrap.tar').read_bytes()))
            for name,data in (('policy.bin',prepared['policy']),('selector.bin',prepared['selector'])):(out/name).write_bytes(data)
            (out/'enrolled.img').write_bytes(gpt(root.read_bytes()));shutil.copyfile(out/'records/executor/expected.json',out/'expected.json')
            media=out/'media';shutil.copytree(prior/'iso-valid',media)
            shutil.copyfile(out/'policy.bin',media/'boot/policy.bin');shutil.copyfile(out/'selector.bin',media/'boot/selector.bin')
            iso=out/'valid.iso';(out/'iso.log').write_text(command(['xorriso','-as','mkisofs','-b','boot/limine/limine-bios-cd.bin','-no-emul-boot','-boot-load-size','4','-boot-info-table','--efi-boot','boot/limine/limine-uefi-cd.bin','--efi-boot-part','--efi-boot-image','--protective-msdos-label',str(media),'-o',str(iso)]))
            (out/'bios-install.log').write_text(command([str(work/'limine/limine'),'bios-install',str(iso)]));assert test.KEY not in iso.read_bytes()
            m['measurements']={p:test.base.H(out/p) for p in ('valid.iso','bootstrap.tar','policy.bin','selector.bin')};m['kernel_sha256']=test.base.H(work/'bin/fortress.elf')
            for fw,smp in ([('bios',1)] if args.smoke else [('bios',1),('bios',4),('uefi',1),('uefi',4)]):
                label=f'{fw}-{smp}';m['cases'].append(boot(out,work,iso,out/'enrolled.img',fw,smp,label,key))
                m['cases'].append(boot(out,work,iso,out/label/'disk.img',fw,smp,label+'-reboot',key))
            m['runtime']='PASS'
    except BaseException as e:m['runtime']='FAIL';m['error']=repr(e);raise
    finally:
        FILES.clear();FILES.update(saved_files)
        m['source_changes']=[p for p,v in sources.items() if test.base.H(ROOT/p)!=v]
        m['guard_changes']=[p for p,v in guards.items() if test.base.H(ROOT/p)!=v]
        m['copy_changes']=[p for p,v in previous['compiled_inputs'].items() if test.base.H(work/p)!=v]
        m['preservation']='FAIL' if m['source_changes'] or m['guard_changes'] or m['copy_changes'] else 'PASS'
        (out/'manifest.json').write_text(json.dumps(m,indent=2)+'\n');print(out,m['runtime'],m['preservation'],flush=True)
if __name__=='__main__':main()
