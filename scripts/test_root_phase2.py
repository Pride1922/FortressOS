#!/usr/bin/env python3
"""Installed-root gates. All disk I/O is to disposable regular NVMe fixtures.
No installer media, raw leases, physical disk I/O or mutable account protocol.
"""
import argparse, hashlib, io, json, re, secrets, selectors, shutil, struct, subprocess, tarfile, tempfile, threading, time, uuid
from pathlib import Path
import test_root_phase0 as fixture
from concurrent.futures import ThreadPoolExecutor, as_completed
ROOT=fixture.REPO
CANCEL=threading.Event()

def parallel(function,cases,jobs):
    pool=ThreadPoolExecutor(max_workers=jobs);futures=[]
    try:
        futures=[pool.submit(function,case) for case in cases]
        for future in as_completed(futures):future.result()
    except BaseException:
        CANCEL.set()
        for future in futures:future.cancel()
        raise
    finally:pool.shutdown(wait=True,cancel_futures=True)

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--host-only',action='store_true');parser.add_argument('--quick',action='store_true');parser.add_argument('--skip-host',action='store_true',help='Focused guest debug, not the complete phase gate');parser.add_argument('--jobs',type=int,default=2);args=parser.parse_args()
    assert 1<=args.jobs<=3 and not (args.host_only and args.skip_host)
    parent=ROOT/'build/root-phase2';parent.mkdir(parents=True,exist_ok=True)
    out=Path(tempfile.mkdtemp(prefix='gate-',dir=parent));manifest={'commands':fixture.COMMANDS,'cases':[]}
    manifest['scope']='focused debug' if args.quick or args.skip_host else 'host-only' if args.host_only else 'complete Phase2 gate'
    manifest['git_head']=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()
    normal={str(p):fixture.digest(p) for p in (ROOT/'bin/fortress.iso',ROOT/'bin/fortress.img') if p.is_file()}
    run=lambda cmd,timeout=240:fixture.run(cmd,out/'commands.log',timeout)
    sources={str(p.relative_to(ROOT)):fixture.digest(p) for base in ('src','user','scripts','tests') for p in (ROOT/base).rglob('*') if p.is_file() and p.suffix in ('.c','.h','.inc','.py','.asm')}
    sources['Makefile']=fixture.digest(ROOT/'Makefile')
    operator=secrets.token_hex(16);secret=secrets.token_hex(16)
    try:
        workspace=Path(run(['python3','scripts/create_ext4_guest_workspace.py']).strip());manifest['workspace']=str(workspace)
        with (out/'build.log').open('w') as log:
            build=['make','-j4','ROOT_PHASE2_TEST=1','bin/fortress.elf','bin/initramfs.tar'];fixture.COMMANDS.append(build)
            subprocess.run(build,cwd=workspace,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=300)
        stage=out/'tree';stage.mkdir();metadata={}
        with tarfile.open(workspace/'bin/initramfs.tar') as src:
            seen=set()
            for m in src:
                path=Path(m.name);name=path.as_posix()
                assert not path.is_absolute() and '..' not in path.parts and (m.isdir() or m.isfile()) and len(seen)<512 and name not in seen
                assert re.fullmatch(r'[A-Za-z0-9_./-]+',name) and len(name)<256 and all(len(p)<64 for p in path.parts)
                seen.add(name);target=stage/path;assert target.resolve().is_relative_to(stage.resolve())
                if m.isdir():target.mkdir(parents=True,exist_ok=True)
                else:
                    target.parent.mkdir(parents=True,exist_ok=True)
                    with target.open('xb') as dst:shutil.copyfileobj(src.extractfile(m),dst)
                metadata['/'+name]=(m.mode,m.uid,m.gid)
        for name in ('dev','run','tmp','mnt','home','var','root','home/operator'):(stage/name).mkdir(parents=True,exist_ok=True)
        for name in ('dev','run','tmp'):(stage/name/'hidden').write_bytes(b'hidden persistent placeholder\n')
        for i in range(1100):(stage/f'cache-{i}').write_bytes(b'cache\n')
        metadata['/tmp']=(0o1777,0,0);metadata['/root']=(0o700,0,0);metadata['/home/operator']=(0o700,1000,1000)
        text=(stage/'etc/passwd').read_text().replace('/run/user/1000','/home/operator');(stage/'etc/passwd').write_text(text)
        (stage/'etc/shadow').write_text('root:!:::::::\noperator:'+fixture.crypt_hash(operator,'$5$'+secrets.token_hex(8))+':::::::\n')
        metadata['/etc/shadow']=(0o600,0,0)
        root_uuid=uuid.uuid4();partuuid=uuid.uuid4();install=secrets.token_hex(16)
        identity=f'install_id={install}\nroot_partuuid={partuuid}\nfilesystem_uuid={root_uuid}\n'
        (stage/'etc/fortress-install.v1').write_text('FORTRESS-INSTALL-1\n'+identity+'payload_version=1\n')
        root=out/'root.img'
        with root.open('xb') as stream:stream.truncate(fixture.ROOT_BYTES)
        run(['mke2fs','-q','-t','ext4','-b','4096','-I','256','-U',str(root_uuid),'-O',fixture.FEATURES,'-E','lazy_itable_init=0','-m','0','-g','4096','-N','4096','-J','size=4','-d',str(stage),str(root)])
        commands=['set_inode_field / mode 040755','set_inode_field / uid 0','set_inode_field / gid 0']
        for p in sorted(stage.rglob('*')):
            name='/'+p.relative_to(stage).as_posix();mode,uid,gid=metadata.get(name,(0o755 if p.is_dir() else 0o644,0,0))
            mode|=0o40000 if p.is_dir() else 0o100000
            commands += [f'set_inode_field {name} mode 0{mode:o}',f'set_inode_field {name} uid {uid}',f'set_inode_field {name} gid {gid}']
        attrs=out/'attrs.commands';attrs.write_text('\n'.join(commands)+'\n');run(['debugfs','-w','-f',str(attrs),str(root)])
        fixture.initialize_journal(root,out);run(['e2fsck','-fn',str(root)])
        run(['make','test-root-boot-host','test-perm-db-host','test-elf-page-host','test-perm-spawn-host','test-perm-capability-host','test-perm-privileges-host','test-usb-mount-host'],300)
        binary=out/'host'
        run(['gcc','-std=c11','-O1','-g','-fsanitize=address,undefined','-Wall','-Wextra','-Werror','-no-pie','-pthread','-DTEST_ROOT_PHASE2_ACCOUNTS',
             '-Itests/ext4_host','-Itests/host','-Isrc/include','-Isrc/fs','-Isrc/drivers','-Isrc/mm','-Isrc/kernel','tests/root_phase1_host.c','tests/ext4_fault_disk.c','-o',str(binary)])
        for sector in (() if args.skip_host else (512,4096)):
            copy=out/f'host-{sector}.img';run(['cp','--sparse=always',str(root),str(copy)])
            run([str(binary),str(copy),str(sector),str(stage/'bin/login')],600);run(['e2fsck','-fn',str(copy)])
            manifest['cases'].append({'host_sector':sector,'result':'PASS'});print('PASS host',sector,flush=True)
        bootstrap=out/'bootstrap.tar'
        with tarfile.open(workspace/'bin/initramfs.tar') as src,tarfile.open(bootstrap,'w',format=tarfile.USTAR_FORMAT) as dst:
            for m in src:dst.addfile(m,src.extractfile(m) if m.isfile() else None)
            m=tarfile.TarInfo('bootstrap');m.type=tarfile.DIRTYPE;m.mode=0o700;dst.addfile(m)
            data=('FORTRESS-RECOVERY-1\n'+identity+'password_hash='+fixture.crypt_hash(secret,'$5$rounds=100000$'+secrets.token_hex(8))+'\n').encode()
            m=tarfile.TarInfo('bootstrap/recovery.v1');m.mode=0o600;m.size=len(data);dst.addfile(m,io.BytesIO(data))
        ready=('FORTRESS-READY-1\n'+identity+'payload_version=1\nkernel_sha256='+fixture.digest(workspace/'bin/fortress.elf')+'\nbootstrap_sha256='+fixture.digest(bootstrap)+'\n').encode()
        ready+=('checksum='+hashlib.sha256(ready).hexdigest()+'\n').encode();assert len(ready)<512
        readiness=out/'readiness.v1';readiness.write_bytes(ready.ljust(512,b'\0'))
        config=out/'limine.conf';config.write_text('timeout: 0\n/FortressOS installed fixture\n    protocol: limine\n    kernel_path: boot():/boot/fortress.elf\n    kernel_cmdline: boot_mode=installed root=PARTUUID='+str(partuuid)+'\n    module_path: boot():/boot/bootstrap.tar\n    module_cmdline: role=bootstrap\n    module_path: boot():/boot/readiness.v1\n    module_cmdline: role=readiness\n')
        esp=out/'esp.img';fixture.create_esp_partition(esp,None,ROOT/'limine',workspace/'bin/fortress.elf',bootstrap,config,None)
        run(['mren','-i',str(esp),'::/boot/initramfs.tar','bootstrap.tar']);run(['mcopy','-i',str(esp),str(readiness),'::/boot/readiness.v1']);run(['fsck.fat','-n',str(esp)])
        template,record=fixture.assemble(out,esp,root,384,partuuid);manifest['disk']=record
        credential=out/'recovery-record.v1'
        with tarfile.open(bootstrap) as src:credential.write_bytes(src.extractfile('bootstrap/recovery.v1').read())
        if not args.skip_host:
            admission=out/'admission-host'
            run(['gcc','-std=c11','-O1','-g','-fsanitize=address,undefined','-Wall','-Wextra','-Werror','-no-pie','-pthread',
                '-Itests/ext4_host','-Itests/host','-Isrc/include','-Isrc/lib','-Isrc/fs','-Isrc/drivers','-Isrc/mm','-Isrc/kernel',
                'tests/root_phase2_admission_host.c','tests/ext4_fault_disk.c','src/kernel/installed_root.c','src/kernel/root_boot.c','user/tools/userdb.c','user/tools/digest.c','-o',str(admission)])
            for kind in ('multiple','inventory','namespaces','truncated','flush','replay','success','freeze-taint'):
                disk=out/('admission-'+kind+'.img');run(['cp','--sparse=always',str(template),str(disk)])
                if kind=='replay':
                    run([str(admission),str(disk),str(workspace/'bin/fortress.elf'),str(bootstrap),str(readiness),str(credential),str(partuuid),'seed-replay'])
                before=fixture.digest(disk)
                run([str(admission),str(disk),str(workspace/'bin/fortress.elf'),str(bootstrap),str(readiness),str(credential),str(partuuid),kind],600)
                after=fixture.digest(disk)
                if kind in ('multiple','inventory','namespaces','truncated','flush','replay'):assert before==after,kind
                if kind=='success':
                    extracted=out/'admission-success-root.img';extract_root(disk,extracted);run(['e2fsck','-fn',str(extracted)])
                    proof=out/'admission-proof.txt';run(['debugfs','-R',f'dump /root/host-proof {proof}',str(extracted)]);assert proof.read_bytes()==b'actual admitted root\n'
                manifest['cases'].append({'host_admission':kind,'result':'PASS','before_sha256':before,'after_sha256':after})
        positives=[]
        for mode in (() if args.host_only else ('bios','uefi')):
            for cpus in ((1,) if args.quick else (1,4,8)):
                disk=out/f'{mode}-{cpus}.img';run(['cp','--sparse=always',str(template),str(disk)])
                positives.append((disk,mode,cpus))
        def three_boots(case):
            disk,mode,cpus=case
            for boot in range(3):guest(out,disk,mode,cpus,boot,operator,secret,manifest)
        parallel(three_boots,positives,args.jobs)
        negatives=[]
        for mode in (() if args.host_only else ('bios',) if args.quick else ('bios','uefi')):
            for cpus in ((1,) if args.quick else (1,4,8)):
                for kind in ('missing','degraded','conflict','duplicate','unsupported','manifest','readiness','login')+(('flush','replay') if not args.skip_host else ()):
                    seed=out/'admission-replay.img' if kind=='replay' else template
                    disk=out/f'{mode}-{cpus}-{kind}.img';run(['cp','--sparse=always',str(seed),str(disk)])
                    # Each mutation has its own diagnostic directory and files.
                    work=out/f'{mode}-{cpus}-{kind}-mutate';work.mkdir()
                    corrupt(disk,kind,work,run);negatives.append((disk,mode,cpus,kind))
        def rejection(case):
            disk,mode,cpus,kind=case;before=fixture.digest(disk)
            result=guest(out,disk,mode,cpus,kind,operator,secret,manifest,recovery=True)
            after=fixture.digest(disk)
            if kind in ('missing','degraded','conflict','duplicate','unsupported','readiness','flush','replay'):assert before==after,(kind,'rejection wrote disk')
            result.update(before_sha256=before,after_sha256=after)
        parallel(rejection,negatives,args.jobs)
        for mode in (() if args.host_only else ('bios','uefi')):
            disk=out/(mode+'-login-publication.img');run(['cp','--sparse=always',str(template),str(disk)])
            guest(out,disk,mode,4,'login-publication',operator,secret,manifest,recovery=True)
        for mode in (() if args.host_only else ('bios','uefi')):
            disk=out/(mode+'-multiple.img');run(['cp','--sparse=always',str(template),str(disk)])
            extra=out/(mode+'-excluded.img')
            with extra.open('xb') as f:f.truncate(1024*1024)
            before=fixture.digest(disk);other=fixture.digest(extra)
            guest(out,disk,mode,1,'multiple',operator,secret,manifest,recovery=True,extra=extra)
            assert before==fixture.digest(disk) and other==fixture.digest(extra)
        if not args.host_only:
            secret2=secrets.token_hex(16);salt2=secrets.token_hex(8);identity2=f'install_id={secrets.token_hex(16)}\nroot_partuuid={uuid.uuid4()}\nfilesystem_uuid={root_uuid}\n'
            for kind in ('explicit-recovery','missing-credential','malformed-credential','second-recovery'):
                archive=out/(kind+'.tar')
                with tarfile.open(bootstrap) as src,tarfile.open(archive,'w',format=tarfile.USTAR_FORMAT) as dst:
                    for m in src:
                        data=src.extractfile(m).read() if m.isfile() else None
                        if m.name=='bootstrap/recovery.v1':
                            if kind=='missing-credential':continue
                            if kind=='malformed-credential':data=b'invalid\n'
                            if kind=='second-recovery':data=('FORTRESS-RECOVERY-1\n'+identity2+'password_hash='+fixture.crypt_hash(secret2,'$5$rounds=100000$'+salt2)+'\n').encode()
                            m.size=len(data)
                        dst.addfile(m,io.BytesIO(data) if data is not None else None)
                alternate=out/(kind+'-esp.img');shutil.copyfile(esp,alternate)
                run(['mcopy','-o','-i',str(alternate),str(archive),'::/boot/bootstrap.tar'])
                text=config.read_text()
                if kind in ('explicit-recovery','second-recovery'):text=text.replace('boot_mode=installed','boot_mode=recovery')
                if kind=='second-recovery':text=text.replace(str(partuuid),re.search(r'root_partuuid=(.*)',identity2)[1])
                alt_config=out/(kind+'.conf');alt_config.write_text(text)
                for path in ('::/boot/limine/limine.conf','::/boot/limine.conf','::/EFI/BOOT/limine.conf','::/limine.conf'):
                    run(['mcopy','-o','-i',str(alternate),str(alt_config),path])
                for mode in ('bios','uefi'):
                    disk=out/(mode+'-'+kind+'.img');run(['cp','--sparse=always',str(template),str(disk)])
                    with disk.open('r+b') as dst:fixture.sparse_copy(alternate,dst,fixture.ESP_START*512)
                    before=fixture.digest(disk)
                    guest(out,disk,mode,1,kind,operator,secret2 if kind=='second-recovery' else secret,manifest,recovery=True,
                        denied=kind in ('missing-credential','malformed-credential'),wrong_secret=secret if kind=='second-recovery' else secret2)
                    assert before==fixture.digest(disk),(kind,'recovery entry wrote disk')
            live_tree=out/'live-iso';(live_tree/'boot/limine').mkdir(parents=True);(live_tree/'EFI/BOOT').mkdir(parents=True)
            shutil.copyfile(workspace/'bin/fortress.elf',live_tree/'boot/fortress.elf');shutil.copyfile(workspace/'bin/initramfs.tar',live_tree/'boot/initramfs.tar')
            for name in ('limine-bios.sys','limine-bios-cd.bin','limine-uefi-cd.bin'):shutil.copyfile(ROOT/'limine'/name,live_tree/'boot/limine'/name)
            shutil.copyfile(ROOT/'limine/BOOTX64.EFI',live_tree/'EFI/BOOT/BOOTX64.EFI')
            live_config='timeout: 0\n/FortressOS live fixture\n    protocol: limine\n    kernel_path: boot():/boot/fortress.elf\n    kernel_cmdline: boot_mode=live\n    module_path: boot():/boot/initramfs.tar\n'
            for path in ('boot/limine/limine.conf','boot/limine.conf','EFI/BOOT/limine.conf','limine.conf'):(live_tree/path).write_text(live_config)
            iso=out/'live.iso';run(['xorriso','-as','mkisofs','-b','boot/limine/limine-bios-cd.bin','-no-emul-boot','-boot-load-size','4','-boot-info-table','--efi-boot','boot/limine/limine-uefi-cd.bin','-efi-boot-part','--efi-boot-image','--protective-msdos-label',str(live_tree),'-o',str(iso)])
            run([str(ROOT/'limine/limine'),'bios-install',str(iso)])
            for mode in ('bios','uefi'):
                guest(out,None,mode,1,'live-absent',operator,secret,manifest,live_iso=iso)
                bad=out/(mode+'-live-unusable.img')
                with bad.open('xb') as f:f.truncate(1024*1024)
                before=fixture.digest(bad);guest(out,None,mode,1,'live-unusable',operator,secret,manifest,extra=bad,live_iso=iso)
                assert before==fixture.digest(bad)
        assert all(fixture.digest(Path(p))==h for p,h in normal.items()),'live artifacts changed'
        assert all(fixture.digest(ROOT/p)==h for p,h in sources.items()),'sources changed during gate'
        manifest['result']='PASS';print('PASS',manifest['scope'],':',out,flush=True)
    except BaseException as exc:manifest.update(result='FAIL',error=type(exc).__name__+': '+str(exc));raise
    finally:
        manifest.update(normal_artifacts=normal,sources=sources)
        (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')

def corrupt(disk,kind,out,run):
    with disk.open('r+b') as f:
        sectors=disk.stat().st_size//512
        if kind in ('missing','degraded','conflict','duplicate'):
            for lba in (1,sectors-1):
                f.seek(lba*512);header=bytearray(f.read(512));array=struct.unpack_from('<Q',header,72)[0]
                f.seek(array*512);table=bytearray(f.read(16384))
                if kind=='missing':table[128:256]=bytes(128)
                if kind=='duplicate':table[16:32]=table[144:160]
                if kind=='conflict' and lba==sectors-1:header[56]^=1
                struct.pack_into('<I',header,88,__import__('binascii').crc32(table))
                struct.pack_into('<I',header,16,0);struct.pack_into('<I',header,16,__import__('binascii').crc32(header[:92]))
                if kind=='degraded' and lba==sectors-1:header[0]=0
                f.seek(array*512);f.write(table);f.seek(lba*512);f.write(header)
            return
        if kind=='unsupported':
            f.seek(fixture.ROOT_START*512+1024);sb=bytearray(f.read(1024));struct.pack_into('<I',sb,96,0x242)
            struct.pack_into('<I',sb,1020,fixture.crc32c(sb[:1020]));f.seek(fixture.ROOT_START*512+1024);f.write(sb);return
    if kind in ('manifest','login'):
        root=out/(kind+'-root-input.img');extract_root(disk,root)
        data=out/(kind+'-invalid');data.write_bytes(b'invalid\n')
        path='/etc/fortress-install.v1' if kind=='manifest' else '/bin/login'
        cmds=out/(kind+'-mutate.commands');cmds.write_text(f'rm {path}\nwrite {data} {path}\nset_inode_field {path} mode 0100755\n')
        run(['debugfs','-w','-f',str(cmds),str(root)])
        with disk.open('r+b') as dst:fixture.sparse_copy(root,dst,fixture.ROOT_START*512)
    elif kind=='readiness':
        esp=out/'bad-ready-esp.img'
        with disk.open('rb') as src,esp.open('xb') as dst:src.seek(fixture.ESP_START*512);dst.write(src.read(fixture.ESP_SECTORS*512))
        data=out/'bad-readiness.v1';data.write_bytes(bytes(512));run(['mcopy','-o','-i',str(esp),str(data),'::/boot/readiness.v1'])
        with disk.open('r+b') as dst:fixture.sparse_copy(esp,dst,fixture.ESP_START*512)

def extract_root(disk,out):
    with disk.open('rb') as src,out.open('xb') as dst:
        src.seek(fixture.ROOT_START*512);remaining=fixture.ROOT_BYTES
        while remaining:
            b=src.read(min(1024*1024,remaining));assert b;dst.write(b);remaining-=len(b)

def guest(out,disk,mode,cpus,boot,operator,secret,manifest,recovery=False,extra=None,denied=False,wrong_secret='incorrect-recovery-secret',live_iso=None):
    label=f'{mode}-{cpus}-{boot}';argv=['qemu-system-x86_64','-M','q35','-m','2G','-accel','tcg','-smp',str(cpus),'-display','none','-monitor','none','-no-reboot','-net','none','-serial','stdio']
    if live_iso:
        assert live_iso.is_file() and live_iso.resolve().parent==out.resolve();argv+=['-cdrom',str(live_iso)]
    else:
        # OVMF repairs an invalid backup GPT before booting the kernel when the
        # backend is writable. Keep this degraded-policy fixture immutable.
        protect=',readonly=on' if mode=='uefi' and boot=='degraded' else ''
        argv+=['-drive',f'file={disk},if=none,id=target,format=raw'+protect,'-device','nvme,drive=target,serial=ROOTPHASE2,bootindex=1']
    if boot=='login-publication':argv+=['-fw_cfg','name=opt/fortress/root_login_fail,string=1']
    if boot in ('flush','replay'):argv+=['-fw_cfg',f'name=opt/fortress/root_{boot}_fail,string=1']
    if extra is not None:
        assert extra.is_file() and extra.resolve().parent==out.resolve()
        argv+=['-drive',f'file={extra},if=none,id=excluded,format=raw','-device','nvme,drive=excluded,serial=EXCLUDED']
    if mode=='uefi':
        variables=out/(label+'-vars.fd');shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
        argv+=['-drive','if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd','-drive',f'if=pflash,format=raw,unit=1,file={variables}']
    if not live_iso:assert disk.is_file() and disk.resolve().parent==out.resolve()
    assert argv.count('-device')==(0 if live_iso else 1)+(1 if extra else 0)
    assert argv.count('-drive')==(2 if mode=='uefi' else 0)+(0 if live_iso else 1)+(1 if extra else 0)
    (out/(label+'-argv.json')).write_text(json.dumps(argv,indent=2));transcript=bytearray();err=(out/(label+'-stderr.log')).open('wb')
    proc=subprocess.Popen(argv,cwd=ROOT,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=err);sel=selectors.DefaultSelector();sel.register(proc.stdout,selectors.EVENT_READ)
    clean=lambda b:re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]','',b.decode(errors='replace')).replace('\r','')
    def wait(predicate,start=0,timeout=240):
        deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            assert not CANCEL.is_set(),'gate cancelled; QEMU cleanup follows'
            for key,_ in sel.select(.1):
                data=key.fileobj.read1(65536)
                if data:transcript.extend(data)
            (out/(label+'-serial.log')).write_bytes(transcript)
            # Live boot retains its deliberate legacy user-fault isolation test.
            fatal=('[FATAL]','KERNEL PANIC') if live_iso else ('[FATAL]','KERNEL PANIC','[PROCESS FAULT]')
            assert not any(s in clean(transcript) for s in fatal),clean(transcript[-6000:])
            if predicate(clean(transcript[start:])):return
            assert proc.poll() is None,clean(transcript[-6000:])
        raise TimeoutError(clean(transcript[-6000:]))
    def send(s):proc.stdin.write((s+'\n').encode());proc.stdin.flush()
    prompt=lambda t:re.search(r'(?:^|\n)[#$] $',t)
    def command(cmd,expected):
        start=len(transcript);send(cmd);wait(lambda t:expected in t and prompt(t),start)
    completed=False
    try:
        if denied:
            wait(lambda t:'[ROOT RECOVERY] invalid independent recovery credential' in t)
            assert 'FortressOS login: ' not in clean(transcript) and 'Recovery passphrase: ' not in clean(transcript)
            completed=True
        elif recovery:
            wait(lambda t:'Recovery passphrase: ' in t);assert 'FortressOS login: ' not in clean(transcript)
            start=len(transcript);send(wrong_secret);wait(lambda t:'Recovery denied' in t and 'Recovery passphrase: ' in t,start)
            start=len(transcript);send(secret);wait(lambda t:'Recovery authenticated' in t and prompt(t),start)
            command('id','uid=0 gid=0');completed=True
        else:
            wait(lambda t:'FortressOS login: ' in t)
            if not live_iso:assert '[ROOT] Installed root admitted by PARTUUID' in clean(transcript)
            else:assert '[ROOT] Installed root admitted' not in clean(transcript) and '[NVMe] Namespace Geometry' not in clean(transcript)
            assert ('All '+str(cpus)+' CPU(s) accounted for' in clean(transcript)) if cpus>1 else 'Single-CPU system confirmed' in clean(transcript)
            send('operator')
            if not live_iso:wait(lambda t:t.endswith('Password: '));send(operator)
            wait(prompt)
            command('pwd','/run/user/1000' if live_iso else '/home/operator');command('id','uid=1000 gid=1000');command('cat /etc/shadow','permission denied')
            if not live_iso:
                if boot:command('cat phase2.txt',f'phase2-boot-{boot-1}')
                command(f'echo phase2-boot-{boot} > phase2.txt','');command('sync','')
                command('echo SYNC_STATUS=$?','SYNC_STATUS=0')
                command('sha256sum phase2.txt',hashlib.sha256(f'phase2-boot-{boot}\n'.encode()).hexdigest())
            start=len(transcript);send('sudo id')
            if not live_iso:wait(lambda t:t.endswith('sudo password: '),start);send(operator)
            wait(lambda t:'uid=0 gid=0' in t and prompt(t),start)
            if not live_iso and boot==0:
                for exit_status in (7,125):
                    start=len(transcript);send(f'exit {exit_status}');wait(lambda t:'FortressOS login: ' in t,start)
                    assert '[ROOT RECOVERY]' not in clean(transcript[start:])
                    send('operator');wait(lambda t:t.endswith('Password: '));send(operator);wait(prompt)
                command('pwd','/home/operator')
            if boot==1:
                # An abrupt whole-guest stop after a successful explicit barrier
                # retains dirty+RECOVER; next boot must admit journal recovery.
                proc.kill();assert proc.wait(timeout=10)!=0;completed=True
            else:
                start=len(transcript);send('sudo shutdown')
                if not live_iso:wait(lambda t:t.endswith('sudo password: '),start);send(operator)
                wait(lambda t:'[POWER] Shutdown initiated' in t,start);assert proc.wait(timeout=30)==0;completed=True
        assert all(s.encode() not in transcript for s in (secret,operator,wrong_secret)),'plaintext secret echoed'
        result={'firmware':mode,'smp':cpus,'boot':boot,'recovery':recovery,'live':bool(live_iso),'credential_denied':denied,'result':'PASS','argv':label+'-argv.json'}
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:proc.wait(timeout=5)
            except subprocess.TimeoutExpired:proc.kill();proc.wait(timeout=5)
        transcript.extend(proc.stdout.read());sel.close();err.close();(out/(label+'-serial.log')).write_bytes(transcript)
        if completed and not recovery and not live_iso:
            root=out/(label+'-root.img');extract_root(disk,root)
            if boot!=1:fixture.run(['e2fsck','-fn',str(root)],out/(label+'-fsck.log'))
            else:
                with root.open('rb') as f:f.seek(1024);sb=f.read(1024)
                assert struct.unpack_from('<H',sb,58)[0]==0 and struct.unpack_from('<I',sb,96)[0]&4
            dumped=out/(label+'-probe.txt');fixture.run(['debugfs','-R',f'dump /home/operator/phase2.txt {dumped}',str(root)],out/(label+'-audit.log'))
            assert dumped.read_bytes()==f'phase2-boot-{boot}\n'.encode()
            metadata=fixture.run(['debugfs','-R','stat /home/operator/phase2.txt',str(root)],out/(label+'-audit.log'));assert 'User:  1000' in metadata and 'Group:  1000' in metadata
    manifest['cases'].append(result);print('PASS guest',label,flush=True)
    return result

if __name__=='__main__':main()
