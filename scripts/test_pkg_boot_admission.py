"""Private measured-module boot admission, disposable GPT/NVMe only."""
import argparse,datetime,hashlib,json,pathlib,re,shutil,socket,subprocess,tarfile,tempfile,threading,time
from pkg_boot_admission_overlay import overlay
from pkg_enroll_fixture import create,gpt
from pkg_enroll_records import build,write_fresh_records
from pkg_boot_journal_fixture import pending
from test_getrandom import CODE,VARS,PROMPT
ROOT=pathlib.Path(__file__).resolve().parents[1]
H=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
CONCURRENT_WLAN={'src/net/wlan/net_80211.c','src/net/wlan/wlan_mgr.c','tests/wifi_host.c','tests/wlan_mac_host.c','user/wifi.c'}
extra_cases=None

def boot(out,work,iso,source,firmware,smp,label,refused=False,expected=None,sequence=1):
    dest=out/label;dest.mkdir();disk=dest/'enrolled.img';shutil.copyfile(source,disk);initial=H(disk)
    assert disk.is_file() and not disk.is_symlink() and disk.stat().st_size==64*1024*1024 and disk.resolve().parent==dest.resolve()
    with tempfile.TemporaryDirectory(prefix='fortress-pkgboot-') as tmp:
        uartpath=pathlib.Path(tmp)/'uart';stop=threading.Event();drainer=None
        argv=['qemu-system-x86_64','-M','q35','-accel','tcg','-cpu','max','-m','512M','-smp',str(smp),'-display','none','-net','none','-no-reboot','-boot','d','-rtc','base=2026-10-24T12:00:00','-cdrom',str(iso),'-chardev',f'socket,id=uart,path={uartpath},server=on,wait=off,logfile={dest/"serial.log"}','-serial','chardev:uart','-drive',f'if=none,format=raw,id=pkgboot,file={disk}','-device','nvme,drive=pkgboot,serial=FORTRESS-PKGBOOT','-fw_cfg','name=opt/fortress/pkg_boot_nvme,string=private-regular-file-only']
        if firmware=='uefi':
            shutil.copyfile(VARS,dest/'vars.fd');argv+=['-drive',f'if=pflash,format=raw,unit=0,readonly=on,file={CODE}','-drive',f'if=pflash,format=raw,unit=1,file={dest/"vars.fd"}']
        if 'read-error' in label:argv+=['-fw_cfg','name=opt/fortress/pkg_boot_read_error,string=injected']
        assert not any(x in argv for x in ('-hda','-hdb','-blockdev','-readconfig'))
        for i,a in enumerate(argv):
            if a=='-drive':assert argv[i+1]==f'if=none,format=raw,id=pkgboot,file={disk}' or argv[i+1].startswith('if=pflash,format=raw,unit=')
        (dest/'argv.json').write_text(json.dumps(argv,indent=2)+'\n');uart=socket.socket(socket.AF_UNIX)
        with (dest/'stderr.log').open('w') as err:p=subprocess.Popen(argv,cwd=work,stderr=err)
        def text():
            f=dest/'serial.log';return re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]','',f.read_text(errors='replace') if f.exists() else '').replace('\r','')
        def wait(predicate,seconds=180):
            until=time.monotonic()+seconds
            while time.monotonic()<until:
                s=text();assert not any(x in s for x in ('[PANIC]','[FATAL]','PKGBOOT RING3 FAIL')),s[-5000:]
                if predicate(s):return s
                assert p.poll() is None,(p.returncode,s[-4000:]);time.sleep(.1)
            raise AssertionError('timeout '+text()[-5000:])
        try:
            until=time.monotonic()+10
            while True:
                try:uart.connect(str(uartpath));break
                except (FileNotFoundError,ConnectionRefusedError):assert time.monotonic()<until;time.sleep(.1)
            uart.settimeout(.2)
            def drain():
                while not stop.is_set():
                    try:
                        if not uart.recv(65536):return
                    except socket.timeout:pass
                    except OSError:return
            drainer=threading.Thread(target=drain,daemon=True);drainer.start()
            wait(lambda s:'FortressOS login: ' in s or PROMPT.search(s))
            s=text();assert ('PKGBOOT REFUSED' if refused else f'PKGBOOT ADMITTED IDLE={sequence} detached') in s,s[-4000:]
            if refused:assert re.search(r'PKGBOOT REFUSED[^\n]*writes=0 flushes=0',s)
            else:assert 'PKGBOOT PROJECTED AUTHENTICATED before-writes=0' in s
            if not PROMPT.search(s):uart.sendall(b'operator\n')
            wait(lambda s:PROMPT.search(s));start=len(text());uart.sendall(b'sudo /bin/pkg-boot refused\n' if refused else b'sudo /bin/pkg-boot\n')
            wait(lambda s:'PKGBOOT RING3 PASS' in s[start:] and PROMPT.search(s[start:]))
            uart.sendall(b'sudo shutdown\n');p.wait(timeout=25);assert not p.returncode
            assert H(disk)==(H(expected) if expected else initial)
            counter=re.search(r'PKGBOOT COUNTERS writes=(\d+) flushes=(\d+)',text());assert counter
            writes,flushes=map(int,counter.groups());assert (writes,flushes)==((16,2) if expected else (0,0))
            if not refused:
                root=dest/'root-after.img';root.write_bytes(disk.read_bytes()[2048*512:(2048+65536)*512])
                r=subprocess.run(['e2fsck','-fn',str(root)],capture_output=True,text=True);(dest/'linux-fsck.log').write_text(r.stdout+r.stderr);assert not r.returncode,r.stdout+r.stderr
            result={'firmware':firmware,'smp':smp,'refused':refused,'boot_runtime':'PASS','sequence':sequence,'whole_disk_unchanged':H(disk)==initial,'disk_sha256':H(disk),'writes':writes,'flushes':flushes,'replay_oracle_match':bool(expected)}
            (dest/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(label,'PASS',flush=True);return result
        finally:
            stop.set();uart.close()
            if drainer:drainer.join(timeout=1)
            if p.poll() is None:
                p.terminate()
                try:p.wait(timeout=5)
                except subprocess.TimeoutExpired:p.kill();p.wait(timeout=5)

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--smoke',action='store_true');parser.add_argument('--build-only',action='store_true');args=parser.parse_args()
    out=ROOT/'.codex-remote-attachments'/('pkg-boot-admission-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'));out.mkdir();work=out/'workspace'
    sources={str(p.relative_to(ROOT)):H(p) for folder in ('src','user','tests','scripts') for p in (ROOT/folder).rglob('*') if p.is_file() and p.suffix in ('.c','.h','.inc','.py','.asm','.mk') and '__pycache__' not in p.parts}
    guards={str(p.relative_to(ROOT)):H(p) for p in [ROOT/'Makefile',ROOT/'PROTECTED.md',ROOT/'AGENTS.md',ROOT/'docs/plans/PKG2_PRODUCTION_INTEGRATION.md',ROOT/'assets/ssl/ca-certificates.sha256',*(ROOT/'bin').glob('*')] if p.is_file()}
    m={'boot_runtime':'NOT RUN','preservation':'INCOMPLETE','sources':sources,'guards':guards,'commands':[],'overlays':[],'cases':[],
       'scope':'private copied kernel, measured separate TEST modules, signed projected-root validation and replay; detached root only, no production publication or enablement'}
    def save():(out/'manifest.json').write_text(json.dumps(m,indent=2)+'\n')
    def replace(name,before,after):
        p=work/name;s=p.read_text();assert s.count(before)==1,(name,before,s.count(before));p.write_text(s.replace(before,after));m['overlays'].append({'file':name,'before':before,'after':after})
    def run(cmd,label):
        m['commands'].append(cmd);save();p=subprocess.run(cmd,cwd=work,capture_output=True,text=True,timeout=600)
        (out/(label+'.log')).write_text(p.stdout+p.stderr)
        assert not p.returncode,(label,p.stdout[-1200:],p.stderr[-4000:])
    try:
        shutil.copytree(ROOT,work,ignore=shutil.ignore_patterns('.git','.codex-remote-attachments','build','bin','dist','__pycache__','*.pyc','*.iso','*.img','.VSCodeCounter'))
        m['snapshot_differences']=[p for p,h in sources.items() if H(work/p)!=h]
        assert set(m['snapshot_differences'])<=CONCURRENT_WLAN,m['snapshot_differences']
        m['copied_source_snapshot']={p:H(work/p) for p in sources}
        m['extraction']=overlay(work,replace);save()
        run(['make','-j8','bin/fortress.elf'],'kernel-build')
        run(['nm','--defined-only','bin/fortress.elf'],'kernel-symbols')
        symbols={line.split()[-1] for line in (out/'kernel-symbols.log').read_text().splitlines() if line.split()}
        assert not symbols.intersection(('mono_crypto_argon2','mono_crypto_ed25519_sign','crypto_ed25519_sign','cr_entropy'))
        frames=[]
        for p in [work/'build/fs/ext4.su',*list((work/'build/kernel').glob('*pkgboot*.su'))]:
            for line in p.read_text().splitlines():
                fields=line.split('\t')
                if len(fields)<2:continue
                name=fields[0].split(':')[-1]
                if name not in symbols or (p.name=='ext4.su' and not name.startswith(('jb_','pn_','e4_semantic_commit','current_read_file','identity','identities','export_bundle'))):continue
                limit=1280 if name=='mono_crypto_eddsa_check_equation' else 1024
                assert int(fields[1])<=limit,(name,fields[1],limit);frames.append({'function':name,'bytes':int(fields[1]),'limit':limit})
        m['linked_private_frames']=frames
        flags=['gcc','-std=c11','-Os','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-fno-pie','-fno-stack-protector','-mno-red-zone','-mno-sse','-mno-mmx','-msoft-float','-Wframe-larger-than=1024','-I.','-Isrc/include','-Itests']
        run(flags+['-c','tests/pkg_boot_ring3.c','-o','build/pkgboot.o'],'probe-compile')
        run(['nasm','-f','elf64','-DTOOL_ENTRY=pkg_call_main','user/tools/start.asm','-o','build/pkgboot-start.o'],'probe-start')
        run(['ld','-nostdlib','-static','-z','noexecstack','-T','user/shell.ld','build/pkgboot-start.o','build/pkgboot.o','-o','build/pkgboot.elf'],'probe-link')
        bootstrap=out/'bootstrap.tar'
        with tarfile.open(ROOT/'bin/initramfs.tar') as src,tarfile.open(bootstrap,'w',format=tarfile.USTAR_FORMAT) as dst:
            for member in src.getmembers():dst.addfile(member,src.extractfile(member) if member.isfile() else None)
            dst.add(work/'build/pkgboot.elf',arcname='bin/pkg-boot')
        assert bootstrap.stat().st_size<=8*1024*1024
        with tempfile.TemporaryDirectory(prefix='fortress-pkgboot-keys-') as temp:
            temp=pathlib.Path(temp);roles={}
            for name,values in (('selector',(111,112,113)),('enrollment',(41,42,43)),('catalog',(1,2,3))):
                roles[name]=[]
                for value in values:
                    p=temp/f'{name}-{value}';p.write_bytes(bytes([value])*32);p.chmod(0o600);roles[name].append(p)
            fixture=temp/'fixture';fixture.mkdir();image,_=create(fixture,identities=True)
            prepared=build(image,temp/'records',roles,bytes.fromhex(H(work/'bin/fortress.elf')),bytes.fromhex(H(bootstrap)))
            write_fresh_records(image,prepared);source=out/'enrolled.img';source.write_bytes(gpt(image.read_bytes()))
            (out/'oracle-root.img').write_bytes(image.read_bytes())
            (out/'selector.bin').write_bytes(prepared['selector']);(out/'policy.bin').write_bytes(prepared['policy'])
            (out/'identities.json').write_text(json.dumps(prepared['identities'],indent=2)+'\n')
            run(['e2fsck','-fn',str(image)],'linux-initial')
        m['measurements']={'kernel':H(work/'bin/fortress.elf'),'bootstrap':H(bootstrap),'selector':H(out/'selector.bin'),'policy':H(out/'policy.bin')}
        def iso(variant):
            directory=out/('iso-'+variant);(directory/'boot/limine').mkdir(parents=True);(directory/'EFI/BOOT').mkdir(parents=True)
            for name,path in (('fortress.elf',work/'bin/fortress.elf'),('bootstrap.tar',bootstrap),('selector.bin',out/'selector.bin'),('policy.bin',out/'policy.bin')):shutil.copyfile(path,directory/'boot'/name)
            if variant in ('kernel-mismatch','bootstrap-mismatch'):
                with (directory/'boot'/('fortress.elf' if variant=='kernel-mismatch' else 'bootstrap.tar')).open('ab') as f:f.write(b'private mismatch' if variant=='kernel-mismatch' else bytes(512))
            if variant=='signature':
                p=directory/'boot/selector.bin';b=bytearray(p.read_bytes());b[-1]^=1;p.write_bytes(b)
            for name in ('limine-bios.sys','limine-bios-cd.bin','limine-uefi-cd.bin'):shutil.copyfile(work/'limine'/name,directory/'boot/limine'/name)
            shutil.copyfile(work/'limine/BOOTX64.EFI',directory/'EFI/BOOT/BOOTX64.EFI')
            config='timeout: 0\n/Private package admission\n    protocol: limine\n    kernel_path: boot():/boot/fortress.elf\n    kernel_cmdline: boot_mode=pkg-test-v1\n    module_path: boot():/boot/bootstrap.tar\n    module_cmdline: role=bootstrap\n'
            if variant!='missing-selector':config+='    module_path: boot():/boot/selector.bin\n    module_cmdline: role=pkg-selector\n'
            config+='    module_path: boot():/boot/policy.bin\n    module_cmdline: '+('role=pkg-selector' if variant=='wrong-role' else 'role=pkg-policy')+'\n'
            (directory/'boot/limine/limine.conf').write_text(config)
            output=out/(variant+'.iso');run(['xorriso','-as','mkisofs','-b','boot/limine/limine-bios-cd.bin','-no-emul-boot','-boot-load-size','4','-boot-info-table','--efi-boot','boot/limine/limine-uefi-cd.bin','--efi-boot-part','--efi-boot-image','--protective-msdos-label',str(directory),'-o',str(output)],'iso-'+variant)
            run([str(work/'limine/limine'),'bios-install',str(output)],'bios-'+variant);return output
        valid=iso('valid');m['compiled_inputs']={str(p.relative_to(work)):H(p) for p in work.rglob('*') if p.is_file() and p.suffix in ('.c','.h','.inc','.asm')};save()
        if not args.build_only:
            grant_block=prepared['identities'][1]['extents'][0][2]
            recovery,recovered=pending(source,out/'journal-valid',grant_block)
            invalid_recovery,_=pending(source,out/'journal-invalid',grant_block,True)
            for fw,smp in ([('bios',1)] if args.smoke else [('bios',1),('bios',4),('uefi',1),('uefi',4)]):
                for repeat in range(3):m['cases'].append(boot(out,work,valid,source,fw,smp,f'{fw}-{smp}-repeat{repeat}'));save()
                previous=recovery
                for repeat in range(3):
                    label=f'{fw}-{smp}-journal{repeat}'
                    m['cases'].append(boot(out,work,valid,previous,fw,smp,label,expected=recovered if repeat==0 else None));save()
                    previous=out/label/'enrolled.img'
            if not args.smoke:
                for variant in ('kernel-mismatch','bootstrap-mismatch','signature','missing-selector','wrong-role'):
                    m['cases'].append(boot(out,work,iso(variant),source,'bios',1,variant,True));save()
                m['cases'].append(boot(out,work,valid,source,'bios',1,'read-error',True));save()
                m['cases'].append(boot(out,work,valid,invalid_recovery,'bios',1,'invalid-projected-journal',True));save()
                import pkg_enroll_fixture as fixture,uuid
                original=fixture.PART
                try:
                    fixture.PART=uuid.UUID('82345678-1234-4567-89ab-123456789abc')
                    foreign=out/'foreign-gpt.img';foreign.write_bytes(gpt((out/'oracle-root.img').read_bytes()))
                finally:fixture.PART=original
                m['cases'].append(boot(out,work,valid,foreign,'bios',1,'foreign-gpt',True));save()
                partial=out/'partial-root.img';shutil.copyfile(out/'oracle-root.img',partial)
                run(['debugfs','-w','-R','unlink /pkg/enrollment.v1',str(partial)],'partial-root')
                partial_disk=out/'partial-gpt.img';partial_disk.write_bytes(gpt(partial.read_bytes()))
                m['cases'].append(boot(out,work,valid,partial_disk,'bios',1,'partial-root',True));save()
                payload=out/'foreign-payload';payload.write_bytes(b'foreign')
                for name,path in (('foreign-package-object','/pkg/unowned'),('nonempty-transaction','/pkg/transaction.v1/stale')):
                    root=out/(name+'.root');shutil.copyfile(out/'oracle-root.img',root)
                    run(['debugfs','-w','-R',f'write {payload} {path}',str(root)],'prepare-'+name)
                    target=out/(name+'.img');target.write_bytes(gpt(root.read_bytes()))
                    m['cases'].append(boot(out,work,valid,target,'bios',1,name,True));save()
            if not args.smoke and extra_cases:
                for value in extra_cases(out,work,valid,source,prepared,run):m['cases'].append(value);save()
            m['boot_runtime']='PASS'
    except BaseException as e:m['error']=repr(e);m['boot_runtime']='FAIL';raise
    finally:
        m['changed_sources']=[p for p,h in sources.items() if H(ROOT/p)!=h]
        m['changed_guards']=[p for p,h in guards.items() if H(ROOT/p)!=h]
        m['changed_copies']=[p for p,h in m.get('compiled_inputs',{}).items() if H(work/p)!=h]
        unexpected=set(m['changed_sources'])-CONCURRENT_WLAN
        m['preservation']='FAIL' if unexpected or m['changed_guards'] or m['changed_copies'] else 'QUALIFIED concurrent WLAN edits' if m['changed_sources'] or m.get('snapshot_differences') else 'PASS'
        save();print(out,m['boot_runtime'],m['preservation'],flush=True)
    assert m['preservation']!='FAIL'

if __name__=='__main__':main()
