#!/usr/bin/env python3
"""Second menu choice: real NVMe inventory + full payload dry-run, no install.

Only disposable regular-file USB/NVMe targets; no account secrets or host keys.
The shipped image is never edited. Default-entry override selects menu choice 2
in the private copy so firmware keyboard timing cannot hide a packaging failure.
"""
from pathlib import Path
import argparse
import hashlib
import re
import selectors
import shutil
import subprocess
import tempfile
import time
from create_combined_media import ROOT,layout,verify

def sha(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()

def test(image,firmware):
    with tempfile.TemporaryDirectory(prefix='fortress-combined-installer-') as directory:
        tmp=Path(directory);usb=tmp/'usb.img';shutil.copyfile(image,usb)
        fat=f'{usb}@@{layout(usb)[0][2]*512}'
        config=tmp/'limine.conf'
        subprocess.run(['mcopy','-i',fat,'::boot/limine/limine.conf',str(config)],check=True,capture_output=True)
        config.write_text(config.read_text().replace('timeout: 5','timeout: 0\ndefault_entry: 2'))
        for name in ('limine.conf','boot/limine.conf','boot/limine/limine.conf','EFI/BOOT/limine.conf'):
            subprocess.run(['mcopy','-o','-i',fat,str(config),'::'+name],check=True,capture_output=True)
        target=tmp/'target.img'
        with target.open('wb') as stream:stream.truncate(384*1024**2)
        original=sha(target)
        controller='ich9-usb-ehci1,id=usb' if firmware=='uefi' else 'qemu-xhci,id=usb'
        args=['qemu-system-x86_64','-M','q35','-m','2G','-smp','1','-accel','tcg',
              '-cpu','max,+rdrand,+rdseed','-display','none','-monitor','none','-serial','stdio','-no-reboot','-net','none',
              '-drive',f'if=none,id=media,format=raw,file={usb}','-device',controller,
              '-device','usb-storage,bus=usb.0,drive=media,bootindex=1',
              '-drive',f'if=none,id=target,format=raw,file={target}',
              '-device','nvme,drive=target,serial=COMBINED-DISPOSABLE']
        if firmware=='uefi':
            variables=tmp/'vars.fd';shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
            args+=['-drive','if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
                   '-drive',f'if=pflash,format=raw,unit=1,file={variables}']
        assert all(p.is_file() and not p.is_symlink() for p in (usb,target))
        assert not any('/dev/' in arg for arg in args)
        proc=subprocess.Popen(args,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        selector=selectors.DefaultSelector();selector.register(proc.stdout,selectors.EVENT_READ);data=bytearray()
        def wait(marker,timeout=300):
            end=time.monotonic()+timeout
            while time.monotonic()<end:
                for key,_ in selector.select(.1):data.extend(key.fileobj.read1(65536))
                text=re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]','',data.decode(errors='replace')).replace('\r','')
                if marker in text:return text
                assert proc.poll() is None,text[-2000:]
            raise AssertionError('timeout: '+text[-2000:])
        def send(text):proc.stdin.write(text.encode());proc.stdin.flush()
        try:
            text=wait('Press 1 or 2, then Enter')
            assert 'FortressOS installer — choose an NVMe disk' in text,text[-2000:]
            assert 'Installer inventory unavailable' not in text
            print(f'{firmware}: Installer entry reached; checking disk selection and payload...',flush=True)
            send('1\n');text=wait('Choose disk 1, Escape goes back: ')
            assert 'No disk is preselected.' in text
            send('1\n');wait('Installer files checked. Your disk has not been changed.',600)
            wait('Computer name [')
            assert sha(target)==original,'dry-run changed the target'
            print(f'PASS {firmware}: menu choice 2 launches full installer, real NVMe selection and complete payload dry-run; target unchanged',flush=True)
        finally:
            proc.terminate()
            try:proc.wait(timeout=5)
            except subprocess.TimeoutExpired:proc.kill();proc.wait()
            selector.close()

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--image',type=Path,default=ROOT/'bin/fortress.img')
    args=parser.parse_args();image=args.image.resolve();before=sha(image);verify(image)
    for firmware in ('bios','uefi'):test(image,firmware)
    assert sha(image)==before
    print('PASS original raw image unchanged; no installation or physical-write acceptance claimed')
