"""Disposable BIOS/UEFI Phase0 Ring3 metadata/ABI and gated spawn acceptance."""
from pathlib import Path
import hashlib,json,selectors,shutil,subprocess,tarfile,tempfile,time,re,sys,os
from test_ext4_read import gpt
from contextlib import nullcontext
ROOT=Path(__file__).resolve().parent.parent
phase2=os.environ.get('PERM_PHASE2_EXPECT')=='1'
output_root=ROOT/('build/permissions-phase2' if phase2 else 'build/permissions-phase0')
output_root.mkdir(parents=True,exist_ok=True)
out=Path(tempfile.mkdtemp(prefix='guest-',dir=output_root))
assert len(sys.argv) in (1,2,3)
usb=len(sys.argv)==3
if usb:assert sys.argv[2]=='--usb'
source=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else None
if source:assert source.is_file() and source.is_relative_to(ROOT/'build/permissions-phase0')
retained=out/'artifacts';retained.mkdir()
with nullcontext(str(retained)) as directory:
 tmp=Path(directory);root=tmp/'iso';shutil.copytree(ROOT/'build/iso_root',root)
 archive=tmp/'initramfs.tar'
 with tarfile.open(ROOT/'bin/initramfs.tar') as src,tarfile.open(archive,'w',format=tarfile.USTAR_FORMAT) as dst:
  for m in src.getmembers():dst.addfile(m,src.extractfile(m) if m.isfile() else None)
  data=(ROOT/('build/perm_phase2_user.elf' if phase2 else 'build/perm_user.elf')).read_bytes();m=tarfile.TarInfo('bin/perm-probe');m.size=len(data);m.mode=0o755
  import io
  dst.addfile(m,io.BytesIO(data))
 for p in (root/'boot/initramfs.tar',root/'initramfs.tar'):shutil.copyfile(archive,p)
 config='timeout: 0\n/FortressOS Phase0 Test\n    protocol: limine\n    kernel_path: boot():/boot/fortress.elf\n    module_path: boot():/boot/initramfs.tar\n'
 if usb:config+='    kernel_cmdline: usb_data=PARTUUID=11223344-5566-7788-99aa-bbccddeeff00 usb_data_mode=rw\n'
 if os.environ.get('PERM_LOGIN_BYPASS')=='1':
  assert phase2,'login bypass allowed only in explicit Phase2 regression'
  config=config.rstrip()+' login=0\n'
 for p in (root/'limine.conf',root/'boot/limine.conf',root/'boot/limine/limine.conf'):p.write_text(config)
 iso=tmp/'permissions.iso'
 subprocess.run(['xorriso','-as','mkisofs','-b','boot/limine/limine-bios-cd.bin','-no-emul-boot','-boot-load-size','4','-boot-info-table','--efi-boot','boot/limine/limine-uefi-cd.bin','-efi-boot-part','--efi-boot-image','--protective-msdos-label',str(root),'-o',str(iso)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 subprocess.run([str(ROOT/'limine/limine'),'bios-install',str(iso)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 for cpus in (1,4):
  for mode in ('bios','uefi'):
   label=f'{mode}-{cpus}';variables=tmp/'vars.fd';disk=tmp/f'{label}.img'
   if source:gpt(source,disk)
   if mode=='uefi':shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
   def command():
    cmd=['qemu-system-x86_64','-M','q35','-m','2G','-accel','tcg','-smp',str(cpus),'-display','none','-monitor','none','-no-reboot','-boot','d','-cdrom',str(iso),'-serial','stdio','-net','none',*([] if phase2 else ['-fw_cfg','name=opt/fortress/permissions_test,string=1'])]
    if source:
     cmd+=['-drive',f'file={disk},if=none,id=e4,format=raw']
     cmd+=['-device','qemu-xhci,id=xhci,p2=4,p3=0','-device','usb-storage,drive=e4,bus=xhci.0','-fw_cfg','name=opt/fortress/ext4_journal_usb_test,string=1'] if usb else ['-device','nvme,drive=e4,serial=PERMPHASE0','-fw_cfg','name=opt/fortress/ext4_journal_test,string=1']
    if mode=='uefi':cmd+=['-drive','if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd','-drive',f'if=pflash,format=raw,unit=1,file={variables}']
    return cmd
   def preflight(cmd):
    assert cmd==command();assert iso.is_file() and iso.resolve().parent==tmp.resolve()
    if source:assert disk.is_file() and disk.resolve().parent==tmp.resolve()
   cmd=command();preflight(cmd)
   for extra in (['-drive','file=/dev/sda'],['-blockdev','driver=host_device'],['-snapshot']):
    try:preflight(cmd+extra)
    except AssertionError:pass
    else:raise AssertionError('extra storage admitted')
   (out/f'{label}-argv.json').write_text(json.dumps(cmd,indent=2));transcript=bytearray()
   with (out/f'{label}.stderr').open('wb') as err:
    proc=subprocess.Popen(cmd,cwd=ROOT,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=err);selector=selectors.DefaultSelector();selector.register(proc.stdout,selectors.EVENT_READ)
    def wait(predicate,timeout=180):
     end=time.monotonic()+timeout
     while time.monotonic()<end:
      for key,_ in selector.select(.1):
       b=key.fileobj.read1(65536)
       if b:transcript.extend(b)
      text=re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]','',transcript.decode(errors='replace')).replace('\r','')
      assert not any(t in text for t in ('[FATAL]','PERM PHASE0 FAIL','PERM PHASE2 FAIL','[FAIL]','CPU EXCEPTION KERNEL PANIC')),text[-4000:]
      if predicate(text):return text
      assert proc.poll() is None,text[-4000:]
     raise TimeoutError(transcript[-4000:])
    try:
     wait(lambda t:re.search(r'fortress:[^\n]* \$ ',t))
     proc.stdin.write(('/bin/perm-probe'+(' usb' if usb else ' mounted' if source else '')+'\n').encode());proc.stdin.flush()
     if phase2:
      text=wait(lambda t:'PERM PHASE2 NONROOT ENFORCEMENT PASS' in t and re.search(r'PASS\n[^\n]*fortress:[^\n]* \$ ',t))
      def execute(command,expected):
       prior=len(transcript);proc.stdin.write((command+'\n').encode());proc.stdin.flush()
       wait(lambda t:expected in re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]','',transcript[prior:].decode(errors='replace')).replace('\r','') and re.search(r'fortress:[^\n]* \$ $',t))
      execute('id','uid=0 gid=0 groups=0\n')
      execute('ls -l /mnt/public/user-file','-rwxrwx--- 1001 1001 0 /mnt/public/user-file\n')
      execute('chmod 0644 /mnt/public/user-file; chown 42:43 /mnt/public/user-file; ls -l /mnt/public/user-file','-rw-r--r-- 42 43 0 /mnt/public/user-file\n')
      execute('umask','0022\n')
      execute('umask 0027; echo data > /mnt/public/root-mask; ls -l /mnt/public/root-mask','-rw-r----- root root 5 /mnt/public/root-mask\n')
      execute('umask 0022; cat /mnt/public/root-mask','data\n')
      print(f'PASS Phase2 {label}: nonroot DAC/metadata/capabilities + root tools/shell',flush=True)
     else:
      text=wait(lambda t:'PERM PHASE0 STAT ABI/NAMESPACES/SPAWN PASS' in t and re.search(r'PASS\n[^\n]*fortress:[^\n]* \$ ',t))
      assert 'PERM PHASE0 CHILD PASS' in text and text.count('PERM PHASE0 dropped-cap user spawn PASS')>=2
      if usb:assert 'PERM PHASE0 USB RAW READ/FILESYSTEM PEER PASS' in text
      assert text.count('FortressOS shell (Ring 3)')==1
      assert 'PERM PHASE1 ACTOR PATHS/ACTIONS/SIGNAL PASS' in text
      if os.environ.get('PERM_TRACE_EXPECT')=='1':
       records=re.findall(r'PERM TRACE mask=(\d+) euid=(\d+) egid=(\d+) caps=(\S+)',text)
       assert records and {1,2,3,4,6} <= {int(m) for m,_,_,_ in records},records
       assert any(int(m)==1 and int(c,16)==0 for m,_,_,c in records),records
       (out/f'{label}-trace.json').write_text(json.dumps(records,indent=2))
      print(f'PASS Phase0 {label}: Ring3 stat ABI/TarFS/devfs/runfs'+('/EXT4' if source else '')+' and dropped-cap user spawn',flush=True)
    finally:
     proc.terminate()
     try:proc.wait(timeout=5)
     except subprocess.TimeoutExpired:proc.kill();proc.wait(timeout=5)
     selector.close();(out/f'{label}.log').write_bytes(transcript)
 (out/'manifest.json').write_text(json.dumps({'source':str(source),'usb':usb,'trace_required':os.environ.get('PERM_TRACE_EXPECT')=='1','cases':4,'iso_sha256':hashlib.sha256(iso.read_bytes()).hexdigest(),'kernel_sha256':hashlib.sha256((ROOT/'bin/fortress.elf').read_bytes()).hexdigest(),'probe_sha256':hashlib.sha256((ROOT/('build/perm_phase2_user.elf' if phase2 else 'build/perm_user.elf')).read_bytes()).hexdigest()},indent=2))
print(f'Evidence: {out}',flush=True)
