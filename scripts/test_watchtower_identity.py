#!/usr/bin/env python3
"""Real service credentials, disk-free BIOS/UEFI SMP=1/4.

Temporary ISO made from the existing release archive plus rebuilt kernel,
Watchtower and acceptance-only probe. Release ISO/image/archive untouched.
No private keys, passwords, data disks or physical targets.
"""
from pathlib import Path
import hashlib
import io
import re
import shutil
import subprocess
import tarfile
import tempfile
import test_logging

ROOT=Path(__file__).resolve().parent.parent

def run(args):
    subprocess.run([str(a) for a in args],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)

def fixture(tmp,ssh_daemon=None):
    tree=tmp/'tree'
    assert (ROOT/'bin/initramfs.tar').is_file(),'Build the base archive with make bin/initramfs.tar first'
    (tree/'boot/limine').mkdir(parents=True);(tree/'EFI/BOOT').mkdir(parents=True)
    for name in ('limine-bios.sys','limine-bios-cd.bin','limine-uefi-cd.bin'):
        shutil.copyfile(ROOT/'limine'/name,tree/'boot/limine'/name)
    shutil.copyfile(ROOT/'limine/BOOTX64.EFI',tree/'EFI/BOOT/BOOTX64.EFI')
    config='timeout: 0\n/Identity acceptance\n    protocol: limine\n    kernel_path: boot():/boot/fortress.elf\n    module_path: boot():/boot/initramfs.tar\n    kernel_cmdline: net=dhcp\n'
    for name in ('boot/limine/limine.conf','boot/limine.conf'):(tree/name).write_text(config)
    shutil.copyfile(ROOT/'bin/fortress.elf',tree/'boot/fortress.elf')
    replacements={
        'sbin/watchtower':((ROOT/'build/watchtower.elf').read_bytes(),0o755),
        'bin/identity-probe':((ROOT/'build/watchtower-identity-probe.elf').read_bytes(),0o755),
        'bin/identity-setid':((ROOT/'build/watchtower-identity-probe.elf').read_bytes(),0o4755),
        'bin/identity-private':((ROOT/'build/watchtower-identity-probe.elf').read_bytes(),0o700),
    }
    if ssh_daemon is not None:replacements['sbin/sshd']=(ssh_daemon.read_bytes(),0o755)
    configs={
        'identity-root':('root','root','/bin/identity-probe root','/'),
        'identity-user':('operator','operator','/bin/identity-probe user','/'),
        'identity-numeric':('1000','1000','/bin/identity-probe numeric','/'),
        'identity-override':('operator','video','/bin/identity-probe override','/'),
        'identity-setid':('operator','operator','/bin/identity-setid setid','/'),
        'identity-baduser':('missing','root','/bin/identity-probe BAD','/'),
        'identity-badgroup':('root','missing','/bin/identity-probe BAD','/'),
        'identity-badexec':('operator','operator','/bin/identity-private BAD','/'),
        'identity-badcwd':('operator','operator','/bin/identity-probe BAD','/root'),
    }
    for name,(user,group,command,cwd) in configs.items():
        text=f'Exec={command}\nUser={user}\nGroup={group}\nWorkingDirectory={cwd}\nRestart=never\nOrder=90\nEnabled=no\n'
        replacements[f'etc/watchtower/{name}.service']=(text.encode(),0o644)
    archive=tree/'boot/initramfs.tar'
    with tarfile.open(ROOT/'bin/initramfs.tar','r:') as src,tarfile.open(archive,'w',format=tarfile.USTAR_FORMAT) as dst:
        for entry in src:
            name=entry.name.removeprefix('./')
            if name in replacements:continue
            dst.addfile(entry,src.extractfile(entry) if entry.isfile() else None)
        for name,(data,mode) in replacements.items():
            entry=tarfile.TarInfo(name);entry.size=len(data);entry.mode=mode
            dst.addfile(entry,io.BytesIO(data))
    shutil.copyfile(archive,tree/'initramfs.tar')
    (tmp/'bin').mkdir();iso=tmp/'bin/fortress.iso'
    run(['xorriso','-as','mkisofs','-b','boot/limine/limine-bios-cd.bin','-no-emul-boot',
         '-boot-load-size','4','-boot-info-table','--efi-boot','boot/limine/limine-uefi-cd.bin',
         '-efi-boot-part','--efi-boot-image','--protective-msdos-label',tree,'-o',iso])
    run([ROOT/'limine/limine','bios-install',iso])
    return configs

def test(mode,smp,tmp,configs):
    guest=test_logging.Guest(mode,smp,tmp)
    try:
        guest.wait(lambda t:t.endswith('FortressOS login: '));guest.send('operator')
        guest.wait(lambda t:t.endswith('$ '))
        for name in configs:
            result=guest.command('sudo service start '+name)
            if 'bad' in name:
                assert 'failed' in result.lower(),result
                status=guest.command('sudo service status '+name)
                assert 'State: failed' in status and 'Readiness: ready' not in status,status
            else:
                expected={'identity-root':'root','identity-user':'user','identity-numeric':'numeric',
                          'identity-override':'override','identity-setid':'setid'}[name]
                # Poll through CLI to let pipe drain/reaping complete; no fixed long sleep.
                for _ in range(25):
                    result=guest.command('sudo cat /run/watchtower/'+name+'.log')
                    assert 'IDENTITY_FAIL' not in result,result
                    if 'IDENTITY_PASS:'+expected in result:break
                    guest.command('sleep 0.1')
                else:raise AssertionError(name+': '+result)
        # A fresh restart must resolve/enforce again and preserve supervisor authority.
        guest.command('sudo service restart identity-user')
        for _ in range(25):
            result=guest.command('sudo cat /run/watchtower/identity-user.log')
            if result.count('IDENTITY_PASS:user')==2:break
            guest.command('sleep 0.1')
        else:raise AssertionError('restart identity evidence missing: '+result)
        assert 'IDENTITY_PASS:reduced' in guest.command('sudo cat /run/watchtower/identity-root.log')
        assert 'State: running' in guest.command('sudo service status crond')
        result=guest.command('sudo id');assert 'uid=0 gid=0' in result,result
        assert 'IDENTITY_FAIL' not in guest.text(),guest.text()[-3000:]
        assert not re.search(r'\[PROCESS FAULT\][\s\S]{0,200}\(/bin/identity-',guest.text())
        print(f'PASS {mode} SMP={smp}: named/numeric/root/override/set-id identity, permission denial, ABI boundaries, invalid identity/exec/cwd fail closed',flush=True)
    finally:guest.close()

if __name__=='__main__':
    retained=[ROOT/'bin'/name for name in ('fortress.iso','fortress.img','initramfs.tar')]
    before={p:hashlib.sha256(p.read_bytes()).digest() for p in retained if p.exists()}
    with tempfile.TemporaryDirectory(prefix='fortress-wt-identity-') as directory:
        tmp=Path(directory);configs=fixture(tmp);test_logging.ROOT=tmp
        for firmware in ('bios','uefi'):
            for smp in (1,4):test(firmware,smp,tmp,configs)
    assert all(hashlib.sha256(p.read_bytes()).digest()==digest for p,digest in before.items())
    print('PASS release ISO/image/archive unchanged; probe and configs confined to temporary fixture')
