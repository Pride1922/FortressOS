"""Current Phase2 Ring3 regression, isolated explicit test kernel, disposable USB."""
from pathlib import Path
import hashlib,json,os,shutil,subprocess,tempfile
root=Path(__file__).resolve().parent.parent
out=Path(tempfile.mkdtemp(prefix='phase2-',dir=root/'build/permissions-phase5'))
workspace=Path(subprocess.check_output(['python3','scripts/create_ext4_guest_workspace.py'],cwd=root,text=True).strip())
fixture=root/'build/permissions-phase0/inodes-l920f8r4/ext4-256.img'
assert fixture.is_file() and workspace.is_relative_to(root/'.codex-remote-attachments/ext4-phase9')
with (out/'build.log').open('w') as log:
    subprocess.run(['make','-j4','LOGIN_TEST=1','PERMISSIONS_TEST=1','bin/fortress.elf','bin/initramfs.tar','bin/fortress.iso','build/perm_phase2_user.elf'],cwd=workspace,stdout=log,stderr=subprocess.STDOUT,check=True)
dest=workspace/'build/permissions-phase0/source.ext4';dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(fixture,dest)
with (out/'guest.log').open('w') as log:
    subprocess.run(['python3','scripts/test_perm_guest.py',str(dest),'--usb'],cwd=workspace,
        env={**os.environ,'PERM_PHASE2_EXPECT':'1','PERM_LOGIN_BYPASS':'1'},stdout=log,stderr=subprocess.STDOUT,check=True)
(out/'manifest.json').write_text(json.dumps({'workspace':str(workspace),'source_fixture':str(fixture),'fixture_sha256':hashlib.sha256(fixture.read_bytes()).hexdigest(),
 'scope':'4 BIOS/UEFI SMP=1/4 cases; explicit test-only self-drop/login bypass; USB regular-file fixture only; exact sources and retained ISO/OVMF/disks in workspace'},indent=2))
print('PASS Phase2 current regression; evidence',out)
