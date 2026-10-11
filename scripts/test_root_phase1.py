#!/usr/bin/env python3
"""Phase1 actual-code host and BIOS/UEFI frozen-root gates; fresh regular files only.
No installer image, production admission, mutable-account or physical write claim.
"""
import argparse, json, re, secrets, selectors, shutil, subprocess, tarfile, tempfile, time
from pathlib import Path
import test_root_phase0 as fixture
ROOT=fixture.REPO

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--host-only',action='store_true');args=parser.parse_args()
    parent=ROOT/'build/root-phase1';parent.mkdir(parents=True,exist_ok=True)
    out=Path(tempfile.mkdtemp(prefix='gate-',dir=parent));manifest={'commands':fixture.COMMANDS,'cases':[]}
    normal={str(p):fixture.digest(p) for p in [ROOT/'bin/fortress.img',ROOT/'bin/fortress.iso'] if p.is_file()}
    log=out/'commands.log'
    run=lambda cmd,timeout=180: fixture.run(cmd,log,timeout)
    secret=secrets.token_hex(16)
    sources={p:fixture.digest(ROOT/p) for p in ['src/fs/vfs.c','src/fs/ext4.c','src/fs/ext4_permissions.inc',
            'src/fs/ext4_mount_journal.inc','src/kernel/thread.c','src/kernel/main.c','tests/root_phase1_host.c','scripts/test_root_phase1.py']}
    try:
        stage=out/'tree';stage.mkdir();metadata={};names=set()
        with tarfile.open(ROOT/'bin/initramfs.tar') as src:
            for m in src:
                parts=Path(m.name).parts
                assert not m.name.startswith('/') and '..' not in parts and (m.isdir() or m.isfile())
                assert re.fullmatch(r'[A-Za-z0-9_./-]+',m.name) and len(m.name)<256 and all(len(p)<64 for p in parts)
                name=Path(m.name).as_posix();assert name not in names and len(names)<512;names.add(name)
                target=stage/name;assert target.resolve().is_relative_to(stage.resolve())
                if m.isdir():target.mkdir(parents=True,exist_ok=True)
                else:
                    target.parent.mkdir(parents=True,exist_ok=True)
                    with target.open('xb') as f:shutil.copyfileobj(src.extractfile(m),f)
                metadata['/'+name]=(m.mode,m.uid,m.gid)
        # Preserve existing read-only account consumers and live-style runtime homes.
        # This fixture proves persistent executable plumbing, not installed accounts.
        (stage/'etc/shadow').write_text('root:!:::::::\noperator:'+fixture.crypt_hash(secret,'$5$'+secrets.token_hex(8))+':::::::\n')
        metadata['/etc/shadow']=(0o600,0,0)
        (stage/'bin/bad-elf').write_bytes(b'not an ELF\n');metadata['/bin/bad-elf']=(0o755,0,0)
        for name in ('dev','run','tmp','mnt','home','var','root'):(stage/name).mkdir(exist_ok=True)
        for name in ('dev','run','tmp'):(stage/name/'hidden').write_bytes(b'persistent contents must be hidden\n')
        for i in range(1100):(stage/f'cache-{i}').write_bytes(b'cache\n')
        root=out/'root.img'
        with root.open('xb') as f:f.truncate(fixture.ROOT_BYTES)
        run(['mke2fs','-q','-t','ext4','-b','4096','-I','256','-O',fixture.FEATURES,'-E','lazy_itable_init=0',
             '-m','0','-g','4096','-N','4096','-J','size=4','-d',str(stage),str(root)])
        commands=['set_inode_field / mode 040755','set_inode_field / uid 0','set_inode_field / gid 0']
        for p in sorted(stage.rglob('*')):
            name='/'+p.relative_to(stage).as_posix();mode,uid,gid=metadata.get(name,(0o755 if p.is_dir() else 0o644,0,0))
            mode|=0o40000 if p.is_dir() else 0o100000
            commands += [f'set_inode_field {name} mode 0{mode:o}',f'set_inode_field {name} uid {uid}',f'set_inode_field {name} gid {gid}']
        attrs=out/'attrs.commands';attrs.write_text('\n'.join(commands)+'\n')
        run(['debugfs','-w','-f',str(attrs),str(root)]);fixture.initialize_journal(root,out)
        run(['e2fsck','-fn',str(root)])
        run(['make','test-elf-page-host','test-perm-spawn-host'],240)
        binary=out/'host'
        run(['gcc','-std=c11','-O1','-g','-fsanitize=address,undefined','-Wall','-Wextra','-Werror','-no-pie','-pthread',
             '-Itests/ext4_host','-Itests/host','-Isrc/include','-Isrc/fs','-Isrc/drivers','-Isrc/mm','-Isrc/kernel',
             'tests/root_phase1_host.c','tests/ext4_fault_disk.c','-o',str(binary)])
        for sector in (512,4096):
            copy=out/f'host-{sector}.img';run(['cp','--sparse=always',str(root),str(copy)])
            run([str(binary),str(copy),str(sector),str(stage/'bin/login')],600);run(['e2fsck','-fn',str(copy)])
            manifest['cases'].append({'host_sector':sector,'result':'PASS'})
            print('PASS host',sector,flush=True)
        if not args.host_only:
            workspace=Path(run(['python3','scripts/create_ext4_guest_workspace.py']).strip())
            manifest['workspace']=str(workspace)
            with (out/'build.log').open('w') as f:
                cmd=['make','-j4','ROOT_PHASE1_TEST=1','bin/fortress.elf','bin/initramfs.tar']
                fixture.COMMANDS.append(cmd)
                subprocess.run(cmd,cwd=workspace,stdout=f,stderr=subprocess.STDOUT,check=True,timeout=240)
            bootstrap=out/'bootstrap.tar';shutil.copyfile(workspace/'bin/initramfs.tar',bootstrap)
            config=out/'limine.conf';config.write_text('timeout: 0\n/Phase1 disposable root\n    protocol: limine\n    kernel_path: boot():/boot/fortress.elf\n    module_path: boot():/boot/initramfs.tar\n')
            esp=out/'esp.img';fixture.create_esp_partition(esp,None,ROOT/'limine',workspace/'bin/fortress.elf',bootstrap,config,None)
            run(['fsck.fat','-n',str(esp)])
            template,record=fixture.assemble(out,esp,root,384);manifest['disk']=record
            for mode in ('bios','uefi'):
                for cpus in (1,4,8):
                    guest(out,template,mode,cpus,secret,manifest)
        assert all(fixture.digest(Path(p))==h for p,h in normal.items()),'live artifacts changed'
        assert all(fixture.digest(ROOT/p)==h for p,h in sources.items()),'sources changed during gate'
        manifest['result']='PASS';print('PASS Phase1 gates:',out,flush=True)
    except BaseException as exc:
        manifest['result']='FAIL';manifest['error']=type(exc).__name__+': '+str(exc);raise
    finally:
        manifest['normal_artifacts']=normal
        manifest['sources']=sources
        (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')

def guest(out,template,mode,cpus,secret,manifest):
    label=f'{mode}-{cpus}';disk=out/(label+'.img');shutil.copyfile(template,disk)
    argv=['qemu-system-x86_64','-M','q35','-m','2G','-accel','tcg','-smp',str(cpus),'-display','none','-monitor','none',
          '-no-reboot','-net','none','-serial','stdio','-fw_cfg','name=opt/fortress/root_phase1,string=1',
          '-drive',f'file={disk},if=none,id=target,format=raw','-device','nvme,drive=target,serial=ROOTPHASE1,bootindex=1']
    if mode=='uefi':
        variables=out/(label+'-vars.fd');shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
        argv+=['-drive','if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
               '-drive',f'if=pflash,format=raw,unit=1,file={variables}']
    assert disk.is_file() and disk.parent==out and '-cdrom' not in argv and '-blockdev' not in argv
    assert argv.count('-device')==1 and argv.count('-drive')==(3 if mode=='uefi' else 1)
    (out/(label+'-argv.json')).write_text(json.dumps(argv,indent=2))
    transcript=bytearray();err=(out/(label+'-stderr.log')).open('wb')
    proc=subprocess.Popen(argv,cwd=ROOT,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=err)
    sel=selectors.DefaultSelector();sel.register(proc.stdout,selectors.EVENT_READ)
    def clean(b):return re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]','',b.decode(errors='replace')).replace('\r','')
    def wait(predicate,start=0,timeout=240):
        end=time.monotonic()+timeout
        while time.monotonic()<end:
            for key,_ in sel.select(.1):
                data=key.fileobj.read1(65536)
                if data:transcript.extend(data)
            (out/(label+'-serial.log')).write_bytes(transcript)
            text=clean(transcript[start:])
            assert not any(p in clean(transcript) for p in ('[FAIL]','[FATAL]','KERNEL PANIC','[PROCESS FAULT]')),clean(transcript[-6000:])
            if predicate(text):return text
            assert proc.poll() is None,clean(transcript[-6000:])
        raise TimeoutError(clean(transcript[-6000:]))
    def send(s):proc.stdin.write((s+'\n').encode());proc.stdin.flush()
    def prompt(t):return re.search(r'(?:^|\n)[#$] $',t)
    def execute(cmd,expected):
        start=len(transcript);send(cmd);return wait(lambda t:expected in t and prompt(t),start)
    try:
        wait(lambda t:'100 spawn/exit cycles, zero resource leaks PASS' in t and 'snapshot OOM/short read, pinned saturation, cleanup PASS' in t and 'FortressOS login: ' in t)
        assert ('All '+str(cpus)+' CPU(s) accounted for' in clean(transcript)) if cpus>1 else 'Single-CPU system confirmed' in clean(transcript)
        send('operator');wait(lambda t:t.endswith('Password: '));send(secret);wait(prompt)
        execute('id','uid=1000 gid=1000');execute('pwd','/run/user/1000')
        execute('/bin/bad-elf','Invalid executable.')
        execute('cat /etc/shadow','permission denied')
        start=len(transcript);send('sudo id');wait(lambda t:t.endswith('sudo password: '),start);send(secret)
        wait(lambda t:'uid=0 gid=0 groups=0' in t and prompt(t),start)
        # Subsequent user spawns use normal scheduling on SMP configurations.
        for _ in range(20):execute('/bin/sh-builtin echo PHASE1_CHILD','PHASE1_CHILD')
        assert secret.encode() not in transcript,'plaintext secret echoed'
        manifest['cases'].append({'firmware':mode,'smp':cpus,'result':'PASS','argv':label+'-argv.json'})
        print('PASS guest',label,flush=True)
    finally:
        proc.terminate()
        try:proc.wait(timeout=5)
        except subprocess.TimeoutExpired:proc.kill();proc.wait(timeout=5)
        transcript.extend(proc.stdout.read());sel.close();err.close()
        (out/(label+'-serial.log')).write_bytes(transcript)
        # Freeze ran before login; audit the actual guest-mutated root independently.
        extracted=out/(label+'-root.img')
        with disk.open('rb') as src,extracted.open('xb') as dst:
            src.seek(fixture.ROOT_START*512);remaining=fixture.ROOT_BYTES
            while remaining:
                b=src.read(min(1024*1024,remaining));assert b;dst.write(b);remaining-=len(b)
        fixture.run(['e2fsck','-fn',str(extracted)],out/(label+'-fsck.log'))

if __name__=='__main__':main()
