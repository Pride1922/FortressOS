"""Private installed-root mutation binding, real Ring 3 and disposable NVMe."""
import argparse,inspect,json,pathlib,struct,subprocess,sys,tempfile,tarfile,shutil,socket,time,re,os,hmac,hashlib
from concurrent.futures import ThreadPoolExecutor,as_completed
import test_pkg_boot_admission as base
from pkg_exec_overlay import overlay
from pkg_install_fixture import fixture
from pkg_enroll_records import keys,PUB,H,signed
from pkg_enroll_fixture import command,gpt,FILES
from test_getrandom import CODE,VARS
ROOT=base.ROOT
KEY=None
BOOTSTRAP=None
PROGRAM=None
BASE_PATHS=('/bin/pkg-boot','/bin/shell','/bin/sudo','/bin/login','/etc/passwd','/etc/group','/etc/shadow','/bin/pkg','/sbin/sshd','/etc/ssh/sshd_config','/etc/ssh/ssh_host_ed25519_key')

def canonical_account_stage(image):
    # Test the authenticated-lineage refusal, not merely malformed dir slots.
    from pkg_enroll_fixture import directory,inode
    from test_jbd2_replay_host import blocks
    parent='/var/lib/fortress/accounts';mapping=blocks(image,parent);assert len(mapping)==1
    raw=bytearray(image.read_bytes());directory(raw,inode(image,parent),inode(image,'/var/lib/fortress'),[(inode(image,parent+'/passwd.new'),'passwd.new')],5,mapping[0]);image.write_bytes(raw)

def cli_objects(run):
    from test_pkg_cli_host import SOURCES,VENDORS,INCLUDES
    objects=[]
    for i,path in enumerate(SOURCES+['user/pkg/transaction_cli.c','user/pkg/mutation_cli.c','tests/pkg_exec_cli.c','src/lib/string.c']+VENDORS):
        obj=f'build/exec-cli-{i}.o';flags=['gcc','-fstack-usage','-Itests','-std=c11','-Os','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-fno-pie','-fno-stack-protector','-mno-red-zone','-mno-sse','-mno-mmx','-msoft-float','-ffunction-sections','-fdata-sections',*INCLUDES]
        if path=='tests/pkg_exec_cli.c':flags+=['-Wframe-larger-than=1024']
        if path in VENDORS:flags+=['-include','src/lib/crypto/namespace.h']
        run(flags+['-c',path,'-o',obj],f'cli-{i}');objects.append(obj)
    return objects

def build(image,out,roles,kernel,bootstrap):
    out.mkdir();seed=fixture(out/'executor',keys(roles['catalog']),KEY,15,False,package='field-kit',command_name='field-tool',executable=PROGRAM)
    # SSH host identity is an independent ephemeral TEST key, not a package
    # signing key or the external local-MAC key. It belongs to this fixture.
    with tempfile.TemporaryDirectory(prefix='fortress-pkgexec-ssh-') as temp:
        ssh_key=pathlib.Path(temp)/'hostkey'
        command(['ssh-keygen','-q','-t','ed25519','-N','','-C','pkg-system-test','-f',str(ssh_key)])
        ssh_bytes=ssh_key.read_bytes()
        ssh_public=ssh_key.with_suffix('.pub').read_bytes()
    (out/'ssh-host.pub').write_bytes(ssh_public)
    commands=out/'base.commands';paths=BASE_PATHS;expected=b''
    with tarfile.open(BOOTSTRAP) as archive:
        lines=['mkdir /bin','set_inode_field /bin mode 040755','mkdir /sbin','mkdir /etc/ssh','set_inode_field /etc/ssh mode 040700']
        for i,path in enumerate(paths):
            if i<4 or path in ('/bin/pkg','/sbin/sshd'):
                data=archive.extractfile(path[1:]).read();source=out/('base-'+str(i));source.write_bytes(data);lines += [f'write {source} {path}',f'set_inode_field {path} mode {"0104755" if i==2 else "0100755"}',f'set_inode_field {path} uid 0',f'set_inode_field {path} gid 0']
            else:
                data=FILES[path] if path in FILES else (b'Enabled yes\nPort 22\nListenAddress 0.0.0.0\nPermitRootLogin no\nPasswordAuthentication yes\nPubkeyAuthentication no\nMaxSessions 2\nMaxAuthTries 5\nLoginGraceTime 60\n' if path.endswith('sshd_config') else ssh_bytes)
                if path not in FILES:
                    source=out/('base-'+str(i));source.write_bytes(data);source.chmod(0o600);lines.append(f'write {source} {path}')
            expected+=H(data)
        for path,mode in [('/root','040700'),('/etc','040755'),('/var','040755'),('/var/lib','040755'),('/var/lib/fortress','040755'),('/var/lib/fortress/accounts','040700')]:lines.append(f'set_inode_field {path} mode {mode}')
        for path in [p for p in paths[4:] if p.startswith('/etc/')]:lines.append(f'set_inode_field {path} mode {"0100600" if path.endswith("shadow") or path.startswith("/etc/ssh/") else "0100644"}')
    commands.write_text('\n'.join(lines)+'\n');(out/'base.log').write_text(command(['debugfs','-w','-f',str(commands),str(seed)]))
    # debugfs mkdir may consume a reserved account slot. Restore the explicit
    # account layout, retaining the new SSH directory as an ordinary entry.
    from pkg_enroll_fixture import directory,inode
    from test_jbd2_replay_host import blocks
    raw=bytearray(seed.read_bytes());mapping=blocks(seed,'/etc');assert len(mapping)==1
    entries=[(inode(seed,path),path.rsplit('/',1)[1]) for path in FILES]
    entries.append((inode(seed,'/etc/ssh'),'ssh',2))
    directory(raw,inode(seed,'/etc'),2,entries,6,mapping[0]);seed.write_bytes(raw);image.write_bytes(raw)
    artifact=(out/'executor/artifact').read_bytes();catalog=(out/'executor/catalog').read_bytes();selector_keys=keys(roles['selector'])
    policy=(out/'executor/policy').read_bytes()+(out/'executor/config').read_bytes()+b''.join(PUB(k) for k in selector_keys)+expected+struct.pack('<II',len(artifact),len(catalog))+artifact+catalog
    assert len(policy)<=32768
    body=bytearray(576);body[:16]=b'FORT-PKG-EXEC-B2';body[16:48]=kernel;body[48:80]=bootstrap;body[80:112]=H(policy);body[112:144]=H(KEY)
    return {'selector':signed(body,selector_keys),'policy':policy,'identities':[]}

def boot(out,work,iso,source,fw,smp,label,key_file,refused=False,cut=False):
    dest=out/label;dest.mkdir();disk=dest/'disk.img';shutil.copyfile(source,disk);initial=disk.read_bytes();assert disk.is_file() and not disk.is_symlink() and len(initial)==64*1024*1024
    with tempfile.TemporaryDirectory(prefix='fortress-pkgexec-uart-') as temp:
        active_disk=pathlib.Path(temp)/'disk.img';shutil.copyfile(disk,active_disk)
        assert active_disk.is_file() and not active_disk.is_symlink() and active_disk.stat().st_size==len(initial)
        # The probe is autonomous. File capture avoids socket backpressure;
        # a bounded UART transmit timeout can otherwise disable guest output.
        argv=['qemu-system-x86_64','-M','q35','-accel','tcg','-cpu','max','-m','512M','-smp',str(smp),'-display','none','-net','none','-no-reboot','-boot','d','-rtc','base=2026-10-25T12:00:00','-cdrom',str(iso),'-serial',f'file:{dest/"serial.log"}','-drive',f'if=none,format=raw,id=exec,file={active_disk}','-device','nvme,drive=exec,serial=FORTRESS-PKG-EXEC','-fw_cfg','name=opt/fortress/pkg_boot_nvme,string=private-regular-file-only','-fw_cfg',f'name=opt/fortress/pkg_exec_key,file={key_file}']
        if fw=='uefi':
            shutil.copyfile(VARS,dest/'vars.fd');argv+=['-drive',f'if=pflash,format=raw,unit=0,readonly=on,file={CODE}','-drive',f'if=pflash,format=raw,unit=1,file={dest/"vars.fd"}']
        if cut:argv+=['-fw_cfg','name=opt/fortress/pkg_exec_cut,string=durable-commit-reset']
        qmp_path=pathlib.Path(temp)/'qmp';argv+=['-qmp',f'unix:{qmp_path},server=on,wait=off','-d','guest_errors','-D',str(dest/'qemu-debug.log')]
        assert not any(x in argv for x in ('-hda','-hdb','-blockdev','-readconfig'))
        for i,arg in enumerate(argv):
            if arg=='-drive':assert argv[i+1]==f'if=none,format=raw,id=exec,file={active_disk}' or argv[i+1].startswith('if=pflash,format=raw,unit=')
        (dest/'argv.json').write_text(json.dumps(argv,indent=2)+'\n')
        with (dest/'stderr.log').open('w') as err:p=subprocess.Popen(argv,cwd=work,stderr=err,stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL)
        qmp=socket.socket(socket.AF_UNIX);events=bytearray()
        try:
            limit=time.monotonic()+10
            while True:
                try:qmp.connect(str(qmp_path));break
                except (FileNotFoundError,ConnectionRefusedError):assert time.monotonic()<limit;time.sleep(.1)
            qmp.sendall(b'{"execute":"qmp_capabilities"}\n');qmp.setblocking(False)
            limit=time.monotonic()+300
            while time.monotonic()<limit:
                time.sleep(.05)
                try:events.extend(qmp.recv(65536))
                except BlockingIOError:pass
                text=(dest/'serial.log').read_text(errors='replace') if (dest/'serial.log').exists() else ''
                assert not any(x in text for x in ('[PANIC]','[FATAL]','RING3 FAIL','CPU EXCEPTION KERNEL PANIC')) ,text[-6000:]
                if cut and 'PKGEXEC CUT durable-COMMIT' in text:p.kill();p.wait(timeout=5);break
                if refused and 'PKGEXEC HALTED no-fallback' in text:break
                if p.poll() is not None:break
            else:raise AssertionError('timeout '+text[-6000:])
            text=(dest/'serial.log').read_text(errors='replace') if (dest/'serial.log').exists() else ''
            assert not any(x in text for x in ('[PANIC]','[FATAL]','RING3 FAIL','CPU EXCEPTION KERNEL PANIC')),text[-6000:]
            # Stop writers before retaining the complete test disk.
            if refused and p.poll() is None:p.terminate();p.wait(timeout=5)
            shutil.copyfile(active_disk,disk)
            if cut:
                assert 'PKGEXEC CUT durable-COMMIT' in text
            elif refused:
                assert 'PKGEXEC REFUSED' in text and 'writes=0 flushes=0' in text and disk.read_bytes()==initial
            else:
                assert p.poll()==0 and 'PKGEXEC RING3 PASS' in text and 'PKGEXEC private command sessions PASS' in text and 'PKGEXEC PUBLISHED installed-root real-account-service' in text,(p.poll(),text[-6000:])
                try:events.extend(qmp.recv(65536))
                except (BlockingIOError,OSError):pass
                assert any(e.get('event')=='SHUTDOWN' and e.get('data',{}).get('reason')=='guest-shutdown' for e in (json.loads(line) for line in events.splitlines())),events.decode(errors='replace')
                raw=disk.read_bytes();assert raw[:2048*512]==initial[:2048*512] and raw[(2048+65536)*512:]==initial[(2048+65536)*512:]
                root=dest/'root.img';root.write_bytes(raw[2048*512:(2048+65536)*512]);(dest/'fsck.log').write_text(command(['e2fsck','-fn',str(root)]))
                expected=json.loads((out/'expected.json').read_text())
                from pkg_pending_records import observation
                for i,item in enumerate(expected):
                    obj=observation(root,item['path'],i);assert obj[48:80].hex()==item['sha256'] and struct.unpack_from('<III',obj,16)==(0,0,1)
                policy=(out/'policy.bin').read_bytes();cfg=policy[336:632];physical=struct.unpack_from('<Q',cfg,24)[0];record=root.read_bytes()[physical*4096:physical*4096+8192]
                assert hmac.compare_digest(record[8160:],hmac.new(KEY,record[:8160],hashlib.sha256).digest())
                assert struct.unpack_from('<QQQ',record,40)==(73,3,7) and struct.unpack_from('<7I',record,64)==(0,1,0,65535,65535,0,0)
                assert not any(record[96:112]+record[144:256])
                for i,path in enumerate(BASE_PATHS):
                    assert observation(root,path,i)[48:80]==policy[728+32*i:760+32*i]
                (dest/'oracle.json').write_text(json.dumps({'result':'PASS','sequence':struct.unpack_from('<Q',record,24)[0],'generation':struct.unpack_from('<Q',record,32)[0],'build_floor':73,'root_floor':3,'catalog_floor':7,'members':16,'base_hashes_preserved':len(BASE_PATHS)},indent=2)+'\n')
            result={'firmware':fw,'smp':smp,'refused':refused,'reset_at_commit':cut,'boot_runtime':'CUT' if cut else 'PASS','qmp_guest_shutdown':not cut and not refused,'disk_sha256':H(disk.read_bytes()).hex()};(dest/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(label,result['boot_runtime'],flush=True);return result
        finally:
            try:events.extend(qmp.recv(65536))
            except (BlockingIOError,OSError):pass
            (dest/'qmp.log').write_bytes(events);observed=p.poll();qmp.close()
            if p.poll() is None:
                p.terminate()
                try:p.wait(timeout=5)
                except subprocess.TimeoutExpired:p.kill();p.wait(timeout=5)
            shutil.copyfile(active_disk,disk)
            (dest/'process.json').write_text(json.dumps({'observed_returncode':observed,'cleanup_returncode':p.returncode,'intentional_cut':cut},indent=2)+'\n')

def main():
    global KEY,BOOTSTRAP
    parser=argparse.ArgumentParser();parser.add_argument('--smoke',action='store_true');parser.add_argument('--build-only',action='store_true');args=parser.parse_args()
    source=inspect.getsource(base.main).replace('def main():','def build_main():').replace("'pkg-boot-admission-'","'pkg-exec-boot-'").replace('boot_mode=pkg-test-v1','boot_mode=pkg-exec-v1')
    source=source.replace("    assert m['preservation']!='FAIL'",'    # Preserve the failure independently; runtime cases remain useful.')
    source=source.replace("        run(['ld','-nostdlib','-static','-z','noexecstack','-T','user/shell.ld','build/pkgboot-start.o','build/pkgboot.o','-o','build/pkgboot.elf'],'probe-link')","        objects=cli_objects(run)\n        run(['ld','-nostdlib','-static','-z','noexecstack','--gc-sections','-T','user/shell.ld','build/pkgboot-start.o','build/pkgboot.o',*objects,'-o','build/pkgboot.elf'],'probe-link')")
    source=source.replace("        bootstrap=out/'bootstrap.tar'","        bootstrap=out/'bootstrap.tar'\n        global BOOTSTRAP;BOOTSTRAP=bootstrap")
    scope=dict(vars(base),overlay=overlay,build=build,write_fresh_records=lambda *a:None,cli_objects=cli_objects)
    exec(compile(source,'<private executor build>','exec'),scope)
    with tempfile.TemporaryDirectory(prefix='fortress-pkgexec-key-') as temp:
        KEY=os.urandom(32);key_file=pathlib.Path(temp)/'key';key_file.write_bytes(KEY);key_file.chmod(0o600)
        before=set((ROOT/'.codex-remote-attachments').glob('pkg-exec-boot-*'));saved=sys.argv;sys.argv=[saved[0],'--build-only']
        # build() reads the bootstrap location through its module globals.
        original_build=scope['build']
        def producer(*a):
            global BOOTSTRAP
            BOOTSTRAP=scope['BOOTSTRAP'];return original_build(*a)
        scope['build']=producer
        try:scope['build_main']()
        finally:sys.argv=saved
        added=set((ROOT/'.codex-remote-attachments').glob('pkg-exec-boot-*'))-before;assert len(added)==1;out=added.pop();work=out/'workspace'
        # Producer temporary inputs are removed by the base runner; reconstruct
        # expected profile directly from its signed manifest recipe.
        from pkg_artifact_vectors import elf
        items=[{'path':'/usr/bin/field-tool','sha256':H(elf() if PROGRAM is None else PROGRAM).hex()}]+[{'path':f'/usr/share/pkg/field-kit/73/doc/shared/{i:02}/a/b/c/readme','sha256':H(b'documentation\n'*(i+1)).hex()} for i in range(15)]
        (out/'expected.json').write_text(json.dumps(items,indent=2)+'\n')
        assert all(KEY not in (out/p).read_bytes() for p in ('valid.iso','policy.bin','selector.bin','bootstrap.tar'))
        manifest=json.loads((out/'manifest.json').read_text());results=[]
        frames={line.split('\t')[0]:int(line.split('\t')[1]) for line in (work/'build/fs/ext4.su').read_text().splitlines() if any(':'+p in line for p in ('px_','mx_','jb_boot'))}
        manifest['executor_frames']=frames;assert frames and max(frames.values())<=1024,frames
        if args.build_only:
            manifest['boot_runtime']='NOT RUN'
            (out/'exec-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
            print(out,'BUILD ONLY',manifest['preservation'],flush=True);return
        try:
            def pair(config):
                fw,smp=config;label=f'{fw}-{smp}';cut_label=label+'-cut'
                a=boot(out,work,out/'valid.iso',out/'enrolled.img',fw,smp,cut_label,key_file,cut=True)
                b=boot(out,work,out/'valid.iso',out/cut_label/'disk.img',fw,smp,label,key_file)
                c=boot(out,work,out/'valid.iso',out/label/'disk.img',fw,smp,label+'-reboot',key_file)
                return [a,b,c]
            configs=[('bios',1)] if args.smoke else [('bios',1),('bios',4),('uefi',1),('uefi',4)]
            # Keep the acceptance VMs isolated; separate development campaigns
            # retain their earlier parallel-startup failures and diagnostics.
            with ThreadPoolExecutor(max_workers=1) as pool:
                for future in as_completed([pool.submit(pair,c) for c in configs]):results.extend(future.result())
            wrong=pathlib.Path(temp)/'wrong';wrong.write_bytes(os.urandom(32));wrong.chmod(0o600);results.append(boot(out,work,out/'valid.iso',out/'enrolled.img','bios',1,'wrong-key',wrong,True))
            foreign_root=out/'foreign-account.root';shutil.copyfile(out/'oracle-root.img',foreign_root);payload=out/'foreign-account.bytes';payload.write_bytes(b'foreign account stage\n')
            commands=out/'foreign-account.commands';commands.write_text(f'write {payload} /var/lib/fortress/accounts/passwd.new\nset_inode_field /var/lib/fortress/accounts/passwd.new mode 0100600\nset_inode_field /var/lib/fortress/accounts/passwd.new uid 0\nset_inode_field /var/lib/fortress/accounts/passwd.new gid 0\n')
            argv=['debugfs','-w','-f',str(commands),str(foreign_root)];manifest['commands'].append(argv);(out/'foreign-account-seed.log').write_text(command(argv));canonical_account_stage(foreign_root);(out/'foreign-account-fsck.log').write_text(command(['e2fsck','-fn',str(foreign_root)]))
            foreign_disk=out/'foreign-account.img';foreign_disk.write_bytes(gpt(foreign_root.read_bytes()));results.append(boot(out,work,out/'valid.iso',foreign_disk,'bios',1,'foreign-account',key_file,True))
            manifest['boot_runtime']='PASS'
        except BaseException as e:manifest['boot_runtime']='FAIL';manifest['error']=repr(e);raise
        finally:
            manifest['cases']=results;manifest['changed_sources']=[p for p,h in manifest['sources'].items() if base.H(ROOT/p)!=h];manifest['changed_guards']=[p for p,h in manifest['guards'].items() if not (ROOT/p).is_file() or base.H(ROOT/p)!=h];manifest['changed_copies']=[p for p,h in manifest['compiled_inputs'].items() if not (work/p).is_file() or base.H(work/p)!=h];manifest['preservation']='FAIL' if manifest['changed_sources'] or manifest['changed_guards'] or manifest['changed_copies'] else manifest['preservation'];(out/'exec-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n');print(out,manifest['boot_runtime'],manifest['preservation'],flush=True)
if __name__=='__main__':main()
