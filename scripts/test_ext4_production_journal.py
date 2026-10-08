"""Ordinary USB journal dispatch: no filesystem fixture flags or test build gates."""
import sys,json,hashlib,shutil,tempfile,subprocess,selectors,time
from pathlib import Path
import test_ext4_physical_image as physical
from test_net_pci import VARS
from test_nmi_transitions import REPO
from audit_ext4_open_unlink import verify_reclaimed

def boot(mode,disk,variables,out,label,expected,actions):
 cmd=physical.command(mode,disk,variables,1,out/'fixture.iso');(out/(label+'-argv.json')).write_text(json.dumps(cmd,indent=2))
 transcript=bytearray();p=None;sel=selectors.DefaultSelector()
 with (out/(label+'-stderr.log')).open('wb') as err:
  try:
   p=subprocess.Popen(cmd,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=err);sel.register(p.stdout,selectors.EVENT_READ)
   def wait(predicate,limit=180):
    end=time.monotonic()+limit
    while time.monotonic()<end:
     for key,_ in sel.select(.1):transcript.extend(key.fileobj.read1(65536))
     s=transcript.decode(errors='replace');assert '[FAIL]' not in s and 'PANIC' not in s,s[-2000:]
     if predicate(s):return s
     assert p.poll() is None,s[-2000:]
    raise AssertionError('boot/shell timeout')
   s=wait(lambda s:'[BOOT] Interactive shell ready.' in s and ' $ ' in s)
   assert expected in s,s[-3000:]
   assert '[EXT4 PHYSICAL]' not in s and '[EXT4 JOURNAL] PASS' not in s
   for command,wanted in actions:
    start=len(transcript);p.stdin.write((command+'\n').encode());p.stdin.flush()
    s=wait(lambda s:' $ ' in s[start:],45)[start:]
    assert wanted in s,(command,s)
   p.stdin.write(b'shutdown\n');p.stdin.flush();p.wait(timeout=45);assert p.returncode==0
  finally:
   if p and p.poll() is None:p.kill();p.wait(timeout=10)
   if p:transcript.extend(p.stdout.read())
   sel.close();(out/(label+'.log')).write_bytes(transcript)

def main():
 manifest=Path(sys.argv[1]).resolve();info=json.loads(manifest.read_text());root=REPO/'.codex-remote-attachments/ext4-phase9'
 assert manifest.is_relative_to(root.resolve());out=Path(tempfile.mkdtemp(prefix='journaled-production-',dir=root));rows=[];errors=[]
 baseline=Path(info['data_source']); pending=root/'physical-start-capture-69ad78f0a97442a59335a24affe4f471/after-start.ext4'
 try:
  for mode in ('bios','uefi'):
   disk=out/(mode+'.img');subprocess.run(['cp','--sparse=always',info['image'],str(disk)],check=True)
   variables=out/(mode+'-vars.fd')
   if mode=='uefi':shutil.copyfile(VARS,variables)
   shutil.copyfile(Path(info['workspace'])/'bin/fortress.iso',out/'fixture.iso') if not (out/'fixture.iso').exists() else None
   # Recovery-needed RO must not replay/write. Ordinary image, no test flags.
   with disk.open('r+b') as f:f.seek(68157440);f.write(pending.read_bytes())
   for token,label in [('usb_data=PARTUUID='+info['data_partuuid']+' usb_data_mode=ro','ro'),('usb_data=PARTUUID=11223344-5566-7788-99aa-bbccddeeff00 usb_data_mode=rw','wrong-target')]:
    conf=f'timeout: 0\n\n/FortressOS\n    protocol: limine\n    kernel_path: boot():/boot/fortress.elf\n    module_path: boot():/boot/initramfs.tar\n    kernel_cmdline: {token}\n'
    physical.put_config(disk,out/(mode+'-'+label+'.conf'),conf)
    boot(mode,disk,variables,out,mode+'-'+label,'/mnt left unmounted',[])
    captured=out/(mode+'-'+label+'.ext4');physical.partition(disk,captured);assert captured.read_bytes()==pending.read_bytes()
   conf=f'timeout: 0\n\n/FortressOS\n    protocol: limine\n    kernel_path: boot():/boot/fortress.elf\n    module_path: boot():/boot/initramfs.tar\n    kernel_cmdline: usb_data=PARTUUID={info["data_partuuid"]} usb_data_mode=rw\n'
   physical.put_config(disk,out/(mode+'.conf'),conf)
   boot(mode,disk,variables,out,mode+'-rw1','[USB E4-B] Selected filesystem: ext4 journaled',[
    ('ls /mnt/cut-commit.txt','cut-commit.txt'),('echo production-journal > /mnt/production-check.txt',' $ '),('sync','Filesystem synced.'),('cat /mnt/production-check.txt','production-journal'),('disk list','sda')])
   boot(mode,disk,variables,out,mode+'-rw2','read-write at /mnt',[
    ('cat /mnt/production-check.txt','production-journal'),('mkdir /mnt/live-dir',' $ '),('mv /mnt/production-check.txt /mnt/live-dir/check.txt',' $ '),('cat /mnt/live-dir/check.txt','production-journal'),('rm /mnt/live-dir/check.txt',' $ '),('rm /mnt/live-dir',' $ '),('sync','Filesystem synced.')])
   final=out/(mode+'-final.ext4');physical.partition(disk,final);verify_reclaimed(final,baseline,out)
   rows.append({'mode':mode,'production_recovery':True,'reboot_persistence_namespace_sync':True,'ro_wrong_target_zero_writes':True});print('PASS production',mode,flush=True)
 except Exception as e:errors.append(repr(e));raise
 finally:
  (out/'manifest.json').write_text(json.dumps({'artifact':str(manifest),'cases':rows,'errors':errors,'physical_writes':False},indent=2));print('Evidence:',out,flush=True)
if __name__=='__main__':main()
