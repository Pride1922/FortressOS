"""Isolated trace kernel + actual shell and Ring3 Phase1 acceptance. No data disks
except explicitly generated EXT2 shell fixtures and disposable journaled EXT4.
Never rebuild/attach the normal fortress.img. No hardware/IRQ claim from mocks.
"""
from pathlib import Path
import hashlib,json,os,re,subprocess,shutil
ROOT=Path(__file__).resolve().parent.parent
out=ROOT/'build/permissions-phase1';out.mkdir(parents=True,exist_ok=True)
image=ROOT/'bin/fortress.img'
before=hashlib.sha256(image.read_bytes()).hexdigest() if image.is_file() else None
subprocess.run(['python3','scripts/audit_permissions_wiring.py'],cwd=ROOT,check=True)
workspace=Path(subprocess.check_output(['python3','scripts/create_ext4_guest_workspace.py'],cwd=ROOT,text=True).strip())
assert workspace.is_relative_to(ROOT/'.codex-remote-attachments/ext4-phase9')
with (out/'trace-build.log').open('w') as log:
 subprocess.run(['make','PERMISSIONS_TRACE=1','bin/fortress.elf','bin/initramfs.tar','bin/fortress.iso','build/perm_user.elf','nvme-gpt-disk'],cwd=workspace,stdout=log,stderr=subprocess.STDOUT,check=True)
env={**os.environ,'PERM_TRACE_EXPECT':'1'}
with (out/'trace-shell.log').open('w') as log:
 subprocess.run(['python3','scripts/test_shell.py'],cwd=workspace,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
shell={}
for firmware in ('bios','uefi'):
 source=workspace/f'build/shell-{firmware}-1cpu.log';target=out/source.name;shutil.copyfile(source,target)
 records=re.findall(r'PERM TRACE mask=(\d+) euid=(\d+) egid=(\d+) caps=(\S+)',source.read_text(errors='replace'))
 assert records and {1,3,4,6} <= {int(m) for m,_,_,_ in records},firmware
 shell[firmware]={'records':len(records),'raw_sha256':hashlib.sha256(source.read_bytes()).hexdigest()}
# Require an explicit Phase0-tested journaled source, never infer/create EXT2.
source_arg=os.environ.get('PERM_PHASE1_FIXTURE')
assert source_arg,'Set PERM_PHASE1_FIXTURE to a disposable journaled EXT4 inode matrix result'
fixture=Path(source_arg).resolve();assert fixture.is_file() and fixture.is_relative_to(ROOT/'build/permissions-phase0')
destination=workspace/'build/permissions-phase0/source.ext4';destination.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(fixture,destination)
with destination.open('rb') as f:
 f.seek(1024+92);assert int.from_bytes(f.read(4),'little') & 4,'journal required'
with (out/'trace-guest.log').open('w') as log:
 subprocess.run(['python3','scripts/test_perm_guest.py',str(destination),'--usb'],cwd=workspace,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
after=hashlib.sha256(image.read_bytes()).hexdigest() if image.is_file() else None
assert before==after,'normal image changed'
manifest={'workspace':str(workspace),'shell':shell,'guest_log':'trace-guest.log',
 'normal_image_sha256':after,'kernel_sha256':hashlib.sha256((workspace/'bin/fortress.elf').read_bytes()).hexdigest(),
 'source_snapshot':'workspace-manifest.json','source_fixture_sha256':hashlib.sha256(fixture.read_bytes()).hexdigest(),
 'scope':'permissive wiring, finite BIOS/UEFI shell and USB Ring3 cases; no enforcement'}
(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(f'PASS Phase1 trace shell BIOS/UEFI and Ring3 USB x SMP1/4; evidence {out}',flush=True)
