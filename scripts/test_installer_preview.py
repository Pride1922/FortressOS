#!/usr/bin/env python3
"""Boot the interface-only USB image with writable regular-file sentinels."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import selectors
import shutil
import socket
import stat
import subprocess
import tempfile
import time

ROOT=Path(__file__).resolve().parents[1]
def digest(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--media',type=Path,required=True)
    parser.add_argument('--iso',action='store_true');parser.add_argument('--hybrid',action='store_true');args=parser.parse_args()
    image=args.media/('fortress-installer-preview.iso' if args.hybrid else 'fortress-installer-preview.img')
    assert not image.is_symlink() and stat.S_ISREG(image.stat().st_mode)
    assert image.resolve().is_relative_to((ROOT/'build/root-phase4').resolve())
    assert b'opt/fortress/installer_disposable' not in (args.media/'iso/boot/fortress.elf').read_bytes()
    out=Path(tempfile.mkdtemp(prefix='preview-',dir=ROOT/'build/root-phase4'))
    manifest={'result':'FAIL','scope':'BIOS/UEFI '+('ISO' if args.iso else 'raw USB')+' interface preview; no disk changes','image_sha256':digest(image),'cases':[]}
    iso=args.media/'fortress-installer-preview.iso'
    if args.iso:
        assert not iso.is_symlink() and stat.S_ISREG(iso.stat().st_mode)
        manifest['iso_sha256']=digest(iso)
    try:
        for firmware in ('bios','uefi'):
            retained=out/firmware;retained.mkdir()
            with tempfile.TemporaryDirectory(prefix='fortress-preview-') as tmp:
                native=Path(tmp);disk=native/'sentinel.img';source=native/'source.img'
                with disk.open('xb') as stream:
                    stream.truncate(384*1024**2);stream.write(b'PREVIEW MUST PRESERVE THIS DISK\n')
                    stream.seek(383*1024**2);stream.write(bytes(range(256))*4096)
                shutil.copyfile(image,source)
                before={p.name:digest(p) for p in (disk,source)}
                # Use the established TCG firmware profile for UEFI acceptance.
                accel='tcg' if firmware=='uefi' else 'kvm'
                usb_controller='ich9-usb-ehci1,id=xhci' if firmware=='uefi' else 'qemu-xhci,id=xhci'
                argv=['qemu-system-x86_64','-M','q35','-m','2G','-accel',accel,'-cpu','max' if accel=='tcg' else 'host','-smp','1',
                    '-display','none','-monitor','none','-no-reboot','-net','none','-serial','stdio',
                    '-drive',f'file={disk},if=none,id=target,format=raw','-device','nvme,drive=target,serial=SENTINEL',
                    '-drive',f'file={source},if=none,id=source,format=raw,readonly=on',
                    '-device',usb_controller,'-device','usb-storage,bus=xhci.0,drive=source,bootindex=1',
                    '-qmp',f'unix:{native}/qmp.sock,server=on,wait=off']
                if firmware=='uefi':
                    variables=native/'vars.fd';shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
                    argv+=['-drive','if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
                        '-drive',f'if=pflash,format=raw,unit=1,file={variables}']
                if args.iso:
                    # Same kernel/bootstrap, optical hybrid transport instead of FAT USB.
                    index=argv.index('-drive',argv.index('-drive')+1)
                    del argv[index:index+6]
                    argv+=['-boot','d','-cdrom',str(args.media/'fortress-installer-preview.iso')]
                assert not any('/dev/' in arg for arg in argv)
                assert all(stat.S_ISREG(p.stat().st_mode) and not p.is_symlink() for p in (disk,source))
                (retained/'argv.json').write_text(json.dumps(argv,indent=2)+'\n')
                transcript=bytearray()
                with (retained/'stderr.log').open('wb') as err:
                    proc=subprocess.Popen(argv,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=err)
                    selector=selectors.DefaultSelector();selector.register(proc.stdout,selectors.EVENT_READ)
                    def text():return re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]','',transcript.decode(errors='replace')).replace('\r','')
                    def wait(marker,start=0):
                        deadline=time.monotonic()+600
                        while time.monotonic()<deadline:
                            for key,_ in selector.select(.1):
                                data=key.fileobj.read1(65536)
                                if data:transcript.extend(data)
                            (retained/'serial.log').write_bytes(transcript)
                            assert not any(s in text() for s in ('KERNEL PANIC','[PROCESS FAULT]','[FATAL]')),text()[-6000:]
                            if marker in text()[start:]:return
                            assert proc.poll() is None,text()[-6000:]
                        raise TimeoutError(text()[-6000:])
                    def send(value):proc.stdin.write((value+'\n').encode());proc.stdin.flush()
                    def screenshot(name):
                        connection=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);connection.settimeout(10);connection.connect(str(native/'qmp.sock'))
                        stream=connection.makefile('rwb');json.loads(stream.readline())
                        for command in ({'execute':'qmp_capabilities'},{'execute':'screendump','arguments':{'filename':str(native/name),'format':'png'}}):
                            stream.write(json.dumps(command).encode()+b'\n');stream.flush()
                            while True:
                                response=json.loads(stream.readline());assert 'error' not in response,response
                                if 'return' in response:break
                        stream.close();connection.close();shutil.copyfile(native/name,retained/name)
                    try:
                        wait('Choose disk 1, or cancel with Escape: ')
                        assert 'Synthetic preview target' in text() and 'Installation is disabled in this image.' in text()
                        screenshot('interface.png');send('1');wait('Choose username: ');send('demo');wait('Choose login password (8..128 characters): ')
                        send('demo-operator-passphrase');wait('Confirm passphrase: ');start=len(text());send('demo-operator-passphrase')
                        wait('Choose independent recovery passphrase (8..128 characters): ',start)
                        send('demo-recovery-passphrase');wait('Confirm passphrase: ',start);start=len(text());send('demo-recovery-passphrase')
                        wait('Type YES to erase this disk and install, or NO to cancel: ',start);screenshot('confirmation.png')
                        assert 'Username: demo' in text()[start:]
                        send('YES');wait('Preview complete. Installation remains disabled.',start)
                        assert 'Installation complete.' not in text() and 'Installing; keep power' not in text()
                        assert 'Installer disposable backend ready' not in text() and '[BLOCK] Registered' not in text()
                        assert 'demo-operator-passphrase' not in text() and 'demo-recovery-passphrase' not in text()
                    finally:
                        if proc.poll() is None:
                            proc.terminate()
                            try:proc.wait(timeout=5)
                            except subprocess.TimeoutExpired:proc.kill();proc.wait(timeout=5)
                        transcript.extend(proc.stdout.read());selector.close();(retained/'serial.log').write_bytes(transcript)
                after={p.name:digest(p) for p in (disk,source)};assert after==before
                manifest['cases'].append({'firmware':firmware,'before':before,'after':after,'complete_confirmation_without_install':True,'result':'PASS'})
                print('PASS preview',firmware,flush=True)
        assert digest(image)==manifest['image_sha256']
        if args.iso:assert digest(iso)==manifest['iso_sha256']
        manifest['result']='PASS';print(out,flush=True)
    except BaseException as error:manifest['error']=type(error).__name__+': '+str(error);raise
    finally:(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
if __name__=='__main__':main()
