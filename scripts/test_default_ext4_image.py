"""Verify default journaled EXT4 image via disposable USB boot/write/reboot."""
import argparse,hashlib,shutil,subprocess,tempfile
from pathlib import Path
from test_usb_persistence import run_qemu_session,send_command,check_offline_ext2
p=argparse.ArgumentParser();p.add_argument('--firmware',choices=('bios','uefi'),required=True)
p.add_argument('--cpus',type=int,choices=(1,4,8),default=8);p.add_argument('--output',type=Path,required=True)
a=p.parse_args();root=Path(__file__).resolve().parent.parent;out=a.output.resolve()
assert out.is_relative_to(root/'build') and out!=root/'build';out.mkdir(exist_ok=False)
source=root/'bin/fortress.img';before=hashlib.sha256(source.read_bytes()).hexdigest()
disk=out/'disposable.img';shutil.copyfile(source,disk)
def write(qmp,child,log):
 send_command(qmp,child,log,'cat /mnt/README.txt\n','Filesystem: ext4')
 send_command(qmp,child,log,'echo ext4-default-persistence > /mnt/default-check.txt\n',' $ ')
 send_command(qmp,child,log,'sync\n','Filesystem synced.')
 send_command(qmp,child,log,'shutdown\n','Shutdown initiated')
def read(qmp,child,log):
 send_command(qmp,child,log,'cat /mnt/default-check.txt\n','ext4-default-persistence')
 send_command(qmp,child,log,'rm /mnt/default-check.txt\n',' $ ')
 send_command(qmp,child,log,'sync\n','Filesystem synced.')
 send_command(qmp,child,log,'shutdown\n','Shutdown initiated')
for number,action in enumerate((write,read),1):
 run_qemu_session(a.firmware,disk,out/f'boot-{number}.log',action,'default-ext4',cpus=a.cpus)
 check_offline_ext2(disk)
with tempfile.TemporaryDirectory(prefix='fortress-default-ext4-') as temp:
 part=Path(temp)/'data.ext4'
 with disk.open('rb') as stream:stream.seek(133120*512);part.write_bytes(stream.read(131072*512))
 info=subprocess.run(['dumpe2fs','-h',str(part)],check=True,capture_output=True,text=True).stdout
 assert 'has_journal' in info and 'extent' in info
 (out/'dumpe2fs.txt').write_text(info)
assert hashlib.sha256(source.read_bytes()).hexdigest()==before
(out/'result.txt').write_text(f'PASS: default journaled EXT4 {a.firmware} SMP={a.cpus}; create/sync/shutdown/reboot/read/unlink, independent Linux fsck after each boot, immutable shipped image. No physical claim.\n')
print((out/'result.txt').read_text())
