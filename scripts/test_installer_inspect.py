#!/usr/bin/env python3
"""Actual identity-only inspector, disposable regular files, no physical writes."""
import hashlib,json,selectors,shutil,socket,stat,subprocess,tempfile,time,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];iso=Path(sys.argv[1]).resolve()
assert iso.is_relative_to(ROOT) and not iso.is_symlink() and stat.S_ISREG(iso.stat().st_mode)
def digest(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
out=Path(tempfile.mkdtemp(prefix='inspect-',dir=ROOT/'build/root-phase4'))
manifest={'result':'FAIL','scope':'identity-only BIOS/UEFI probe, writable disposable NVMe unchanged','iso_sha256':digest(iso),'cases':[]}
try:
    for firmware in ('bios','uefi'):
        retained=out/firmware;retained.mkdir()
        with tempfile.TemporaryDirectory(prefix='fortress-inspect-') as tmp:
            native=Path(tmp);disk=native/'nvme.img'
            with disk.open('xb') as f:f.truncate(384*1024**2);f.seek(64*512);f.write(b'INSPECTION MUST PRESERVE THIS DISK')
            before=digest(disk)
            argv=['qemu-system-x86_64','-M','q35','-m','2G','-accel','kvm','-cpu','host','-smp','1','-display','none','-monitor','none','-serial','stdio','-net','none','-no-reboot','-boot','d','-cdrom',str(iso),'-drive',f'file={disk},if=none,id=target,format=raw','-device','nvme,drive=target,serial=SPARE-INSPECT-TEST','-qmp',f'unix:{native}/qmp.sock,server=on,wait=off']
            if firmware=='uefi':
                variables=native/'vars.fd';shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
                argv+=['-drive','if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd','-drive',f'if=pflash,format=raw,unit=1,file={variables}']
            assert not any('/dev/' in a for a in argv)
            (retained/'argv.json').write_text(json.dumps(argv,indent=2)+'\n')
            transcript=bytearray()
            with (retained/'stderr.log').open('wb') as err:
                process=subprocess.Popen(argv,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=err)
                selector=selectors.DefaultSelector();selector.register(process.stdout,selectors.EVENT_READ)
                try:
                    deadline=time.monotonic()+180
                    while True:
                        for key,_ in selector.select(.1):
                            data=key.fileobj.read1(65536)
                            if data:transcript.extend(data)
                        (retained/'serial.log').write_bytes(transcript)
                        text=transcript.decode(errors='replace')
                        assert not any(s in text for s in ('KERNEL PANIC','[FATAL]','[PROCESS FAULT]')),text[-5000:]
                        if 'Photograph this screen' in text:break
                        assert process.poll() is None and time.monotonic()<deadline,text[-5000:]
                    assert 'Actual serial: SPARE-INSPECT-TEST' in text and 'Capacity MiB: 384' in text and 'Logical sector bytes: 512' in text
                    assert 'Installation is disabled in this image.' in text and 'No installation or disk-sector operations are available.' in text
                    assert '[BLOCK] Registered' not in text and 'Installer disposable backend ready' not in text
                    connection=socket.socket(socket.AF_UNIX);connection.connect(str(native/'qmp.sock'));stream=connection.makefile('rwb');stream.readline()
                    for cmd in ({'execute':'qmp_capabilities'},{'execute':'screendump','arguments':{'filename':str(native/'interface.png'),'format':'png'}}):
                        stream.write(json.dumps(cmd).encode()+b'\n');stream.flush()
                        while True:
                            reply=json.loads(stream.readline());assert 'error' not in reply,reply
                            if 'return' in reply:break
                    stream.close();connection.close();shutil.copyfile(native/'interface.png',retained/'interface.png')
                finally:
                    if process.poll() is None:
                        process.terminate()
                        try:process.wait(timeout=5)
                        except subprocess.TimeoutExpired:process.kill();process.wait(timeout=5)
                    transcript.extend(process.stdout.read());selector.close();(retained/'serial.log').write_bytes(transcript)
            after=digest(disk);assert after==before
            manifest['cases'].append({'firmware':firmware,'before':before,'after':after,'result':'PASS'})
            print('PASS identity-only inspection',firmware,flush=True)
    assert digest(iso)==manifest['iso_sha256'];manifest['result']='PASS';print(out,flush=True)
except BaseException as error:manifest['error']=type(error).__name__+': '+str(error);raise
finally:(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
