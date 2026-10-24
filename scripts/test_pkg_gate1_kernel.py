"""Private Ring 3 status/recover, enrolled RAM root, real signal/reaper paths.

Only regular-file artifacts in a private copied workspace are built/written.
No block device is attached to QEMU; the enrolled data image is copied from an
immutable boot module into retained RAM. This is not physical durability proof.
"""
import argparse,datetime,hashlib,json,pathlib,re,shutil,socket,subprocess,tarfile,tempfile,threading,time
from pkg_gate1_kernel_overlay import overlay
from test_getrandom import CODE,VARS,PROMPT
ROOT=pathlib.Path(__file__).resolve().parents[1]
H=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()

def boot(out,work,iso,fw,smp,expected,source=None,negative=None,repeat=0):
 dest=out/(f'{fw}-{smp}'+('-'+negative if negative else '')+(f'-repeat{repeat}' if repeat else ''));dest.mkdir()
 disk=None;initial=None
 if source:
  disk=dest/'enrolled.img';shutil.copyfile(source,disk)
  assert disk.is_file() and not disk.is_symlink() and disk.resolve().parent==dest.resolve() and disk.stat().st_size==33554432
  if negative=='signature':
   prior=source.parent;loc=(prior/'4096-512-completion-locator.img').read_bytes();import struct
   physical=struct.unpack_from('<Q',loc,48)[0];data=bytearray(disk.read_bytes());data[physical*4096+1568]^=1;disk.write_bytes(data)
  if negative=='partial':
   r=subprocess.run(['debugfs','-w','-R','unlink /pkg/enrollment.v1',str(disk)],capture_output=True,text=True);assert not r.returncode
  if negative=='foreign':
   # Mutate only the immutable enrollment marker; checksum/signature mismatch
   # is intentional and the guest must refuse before any replay/activation.
   r=subprocess.run(['debugfs','-R','stat /pkg/enrollment.v1',str(disk)],capture_output=True,text=True);assert not r.returncode
   physical=int(re.search(r'\(0\):([0-9]+)',r.stdout)[1]);data=bytearray(disk.read_bytes());data[physical*4096+40]^=1;disk.write_bytes(data)
  initial=H(disk)
 with tempfile.TemporaryDirectory(prefix='fortress-gate1-') as tmp:
  uartpath=pathlib.Path(tmp)/'uart';qmppath=pathlib.Path(tmp)/'qmp'
  argv=['qemu-system-x86_64','-M','q35','-accel','tcg','-cpu','max','-m','512M','-smp',str(smp),'-display','none','-net','none','-no-reboot','-boot','d','-cdrom',str(iso),'-chardev',f'socket,id=uart,path={uartpath},server=on,wait=off,logfile={dest/"serial.log"}','-serial','chardev:uart','-qmp',f'unix:{qmppath},server=on,wait=off']
  if fw=='uefi':
   shutil.copyfile(VARS,dest/'vars.fd');argv+=['-drive',f'if=pflash,format=raw,unit=0,readonly=on,file={CODE}','-drive',f'if=pflash,format=raw,unit=1,file={dest/"vars.fd"}']
  if disk:argv+=['-drive',f'if=none,format=raw,id=gate1,file={disk}','-device','nvme,drive=gate1,serial=FORTRESS-GATE1','-fw_cfg','name=opt/fortress/pkg_gate1_nvme,string=private-regular-file-only']
  if negative=='read-error':argv+=['-fw_cfg','name=opt/fortress/pkg_gate1_read_error,string=private-injected-read-failure']
  for forbidden in ('-hda','-hdb','-blockdev','-readconfig'):assert forbidden not in argv
  for i,a in enumerate(argv):
   if a=='-drive':assert argv[i+1].startswith('if=pflash,format=raw,unit=') or (disk and argv[i+1]==f'if=none,format=raw,id=gate1,file={disk}')
  assert sum(a.startswith('if=none,') for a in argv)==(1 if disk else 0)
  (dest/'argv.json').write_text(json.dumps(argv,indent=2)+'\n');stop=threading.Event();uart=socket.socket(socket.AF_UNIX);drainer=None
  with (dest/'stderr.log').open('w') as err:
   p=subprocess.Popen(argv,cwd=work,stderr=err)
   def text():
    f=dest/'serial.log';return re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]','',f.read_text(errors='replace') if f.exists() else '').replace('\r','')
   def wait(predicate,seconds=240):
    until=time.monotonic()+seconds
    while time.monotonic()<until:
     s=text();assert not any(x in s for x in ('[FATAL]','[PANIC]','GATE1 ASSERT','GATE1 RING3 FAIL')),s[-5000:]
     if predicate(s):return s
     assert p.poll() is None,(p.returncode,s[-3000:]);time.sleep(.1)
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
    assert ('GATE1 ROOT REFUSED writes=0 flushes=0' if negative else 'GATE1 ENROLLED DETACHED ROOT ADMITTED') in text()
    if not PROMPT.search(text()):uart.sendall(b'operator\n')
    wait(lambda s:PROMPT.search(s));start=len(text());uart.sendall(b'sudo /bin/pkg-gate1 refused\n' if negative else b'sudo /bin/pkg-gate1 again\n' if repeat else b'sudo /bin/pkg-gate1\n')
    marker='GATE1 REFUSAL PASS writes=0' if negative else 'GATE1 REPEAT PASS commits=0 IDLE=12' if repeat else 'GATE1 RING3 PASS children=7 reaped=7 signals=3 commits=11 IDLE=12'
    result=wait(lambda s:marker in s[start:] and PROMPT.search(s[start:]))[start:]
    if negative:
     uart.sendall(b'sudo shutdown\n');p.wait(timeout=25);assert not p.returncode and H(disk)==initial
     value=dict(firmware=fw,smp=smp,negative=negative,boot_runtime='PASS',writes=0,flushes=0,input_sha256=initial,output_sha256=H(disk));(dest/'result.json').write_text(json.dumps(value,indent=2)+'\n');print(value,flush=True);return value
    snap=re.search(r'GATE1 SNAPSHOT address=(0x[0-9A-Fa-f]+) bytes=(\d+) commits=(\d+) writes=(\d+) flushes=(\d+)',result);assert snap,result
    mask=None if repeat else int(re.search(r'(?m)^GATE1 CPU_MASK=(\d+)',result)[1])
    if not repeat:assert mask and (smp==1 or mask&~1)
    start=len(text());uart.sendall(b'echo GATE1_RC=$?\n');wait(lambda s:re.search(r'(?m)^GATE1_RC=0\s*$',s[start:]) and PROMPT.search(s[start:]))
    image=dest/'kernel-final.img'
    if disk:shutil.copyfile(disk,image)
    else:
     with socket.socket(socket.AF_UNIX) as q:
      q.connect(str(qmppath));q.settimeout(30);f=q.makefile('rwb',buffering=0);json.loads(f.readline())
      def command(value):
       f.write((json.dumps(value)+'\n').encode())
       while True:
        reply=json.loads(f.readline());assert 'error' not in reply,reply
        if 'return' in reply:return reply['return']
      command({'execute':'qmp_capabilities'});response=command({'execute':'human-monitor-command','arguments':{'command-line':f'memsave {snap[1]} {snap[2]} "{image}"'}});assert not response,response
    assert image.stat().st_size==33554432
    r=subprocess.run(['e2fsck','-fn',str(image)],capture_output=True,text=True);(dest/'fsck.log').write_text(r.stdout+r.stderr);assert not r.returncode
    files=['/pkg/grant.v1','/pkg/ownership.v1','/pkg/retirement.v1','/pkg/account-generation.v1','/pkg/origin.v1','/usr/bin/demo']
    hashes={}
    for i,name in enumerate(files):
     actual=dest/f'actual-{i}.bin';oracle=dest/f'oracle-{i}.bin'
     for disk,target in ((image,actual),(expected,oracle)):
      r=subprocess.run(['debugfs','-R',f'dump {name} {target}',str(disk)],capture_output=True,text=True);assert not r.returncode and target.is_file(),r.stderr
     assert actual.read_bytes()==oracle.read_bytes(),name;hashes[name]=H(actual)
    uart.sendall(b'sudo shutdown\n');p.wait(timeout=25);assert not p.returncode
    value=dict(firmware=fw,smp=smp,repeat=repeat,boot_runtime='PASS',linux_oracle='PASS',cpu_mask=mask,commits=int(snap[3]),writes=int(snap[4]),flushes=int(snap[5]),image_sha256=H(image),records=hashes)
    (dest/'result.json').write_text(json.dumps(value,indent=2)+'\n');print(value,flush=True);return value
   except BaseException as e:(dest/'failure.txt').write_text(repr(e));raise
   finally:
    stop.set();uart.close()
    if drainer:drainer.join(timeout=1)
    if p.poll() is None:
     p.terminate()
     try:p.wait(timeout=5)
     except subprocess.TimeoutExpired:p.kill();p.wait(timeout=5)

def main():
 parser=argparse.ArgumentParser();parser.add_argument('--smoke',action='store_true');parser.add_argument('--build-only',action='store_true');parser.add_argument('--nvme',action='store_true');parser.add_argument('--negatives',action='store_true');args=parser.parse_args()
 assert not args.negatives or args.nvme
 out=ROOT/'.codex-remote-attachments'/('pkg-gate1-kernel-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'));work=out/'workspace';out.mkdir(parents=True)
 sources={str(p.relative_to(ROOT)):H(p) for folder in ('src','user','tests','scripts') for p in (ROOT/folder).rglob('*') if p.is_file() and p.suffix in ('.c','.h','.inc','.py','.asm','.mk') and '__pycache__' not in p.parts}
 guards={str(p.relative_to(ROOT)):H(p) for p in [ROOT/'Makefile',ROOT/'AGENTS.md',ROOT/'PROTECTED.md',ROOT/'docs/plans/PKG2_PRODUCTION_INTEGRATION.md',ROOT/'assets/ssl/ca-certificates.sha256',*(p for p in (ROOT/'bin').iterdir() if p.is_file())]}
 m=dict(boot_runtime='NOT RUN',preservation='INCOMPLETE',sources=sources,guards=guards,commands=[],overlays=[],cases=[],storage='regular-file NVMe' if args.nvme else 'RAM',scope='private copied kernel, boot-selected enrolled detached root, actual extracted validators and Ring 3 signals/reaper; storage backend recorded separately; no production root publication, hardware durability or release acceptance')
 def save():(out/'manifest.json').write_text(json.dumps(m,indent=2)+'\n')
 save()
 def replace(name,before,after):
  p=work/name;s=p.read_text();assert s.count(before)==1,(name,before,s.count(before));p.write_text(s.replace(before,after));m['overlays'].append(dict(file=name,before=before,after=after))
 def run(cmd,label,timeout=600):
  m['commands'].append(cmd);save();r=subprocess.run(cmd,cwd=work,capture_output=True,text=True,timeout=timeout);(out/(label+'.log')).write_text(r.stdout+r.stderr);assert not r.returncode,(label,r.stdout[-1000:],r.stderr[-4000:])
 try:
  shutil.copytree(ROOT,work,ignore=shutil.ignore_patterns('.git','.codex-remote-attachments','build','bin','dist','__pycache__','*.pyc','*.iso','*.img','.VSCodeCounter'))
  assert all(H(work/p)==h for p,h in sources.items());m['extraction']=overlay(work,replace);save()
  run(['make','-j8','bin/fortress.elf'],'kernel-build')
  run(['nm','--defined-only','bin/fortress.elf'],'kernel-symbols')
  symbols={line.split()[-1] for line in (out/'kernel-symbols.log').read_text().splitlines() if line.split()}
  assert not symbols.intersection(('mono_crypto_argon2','mono_crypto_ed25519_sign','crypto_ed25519_sign','cr_entropy'))
  frames=[]
  for p in [work/'build/fs/ext4.su',*list((work/'build/kernel').glob('*gate1*.su'))]:
   for line in p.read_text().splitlines():
    fields=line.split('\t')
    if len(fields)<2:continue
    name=fields[0].split(':')[-1]
    if name not in symbols:continue
    if p.name=='ext4.su' and not name.startswith(('xi_','xr_','ad_','sv_','ng_','gk_')):continue
    limit=1280 if name=='mono_crypto_eddsa_check_equation' else 1024
    assert int(fields[1])<=limit,(name,fields[1],limit)
    frames.append(dict(function=name,bytes=int(fields[1]),limit=limit,kind=fields[2],source=str(p.relative_to(work))))
  assert frames;m['linked_private_frames']=frames;save()
  flags=['gcc','-std=c11','-Os','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-fno-pie','-fno-stack-protector','-mno-red-zone','-mno-sse','-mno-mmx','-msoft-float','-Wframe-larger-than=1024','-I.','-Isrc/include','-Itests']
  run(flags+['-c','tests/pkg_gate1_ring3.c','-o','build/gate1.o'],'ring3-compile')
  run(['nasm','-f','elf64','-DTOOL_ENTRY=pkg_call_main','user/tools/start.asm','-o','build/gate1-start.o'],'ring3-start')
  run(['ld','-nostdlib','-static','-z','noexecstack','-T','user/shell.ld','build/gate1-start.o','build/gate1.o','-o','build/gate1.elf'],'ring3-link')
  prior=ROOT/'.codex-remote-attachments/pkg-kernel-backend-host-20261024-073146-531274';assert json.loads((prior/'manifest.json').read_text())['status']=='PASS'
  fixtures={'gate1/root.img':prior/'4096-512-activation-final.img','gate1/policy.bin':prior/'4096-512-policy.bin','gate1/enrollment.bin':prior/'4096-512-enrollment.bin'}
  expected=prior/'4096-512-recover-final.img';m['fixture_inputs']={str(p.relative_to(ROOT)):H(p) for p in [*fixtures.values(),expected]}
  with tarfile.open(ROOT/'bin/initramfs.tar') as src,tarfile.open(out/'initramfs.tar','w',format=tarfile.USTAR_FORMAT) as dst:
   for member in src.getmembers():dst.addfile(member,src.extractfile(member) if member.isfile() else None)
   dst.add(work/'build/gate1.elf',arcname='bin/pkg-gate1')
   directory=tarfile.TarInfo('gate1');directory.type=tarfile.DIRTYPE;directory.mode=0o700;dst.addfile(directory)
   for name,path in fixtures.items():dst.add(path,arcname=name)
  iso_root=out/'iso-root';(iso_root/'boot/limine').mkdir(parents=True);(iso_root/'EFI/BOOT').mkdir(parents=True)
  shutil.copyfile(work/'bin/fortress.elf',iso_root/'boot/fortress.elf');shutil.copyfile(out/'initramfs.tar',iso_root/'boot/initramfs.tar')
  for name in ('limine-bios.sys','limine-bios-cd.bin','limine-uefi-cd.bin'):shutil.copyfile(work/'limine'/name,iso_root/'boot/limine'/name)
  shutil.copyfile(work/'limine/BOOTX64.EFI',iso_root/'EFI/BOOT/BOOTX64.EFI')
  (iso_root/'boot/limine/limine.conf').write_text('timeout: 0\n/Private Gate 1\n    protocol: limine\n    kernel_path: boot():/boot/fortress.elf\n    kernel_cmdline: boot_mode=live\n    module_path: boot():/boot/initramfs.tar\n')
  iso=out/'private-gate1.iso';run(['xorriso','-as','mkisofs','-b','boot/limine/limine-bios-cd.bin','-no-emul-boot','-boot-load-size','4','-boot-info-table','--efi-boot','boot/limine/limine-uefi-cd.bin','--efi-boot-part','--efi-boot-image','--protective-msdos-label',str(iso_root),'-o',str(iso)],'private-iso')
  run([str(work/'limine/limine'),'bios-install',str(iso)],'private-bios')
  m['compiled_inputs']={str(p.relative_to(work)):H(p) for p in work.rglob('*') if p.is_file() and p.suffix in ('.c','.h','.inc','.asm')};m['private_iso_sha256']=H(iso)
  if not args.build_only:
   source=fixtures['gate1/root.img'] if args.nvme else None
   for fw,smp in ([('bios',1)] if args.smoke else [('bios',1),('bios',4),('uefi',1),('uefi',4)]):
    m['cases'].append(boot(out,work,iso,fw,smp,expected,source));save()
    if args.nvme:
     previous=out/f'{fw}-{smp}'/'enrolled.img'
     for repeat in (1,2):
      m['cases'].append(boot(out,work,iso,fw,smp,expected,previous,repeat=repeat));save();previous=out/f'{fw}-{smp}-repeat{repeat}'/'enrolled.img'
   if args.negatives:
    for negative in ('signature','partial','foreign','read-error'):m['cases'].append(boot(out,work,iso,'bios',1,expected,source,negative));save()
   m['boot_runtime']='PASS'
 except BaseException as e:m['error']=repr(e);m['boot_runtime']='FAIL';raise
 finally:
  m['changed_compiled_copies']=[p for p,h in m.get('compiled_inputs',{}).items() if H(work/p)!=h]
  m['changed_repository_sources']=[p for p,h in sources.items() if H(ROOT/p)!=h]
  m['changed_guards']=[p for p,h in guards.items() if H(ROOT/p)!=h]
  m['preservation']='FAIL' if any(m[k] for k in ('changed_compiled_copies','changed_repository_sources','changed_guards')) else 'PASS';save();print('Evidence:',out,m['boot_runtime'],m['preservation'],flush=True)
 assert m['preservation']=='PASS'
if __name__=='__main__':main()
