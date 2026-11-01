#!/usr/bin/env python3
"""Disposable actual USB -> NVMe kernel/package acceptance, BIOS and UEFI."""
import argparse
import hashlib
import json
from pathlib import Path
import selectors
import shutil
import struct
import test_root_phase2 as root_gate
import subprocess
import tempfile
import test_installer as install
import test_pkg_upgrade_qemu as qemu
import test_root_phase0 as fixture
import test_rot13_installer as rot13
ROOT=fixture.REPO

def login(guest):
    guest.wait(lambda t:'FortressOS login: ' in t)
    assert '[ROOT] Installed root admitted by PARTUUID' in guest.text()
    for attempt in range(3):
        start=len(guest.raw);guest.send('operator');guest.wait(lambda t:t.endswith('Password: '),start)
        start=len(guest.raw);guest.send(rot13.PASSWORD)
        text=guest.wait(lambda t:qemu.PROMPT.search(t) or 'Accounts changed; retry login' in t,start)
        if qemu.PROMPT.search(text):return
        guest.actions.append({'authentication_retry':'explicit Accounts changed response','attempt':attempt+1})
        guest.wait(lambda t:'FortressOS login: ' in t,start)
    raise AssertionError('account state changed on all three bounded authentication attempts')

def root_hash(disk):
    h=hashlib.sha256()
    with disk.open('rb') as stream:
        stream.seek(135168*512);remaining=disk.stat().st_size-135168*512-33*512
        while remaining:
            data=stream.read(min(1024*1024,remaining));assert data;h.update(data);remaining-=len(data)
    return h.hexdigest()

def update(out,disk,media,firmware,refuse=False):
    assert disk.is_file() and disk.resolve().is_relative_to(out.resolve())
    source=out/('repeat-usb.img' if refuse else 'update-usb.img');shutil.copyfile(media/'fortress-installer.img',source)
    argv=['qemu-system-x86_64','-accel','kvm','-M','q35','-cpu','host','-m','2G','-smp','4','-display','none','-monitor','none','-no-reboot','-net','none','-serial','stdio',
          '-drive',f'file={disk},if=none,id=target,format=raw','-device','nvme,drive=target,serial=KERNELUPDATE',
          '-drive',f'file={source},if=none,id=source,format=raw,readonly=on','-device',('ich9-usb-ehci1' if firmware=='uefi' else 'qemu-xhci')+',id=usb',
          '-device','usb-storage,bus=usb.0,drive=source,bootindex=1']
    label='repeat' if refuse else 'update'
    if firmware=='uefi':
        variables=out/(label+'.vars.fd');shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
        argv+=['-drive','if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd','-drive',f'if=pflash,format=raw,unit=1,file={variables}']
    assert not any('/dev/' in item for item in argv) and argv.count('-device')==3 and argv.count('-drive')==(4 if firmware=='uefi' else 2)
    (out/(label+'.argv.json')).write_text(json.dumps(argv,indent=2)+'\n')
    guest=qemu.Guest.__new__(qemu.Guest);guest.out=out;guest.label=label;guest.raw=bytearray();guest.counter=0;guest.actions=[]
    guest.error=(out/(label+'.stderr.log')).open('wb');guest.process=subprocess.Popen(argv,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=guest.error)
    guest.selector=selectors.DefaultSelector();guest.selector.register(guest.process.stdout,selectors.EVENT_READ)
    before=root_hash(disk);source_hash=fixture.digest(source)
    try:
        guest.wait(lambda t:'Select disk index' in t,timeout=180);guest.send('0')
        if refuse:guest.wait(lambda t:'wrong-version installation; no writes.' in t,timeout=120)
        else:
            guest.wait(lambda t:'Type UPDATE' in t,timeout=120);guest.send('UPDATE');guest.wait(lambda t:'KERNEL UPDATE COMPLETE' in t,timeout=180)
    finally:guest.close()
    assert root_hash(disk)==before,'offline update changed installed root bytes'
    assert fixture.digest(source)==source_hash,'update USB modified'

def host(out,disk,record):
    binary=out/'host';flags=['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-ffunction-sections','-fdata-sections','-Wl,--gc-sections']
    flags+=['-I'+str(ROOT/p) for p in ('src/include','src/lib','src/kernel','src/fs','src/drivers','src/mm','src/arch/x86_64')]
    units=['tests/kernel_update_host.c','src/lib/kernel_update.c','src/lib/install_fat.c','src/kernel/root_boot.c','src/fs/gpt.c','src/lib/crc32.c','user/tools/digest.c']
    fixture.run(flags+[str(ROOT/p) for p in units]+['-o',str(binary)],out/'host.log',180)
    edited=out/'host-edited-esp.img'
    fixture.run([str(binary),str(disk),record['candidate_kernel'],record['baseline_kernel_sha256'],str(edited)],out/'host.log',180)
    fixture.run(['fsck.fat','-n',str(edited)],out/'host.log')

def audit_boot(out,disk,record):
    esp=out/'updated-esp.img'
    with disk.open('rb') as stream:stream.seek(4096*512);esp.write_bytes(stream.read(64*1024*1024))
    fixture.run(['fsck.fat','-n',str(esp)],out/'updated-fat-audit.log')
    def extract(path,label):
        dest=out/('updated-'+label);fixture.run(['mcopy','-i',str(esp),'::/'+path,str(dest)],out/'updated-fat-audit.log');return dest.read_bytes()
    old=extract('boot/fortress.elf','kernel-a');new=extract('boot/kernel-b.elf','kernel-b')
    assert hashlib.sha256(old).hexdigest()==record['baseline_kernel_sha256']
    assert new==Path(record['candidate_kernel']).read_bytes().ljust(8*1024*1024,b'\0')
    bootstrap=extract('boot/bootstrap.tar','bootstrap')
    assert bootstrap==(out/'installed-bootstrap.tar').read_bytes()
    original=dict(line.split('=',1) for line in (out/'installed-readiness.v1').read_bytes().rstrip(b'\0').decode().splitlines()[1:])
    for i,path in enumerate(('boot/readiness.v1','boot/ready-b.v1')):
        raw=extract(path,f'ready-{i}');assert len(raw)==512
        values=dict(line.split('=',1) for line in raw.rstrip(b'\0').decode().splitlines()[1:])
        for name in ('install_id','root_partuuid','filesystem_uuid','payload_version','bootstrap_sha256'):assert values[name]==original[name]
        assert values['kernel_sha256']==hashlib.sha256(new if i else old).hexdigest()
        assert values['checksum']==hashlib.sha256(raw[:raw.index(b'checksum=')]).hexdigest()
    menus=[extract(path,f'config-{i}') for i,path in enumerate(('limine.conf','boot/limine.conf','boot/limine/limine.conf','efi/boot/limine.conf'))]
    assert all(m==menus[0] and m.startswith(b'default_entry: 3\n') for m in menus)
    return {'result':'PASS','fat_fsck':'PASS','previous_kernel_exact':True,'candidate_kernel_exact':True,'bootstrap_exact':True,'both_readiness_records':'PASS','four_selectors':'B'}

def select_copy(out,source,entry,label):
    """Select an existing menu entry on a disposable copy; no boot files replaced."""
    out.mkdir();disk=out/'target.img';shutil.copyfile(source,disk)
    esp=out/'esp.img'
    with disk.open('rb') as f:f.seek(4096*512);esp.write_bytes(f.read(64*1024*1024))
    # Extract canonical configurations with independent mtools, then locate their
    # complete bytes to change only each selector digit in this test copy.
    data=bytearray(esp.read_bytes())
    changed=0
    for i,path in enumerate(('limine.conf','boot/limine.conf','boot/limine/limine.conf','efi/boot/limine.conf')):
        config=out/f'config-{i}';fixture.run(['mcopy','-i',str(esp),'::/'+path,str(config)],out/'select.log')
        content=config.read_bytes();assert content.startswith(b'default_entry: 3\n')
        matches=[];at=0
        while (at:=data.find(content,at))>=0:matches.append(at);at+=len(content)
        # All four are identical initially; change every matching copy once.
        for at in matches:data[at+15]=ord(str(entry));changed+=1
        if not matches:assert entry!=3
    assert changed==4
    with disk.open('r+b') as f:f.seek(4096*512);f.write(data)
    return disk

def boot_sets(case,firmware,result):
    source=case/'target.img'
    previous=case/'previous-entry';disk=select_copy(previous,source,1,'previous')
    guest=qemu.Guest(previous,disk,firmware,4,'previous-kernel','kvm')
    try:
        login(guest);assert '[KERNEL RELEASE] kernel-update-baseline-1' in guest.text()
        assert '\nrot13 2.0\n' in guest.command('/usr/bin/rot13 --version');guest.root();guest.shutdown()
    finally:guest.close()
    for entry,name in ((2,'previous-recovery'),(4,'new-recovery')):
        out=case/name;disk=select_copy(out,source,entry,name);before=fixture.digest(disk)
        root_gate.guest(out,disk,firmware,4,name,rot13.PASSWORD,rot13.RECOVERY,result,recovery=True,accel='kvm')
        assert fixture.digest(disk)==before,'authenticated recovery changed disk'
    result['cases'].append({'firmware':firmware,'previous_installed_kernel':'PASS','old_and_new_authenticated_recovery':'PASS',
                            'selection':'existing menu selector changed on disposable copies; no automatic failed-boot claim'})


def resume_boot_sets(record,retained,firmwares):
    """Finish boot-set gates from preserved post-upgrade disks; retain first failure."""
    out=Path(tempfile.mkdtemp(prefix='finish-',dir=ROOT/'build/kernel-update'));out.chmod(0o755)
    result={'result':'FAIL','scope':'post-upgrade boot-set completion from retained disposable disks','retained_campaign':str(retained.resolve()),'cases':[],'physical_writes':False}
    print(out,flush=True)
    try:
        for firmware in firmwares:
            original=retained/firmware;case=out/firmware;case.mkdir();disk=case/'target.img';shutil.copyfile(original/'target.img',disk)
            guest=qemu.Guest(case,disk,firmware,4,'candidate-confirmation','kvm')
            try:
                login(guest);assert '[KERNEL RELEASE] kernel-update-candidate-2' in guest.text()
                assert '\nrot13 2.0\n' in guest.command('/usr/bin/rot13 --version');guest.root();guest.shutdown()
            finally:guest.close()
            accounts={path:(original/('before-'+path.replace('/','-'))).read_bytes() for path in ('/etc/passwd','/etc/group','/etc/shadow','/etc/pkg/trust.conf','/etc/pkg/repository.conf')}
            audit=rot13.audit(case,disk,'package-confirmation',2,fixture.digest(ROOT/'build/pkg-rot13/rot13-v2.elf'),accounts,expected_pkg=Path(record['templates'])/'tree/bin/pkg')
            result['cases'].append({'firmware':firmware,'candidate_kernel_package_reboot':'PASS','linux_package_audit':audit})
            boot_sets(case,firmware,result)
        result['result']='PASS';print(json.dumps(result,indent=2),flush=True)
    finally:(out/'manifest.json').write_text(json.dumps(result,indent=2)+'\n')


def main():
    p=argparse.ArgumentParser();p.add_argument('--resume-boot-sets',type=Path);p.add_argument('--manifest',type=Path,required=True);p.add_argument('--firmware',choices=['bios','uefi','all'],default='all');a=p.parse_args();record=json.loads(a.manifest.read_text())
    assert fixture.digest(Path(record['candidate_kernel']))==record['candidate_sha256']
    for value in record['outputs'].values():assert fixture.digest(Path(value['path']))==value['sha256']
    if a.resume_boot_sets:
        qemu.PASSWORD=rot13.PASSWORD;resume_boot_sets(record,a.resume_boot_sets,(('bios','uefi') if a.firmware=='all' else (a.firmware,)));return
    parent=ROOT/'build/kernel-update';out=Path(tempfile.mkdtemp(prefix='accept-',dir=parent));out.chmod(0o755);print(out,flush=True)
    result={'result':'FAIL','media_manifest':str(a.manifest.resolve()),'cases':[],'physical_writes':False};install.ACCEL='kvm';qemu.PASSWORD=rot13.PASSWORD
    try:
        for firmware in (('bios','uefi') if a.firmware=='all' else (a.firmware,)):
            case=out/firmware;case.mkdir();disk=install.install(case,Path(record['baseline_media']),firmware,384,result,source='usb',disposable_gate=False,operator=rot13.PASSWORD,recovery=rot13.RECOVERY)
            install.audit(disk,case,Path(record['baseline_kernel']),rot13.PASSWORD,rot13.RECOVERY,result)
            # Keep a pre-update regular-file fixture for host faults and fallback.
            baseline=case/'baseline.img';shutil.copyfile(disk,baseline)
            if firmware=='bios':host(case,baseline,record)
            guest=qemu.Guest(case,disk,firmware,4,'baseline-boot','kvm')
            try:
                login(guest);assert '[KERNEL RELEASE] kernel-update-baseline-1' in guest.text()
                assert '\nrot13 1.0\n' in guest.command('/usr/bin/rot13 --version')
                guest.command('echo KEEP_AFTER_UPGRADE > /home/operator/upgrade-note');guest.root();guest.shutdown()
            finally:guest.close()
            before=root_hash(disk);update(case,disk,Path(record['update_media']),firmware)
            assert root_hash(disk)==before
            result['cases'].append({'firmware':firmware,'updated_esp_linux_oracle':audit_boot(case,disk,record)})
            # Reusing the same predecessor-bound media must be a read-only refusal.
            unchanged=fixture.digest(disk);update(case,disk,Path(record['update_media']),firmware,refuse=True);assert fixture.digest(disk)==unchanged
            accounts={}
            for path in ('/etc/passwd','/etc/group','/etc/shadow','/etc/pkg/trust.conf','/etc/pkg/repository.conf'):
                target=case/('before-'+path.replace('/','-'));fixture.run(['debugfs','-R',f'dump {path} {target}',str(case/'installed-root.img')],case/'before.log');accounts[path]=target.read_bytes()
            guest=qemu.Guest(case,disk,firmware,4,'new-kernel-package-upgrade','kvm')
            try:
                login(guest);assert '[KERNEL RELEASE] kernel-update-candidate-2' in guest.text()
                assert '\nrot13 1.0\n' in guest.command('/usr/bin/rot13 --version');guest.root()
                guest.command('/sbin/ifup 10.0.2.15/24 10.0.2.2 10.0.2.3');guest.command('/bin/pkg update');guest.command('/bin/pkg get rot13')
                assert 'upgrade complete' in guest.command('/bin/pkg upgrade rot13');assert '\nrot13 2.0\n' in guest.command('/usr/bin/rot13 --version');guest.shutdown()
            finally:guest.close()
            guest=qemu.Guest(case,disk,firmware,4,'new-kernel-reboot','kvm')
            try:
                login(guest);assert '[KERNEL RELEASE] kernel-update-candidate-2' in guest.text();assert '\nrot13 2.0\n' in guest.command('/usr/bin/rot13 --version');guest.root();guest.shutdown()
            finally:guest.close()
            # Linux oracle for package bytes, record, accounts and user setting.
            expected=fixture.digest(ROOT/'build/pkg-rot13/rot13-v2.elf')
            rot13.audit(case,disk,'final',2,expected,accounts,expected_pkg=Path(record['templates'])/'tree/bin/pkg')
            boot_sets(case,firmware,result)
            result['cases'].append({'firmware':firmware,'result':'PASS','root_unchanged_by_update':before,'kernel_release':'kernel-update-candidate-2','rot13':'1.0 -> 2.0 -> reboot'})
        result['result']='PASS';print(json.dumps(result,indent=2),flush=True)
    finally:(out/'manifest.json').write_text(json.dumps(result,indent=2)+'\n')
if __name__=='__main__':main()
