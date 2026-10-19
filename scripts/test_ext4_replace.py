#!/usr/bin/env python3
"""Isolated BIOS/UEFI Ring 3 replacement ABI; disposable NVMe files only."""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import hashlib,json,re,selectors,shutil,struct,subprocess,tarfile,tempfile,time
from test_getrandom import CODE,VARS,PROMPT
from test_ext4_read import gpt
from test_ext4_time_host import fixture
from create_ext4_fixtures import ROOT,run

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def boot(out,work,base,firmware,smp):
    case=out/f'{firmware}-{smp}';case.mkdir()
    with tempfile.TemporaryDirectory(prefix='replace-guest-') as tmp:
        folder=Path(tmp);disk=folder/'nvme.img';gpt(base,disk)
        cmd=['qemu-system-x86_64','-M','q35','-accel','tcg','-cpu','max','-m','512M','-smp',str(smp),
             '-display','none','-monitor','none','-no-reboot','-net','none','-boot','d',
             '-cdrom',str(work/'bin/fortress.iso'),'-serial','stdio',
             '-drive',f'file={disk},format=raw,if=none,id=data', '-device','nvme,drive=data,serial=REPLACEFIXTURE',
             '-fw_cfg','name=opt/fortress/ext4_journal_test,string=1']
        if firmware=='uefi':
            variables=folder/'vars.fd';shutil.copyfile(VARS,variables)
            cmd+=['-drive',f'if=pflash,format=raw,unit=0,readonly=on,file={CODE}',
                  '-drive',f'if=pflash,format=raw,unit=1,file={variables}']
        assert disk.is_file() and disk.resolve().parent==folder.resolve()
        assert len([c for c in cmd if c=='-drive'])==(3 if firmware=='uefi' else 1)
        (case/'argv.json').write_text(json.dumps(cmd,indent=2)+'\n')
        log=bytearray()
        with (case/'stderr.log').open('wb') as err:
            proc=subprocess.Popen(cmd,cwd=work,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=err)
            selector=selectors.DefaultSelector();selector.register(proc.stdout,selectors.EVENT_READ)
            def text():return re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]','',log.decode(errors='replace')).replace('\r','')
            def wait(predicate,timeout=150):
                deadline=time.monotonic()+timeout
                while time.monotonic()<deadline:
                    for key,_ in selector.select(.1):
                        chunk=key.fileobj.read1(65536)
                        if chunk:log.extend(chunk)
                    s=text();assert 'PANIC' not in s and 'REPLACE FAIL' not in s,s[-5000:]
                    if predicate(s):return s
                    assert proc.poll() is None,s[-5000:]
                raise AssertionError(text()[-5000:])
            def send(s):proc.stdin.write(s.encode()+b'\n');proc.stdin.flush()
            try:
                wait(lambda s:'FortressOS login:' in s or PROMPT.search(s))
                if not PROMPT.search(text()):send('operator')
                wait(lambda s:PROMPT.search(s))
                assert '[EXT4 JOURNAL] PASS boot 1' in text(),text()[-5000:]
                pos=len(text());send('sudo /bin/replace-probe')
                wait(lambda s:'REPLACE Ring3 PASS' in s[pos:] and PROMPT.search(s[pos:]))
                send('sudo shutdown');proc.wait(timeout=30);assert proc.returncode==0
            finally:
                if proc.poll() is None:
                    proc.terminate()
                    try:proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:proc.kill();proc.wait(timeout=5)
                log.extend(proc.stdout.read());selector.close();(case/'serial.log').write_bytes(log)
        data=disk.read_bytes();first,last=struct.unpack_from('<QQ',data,1056)
        image=folder/'partition.ext4';image.write_bytes(data[first*512:(last+1)*512])
        audit=run(['e2fsck','-fn',str(image)])
        payload=folder/'payload';audit+=run(['debugfs','-R',f'dump /replace-b/target {payload}',str(image)])
        assert payload.read_bytes()==b'NEW'
        missing=run(['debugfs','-R','stat /replace-a/source',str(image)]);assert 'File not found' in missing
        (case/'linux.log').write_text(audit+missing)
    result={'firmware':firmware,'smp':smp,'result':'PASS'}
    (case/'result.json').write_text(json.dumps(result)+'\n');print(result,flush=True);return result

def main():
    out=ROOT/'.codex-remote-attachments'/('ext4-replace-'+time.strftime('%Y%m%d-%H%M%S'));out.mkdir()
    work=out/'workspace'
    shutil.copytree(ROOT,work,ignore=shutil.ignore_patterns('.git','.codex-remote-attachments','build','bin','dist','__pycache__','*.pyc','*.iso','*.img'))
    # HTTPS is being edited concurrently. Freeze unrelated user tools/build
    # rules at the committed checkpoint inside this copy, retaining the actual
    # replacement kernel/ABI sources and the explicitly gated probe packaging.
    frozen=['Makefile','user/wget.c','user/wget_codec.c','user/wget_codec.h',
            'src/lib/tls/fortress_tls.c','src/lib/tls/tls.mk']
    checkpoint=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()
    for name in frozen:
        (work/name).write_bytes(subprocess.check_output(['git','show',checkpoint+':'+name],cwd=ROOT))
    makefile=work/'Makefile';rules=makefile.read_text()
    rules=rules.replace('include scripts/fstat.mk','include scripts/fstat.mk\ninclude scripts/replace.mk')
    line=next(line for line in rules.splitlines() if '"$(FSTAT_TEST_PROBE)"' in line)
    rules=rules.replace(line,line+'\n\t@if [ "$(REPLACE_TEST_PROBE)" = 1 ]; then cp -f $(BUILD_DIR)/replace-probe.elf $(BUILD_DIR)/initramfs/bin/replace-probe; else rm -f $(BUILD_DIR)/initramfs/bin/replace-probe; fi')
    makefile.write_text(rules)
    # No source/dependency symlinks may write through to the active checkout.
    assert (work/'limine/limine').is_file()
    paths=['src/fs/ext4_replace.inc','src/fs/ext4.c','src/fs/ext4_transaction.inc','src/fs/vfs.c',
           'src/fs/vfs.h','src/include/syscall_abi.h','src/kernel/syscall.c','tests/replace_probe.c']
    before={p:sha(ROOT/p) for p in paths};media={str(p.relative_to(ROOT)):sha(p) for p in (ROOT/'bin').glob('*') if p.is_file()}
    with (out/'build.log').open('w') as stream:
        subprocess.run(['make','-j4','iso','REPLACE_TEST_PROBE=1'],cwd=work,stdout=stream,stderr=subprocess.STDOUT,check=True,timeout=600)
    with tarfile.open(work/'bin/initramfs.tar') as archive:
        assert any(m.name.lstrip('./')=='bin/replace-probe' for m in archive.getmembers())
    with tempfile.TemporaryDirectory(prefix='replace-base-') as tmp:
        base=fixture(Path(tmp),4096);run(['debugfs','-w','-R','rm /target.bin',str(base)]);run(['e2fsck','-fn',str(base)])
        original=sha(base)
        with ThreadPoolExecutor(max_workers=2) as pool:
            results=list(pool.map(lambda x:boot(out,work,base,*x),[(f,s) for f in ('bios','uefi') for s in (1,4)]))
        assert sha(base)==original
    assert all(sha(ROOT/p)==h for p,h in before.items()),'replacement sources changed during gate'
    # Other chats may build release outputs; retain that qualification without
    # attributing their writes to this isolated builder.
    changed=[p for p,h in media.items() if not (ROOT/p).is_file() or sha(ROOT/p)!=h]
    (out/'manifest.json').write_text(json.dumps({'sources':before,'results':results,'test_iso_sha256':sha(work/'bin/fortress.iso'),
        'unrelated_frozen_checkpoint':checkpoint,'unrelated_frozen_paths':frozen,
        'release_outputs_changed_during_gate':changed,'scope':'isolated fixtures; no physical or sector-tear claim'},indent=2)+'\n')
    print('PASS Ring3 replacement 4/4; '+str(out),flush=True)
if __name__=='__main__':main()
