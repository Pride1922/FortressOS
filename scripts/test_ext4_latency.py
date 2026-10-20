"""EH-5 private ISO, BIOS/UEFI x SMP=1/4 x sole NVMe/USB fixture."""
from pathlib import Path
import argparse,hashlib,json,re,selectors,shutil,struct,subprocess,tarfile,tempfile,time
from test_getrandom import CODE,VARS,PROMPT
from test_ext4_read import gpt
from test_ext4_time_host import fixture
from create_ext4_fixtures import ROOT,run
STAGE=False

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def boot(out,work,base,firmware,smp,transport):
    case=out/f'{firmware}-{smp}-{transport}';case.mkdir()
    with tempfile.TemporaryDirectory(prefix='latency-guest-') as tmp:
        folder=Path(tmp);disk=folder/'data.img';gpt(base,disk)
        cmd=['qemu-system-x86_64','-M','q35','-accel','tcg','-cpu','max','-m','512M','-smp',str(smp),
             '-display','none','-monitor','none','-no-reboot','-net','none','-boot','d',
             '-cdrom',str(work/'bin/fortress.iso'),'-serial','stdio',
             '-drive',f'file={disk},format=raw,if=none,id=data,cache=writeback']
        if transport=='nvme':cmd+=['-device','nvme,drive=data,serial=LATENCYFIXTURE','-fw_cfg','name=opt/fortress/ext4_journal_test,string=1']
        else:cmd+=['-device','qemu-xhci,id=xhci,p2=4,p3=0','-device','usb-storage,drive=data,id=journalusb,bus=xhci.0','-fw_cfg','name=opt/fortress/ext4_journal_usb_test,string=1']
        if firmware=='uefi':
            variables=folder/'vars.fd';shutil.copyfile(VARS,variables)
            cmd+=['-drive',f'if=pflash,format=raw,unit=0,readonly=on,file={CODE}','-drive',f'if=pflash,format=raw,unit=1,file={variables}']
        assert disk.is_file() and disk.resolve().parent==folder.resolve()
        assert len([c for c in cmd if c=='-drive'])==(3 if firmware=='uefi' else 1)
        assert not any('/dev/' in c or 'host_device' in c for c in cmd)
        (case/'argv.json').write_text(json.dumps(cmd,indent=2)+'\n');log=bytearray()
        with (case/'stderr.log').open('wb') as err:
            proc=subprocess.Popen(cmd,cwd=work,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=err)
            selector=selectors.DefaultSelector();selector.register(proc.stdout,selectors.EVENT_READ)
            def text():return re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]','',log.decode(errors='replace')).replace('\r','')
            def wait(predicate,timeout=300):
                deadline=time.monotonic()+timeout
                while time.monotonic()<deadline:
                    for key,_ in selector.select(.1):
                        chunk=key.fileobj.read1(65536)
                        if chunk:
                            log.extend(chunk);(case/'serial-live.log').write_bytes(log)
                    s=text();assert 'PANIC' not in s and 'LATENCY FAIL' not in s and 'STAGE FAIL' not in s and '[FAIL] ext2/audit:' not in s,s[-5000:]
                    if predicate(s):return s
                    assert proc.poll() is None,s[-5000:]
                raise AssertionError(text()[-5000:])
            def send(s):proc.stdin.write(s.encode()+b'\n');proc.stdin.flush()
            try:
                wait(lambda s:'FortressOS login:' in s or PROMPT.search(s))
                if not PROMPT.search(text()):send('operator')
                wait(lambda s:PROMPT.search(s));assert '[EXT4 JOURNAL] PASS boot 1' in text(),text()[-5000:]
                pos=len(text());start=time.monotonic();send('sudo /bin/'+('stage-probe' if STAGE else 'latency-probe'))
                wait(lambda s:('STAGE Ring3 PASS' in s[pos:] if STAGE else 'LATENCY Ring3 PASS' in s[pos:] and 'LATENCY peer PASS' in s[pos:]) and PROMPT.search(s[pos:]))
                seconds=time.monotonic()-start
                metrics={k:int(v) for k,v in re.findall(('STAGE' if STAGE else 'LATENCY')+r' (\w+)=(\d+)',text()[pos:])}
                if STAGE:assert metrics['base_calls']==640 and metrics['batch_calls']==80 and metrics['large_calls']==800
                else:assert metrics['calls']==640 and metrics['bytes']==10485760
                # Report actual overlap instead of inferring it from two processes.
                assert (metrics['base_peer_overlap']>0 and metrics['batch_peer_overlap']>0) if STAGE else metrics['peer_overlap_samples']>0,metrics
                send('sudo shutdown');proc.wait(timeout=30);assert proc.returncode==0
            finally:
                if proc.poll() is None:
                    proc.terminate()
                    try:proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:proc.kill();proc.wait(timeout=5)
                log.extend(proc.stdout.read());selector.close();(case/'serial.log').write_bytes(log)
        data=disk.read_bytes();first,last=struct.unpack_from('<QQ',data,1056)
        image=folder/'partition.ext4';image.write_bytes(data[first*512:(last+1)*512]);audit=run(['e2fsck','-fn',str(image)])
        payloads=(('/stage-base',b'B'*10485760),('/stage-batch',b'B'*10485760),('/stage-100',b'B'*104857600),('/stage-log',b'LOG\n'*128),('/stage-check',b'B'*131072)) if STAGE else (('/latency-stage',b'L'*10485760),('/latency-log',b'LOG\n'*64))
        for path,expected in payloads:
            payload=folder/'payload';audit+=run(['debugfs','-R',f'dump {path} {payload}',str(image)])
            assert payload.read_bytes()==expected;payload.unlink()
        (case/'linux.log').write_text(audit)
    result={'firmware':firmware,'smp':smp,'transport':transport,'result':'PASS','wall_seconds':seconds,'metrics':metrics}
    (case/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result),flush=True);return result

def main():
    global STAGE
    parser=argparse.ArgumentParser();parser.add_argument('--stage',action='store_true');args=parser.parse_args();STAGE=args.stage
    out=ROOT/'.codex-remote-attachments'/(('ext4-stage-' if STAGE else 'ext4-latency-')+time.strftime('%Y%m%d-%H%M%S'));out.mkdir();work=out/'workspace'
    shutil.copytree(ROOT,work,ignore=shutil.ignore_patterns('.git','.codex-remote-attachments','build','bin','dist','__pycache__','*.pyc','*.iso','*.img'))
    checkpoint=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()
    frozen=['Makefile','user/wget.c','user/wget_codec.c','user/wget_codec.h','src/lib/tls/fortress_tls.c','src/lib/tls/tls.mk']
    frozen+=subprocess.check_output(['git','ls-tree','-r','--name-only',checkpoint,'--','user/watchtower'],cwd=ROOT,text=True).splitlines()
    for name in frozen:(work/name).write_bytes(subprocess.check_output(['git','show',checkpoint+':'+name],cwd=ROOT))
    # Private rules only; ordinary builds have neither a dependency nor packaging.
    rules='''
$(INITRAMFS_TAR): $(BUILD_DIR)/latency-probe.elf
$(BUILD_DIR)/latency-probe.elf: tests/latency_probe.c tests/replace_probe.ld $(BUILD_DIR)/tool-common.o
\t$(CC) $(CFLAGS) -Os -fno-pie -Iuser/tools -Wframe-larger-than=1024 -fstack-usage -c $< -o $(BUILD_DIR)/latency-probe.o
\t$(AS) -f elf64 -DTOOL_ENTRY=latency_probe_main user/tools/start.asm -o $(BUILD_DIR)/latency-probe-start.o
\t$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack -T tests/replace_probe.ld $(BUILD_DIR)/latency-probe-start.o $(BUILD_DIR)/latency-probe.o $(BUILD_DIR)/tool-common.o -o $@
'''
    if STAGE:
        rules=rules.replace('latency-probe','stage-probe').replace('tests/latency_probe.c','tests/stage_probe.c').replace('latency_probe_main','stage_probe_main')
    makefile=work/'Makefile';s=makefile.read_text();line=next(x for x in s.splitlines() if '"$(FSTAT_TEST_PROBE)"' in x)
    probe='stage-probe' if STAGE else 'latency-probe'
    s=s.replace(line,line+f'\n\tcp $(BUILD_DIR)/{probe}.elf $(BUILD_DIR)/initramfs/bin/{probe}');makefile.write_text(s+rules)
    config=work/'limine.conf';s,count=re.subn(r'(kernel_cmdline:[^\r\n]*)',r'\1 usb_data=PARTUUID=11223344-5566-7788-99aa-bbccddeeff00 usb_data_mode=rw',config.read_text(),count=1);assert count==1;config.write_text(s)
    paths=[p for p in (ROOT/'src/fs').iterdir() if p.is_file()]+[ROOT/'src/kernel/main.c',ROOT/'src/kernel/syscall.c',ROOT/'src/include/syscall_abi.h',ROOT/'tests/latency_probe.c',ROOT/'scripts/test_ext4_latency.py']
    if STAGE:paths += [ROOT/'tests/stage_probe.c',ROOT/'src/kernel/ext4_stage_test.inc',ROOT/'scripts/test_ext4_time_host.py']
    before={str(p.relative_to(ROOT)):sha(p) for p in paths};media={str(p.relative_to(ROOT)):sha(p) for p in (ROOT/'bin').glob('*') if p.is_file()}
    assert all(sha(work/p)==h for p,h in before.items()),'snapshot changed before build'
    with (out/'build.log').open('w') as stream:subprocess.run(['make','-j4','iso']+(['STAGE_TEST_PROBE=1'] if STAGE else []),cwd=work,stdout=stream,stderr=subprocess.STDOUT,check=True,timeout=600)
    with tarfile.open(work/'bin/initramfs.tar') as archive:assert any(m.name.lstrip('./')=='bin/'+probe for m in archive.getmembers())
    results=[]
    with tempfile.TemporaryDirectory(prefix='latency-base-') as tmp:
        base=fixture(Path(tmp),4096,size_mib=160 if STAGE else 32);run(['debugfs','-w','-R','rm /target.bin',str(base)]);run(['e2fsck','-fn',str(base)]);original=sha(base)
        for transport in ('nvme','usb'):
            for firmware in ('bios','uefi'):
                for smp in (1,4):results.append(boot(out,work,base,firmware,smp,transport))
        assert sha(base)==original
    if STAGE:
        objects=['build/fs/ext4.o','build/kernel/syscall.o'];symbols={p:run(['nm','-a',str(work/p)]) for p in objects}
        assert ('ext4_stage_write' in symbols[objects[0]] or 'ext4_stage_test_write' in symbols[objects[0]]) and ('stage_test_owners' in symbols[objects[1]] or 'stage_owners' in symbols[objects[1]])
        (out/'production-inclusion.json').write_text(json.dumps({'result':'PASS','objects':objects,'scope':'SYS_STAGE_WRITE (85) and SYS_FSYNC (86) production ABI inclusion'},indent=2)+'\n')
    changed=[p for p,h in media.items() if not (ROOT/p).is_file() or sha(ROOT/p)!=h]
    source_changes=[p for p,h in before.items() if sha(ROOT/p)!=h]
    (out/'manifest.json').write_text(json.dumps({'sources':before,'source_changes_during_gate':source_changes,'results':results,'test_iso_sha256':sha(work/'bin/fortress.iso'),'unrelated_frozen_checkpoint':checkpoint,'unrelated_frozen_paths':frozen,'release_outputs_changed_during_gate':changed,'scope':'SYS_STAGE_WRITE (85) and SYS_FSYNC (86) 32-block staging verification: paired 10MiB and batch 100MiB staging with peers' if STAGE else 'guest TSC and wall timings; one 10MiB run per configuration with actual peer overlap; no physical, SSH, slow controller, kernel phase timing or installed-root claim'},indent=2)+'\n')
    print('Evidence: '+str(out),flush=True)
if __name__=='__main__':main()
