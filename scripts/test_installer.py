#!/usr/bin/env python3
"""Text installer and independent boot/recovery on disposable regular files.
All QEMU images are preflighted; no host raw devices or physical writes.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import selectors
import shutil
import socket
import stat
import subprocess
import tempfile
import time
import test_root_phase0 as fixture
import test_root_phase2 as root_gate
from install_audit import audit

ROOT=fixture.REPO
OPERATOR='phase4-operator-passphrase'
RECOVERY='phase4-independent-recovery'
# TCG copying 330 MiB through polled NVMe exceeded 20 min; KVM is opt-in for the install step only.
ACCEL=os.environ.get('FORTRESS_INSTALL_ACCEL','tcg')
assert ACCEL in ('tcg','kvm')

def immutable_digest(disk):
    digest=hashlib.sha256()
    with disk.open('rb') as stream:
        for start,length in ((0,135168*512),(659456*512,disk.stat().st_size-659456*512)):
            stream.seek(start)
            while length:
                data=stream.read(min(1024**2,length));assert data;digest.update(data);length-=len(data)
    return digest.hexdigest()


def install(out,media,firmware,capacity,manifest,exercise_inputs=False,reject=None,source='iso',seed=None,operator=OPERATOR,recovery=RECOVERY,media_stem='fortress-installer',serial='INSTALLPHASE4',disposable_gate=True,username="operator",ui_capture=False):
    disk=out/'target.img'
    with disk.open('xb') as stream:stream.truncate(capacity*1024**2)
    if seed:shutil.copyfile(seed,disk)
    assert media_stem in ('fortress-installer','fortress-installer-bound') and re.fullmatch(r'[A-Za-z0-9-]{1,20}',serial)
    iso=media/(media_stem+'.iso');assert iso.resolve().is_relative_to((ROOT/'build/root-phase4').resolve())
    assert all(stat.S_ISREG(p.stat().st_mode) and not p.is_symlink() for p in (disk,iso))
    argv=['qemu-system-x86_64','-M','q35','-m','2G','-accel',ACCEL,'-cpu','host' if ACCEL=='kvm' else 'max','-smp','1',
          '-display','none','-monitor','none','-no-reboot','-net','none','-serial','stdio',
          '-drive',f'file={disk},if=none,id=target,format=raw',
          '-device','nvme,drive=target,serial='+serial]
    if disposable_gate:argv+=['-fw_cfg','name=opt/fortress/installer_disposable,string=1']
    if source=='iso':argv+=['-boot','d','-cdrom',str(iso)]
    else:
        raw=out/'source.img';shutil.copyfile(media/(media_stem+'.img'),raw)
        if source=='usb':
            # OVMF's QEMU xHCI preboot path stalls before Limine/kernel entry.
            # The EHCI firmware path boots the same raw bytes and provenance.
            usb_controller='ich9-usb-ehci1,id=xhci' if firmware=='uefi' else 'qemu-xhci,id=xhci'
            argv+=['-drive',f'file={raw},if=none,id=source,format=raw,readonly=on',
                   '-device',usb_controller,'-device','usb-storage,bus=xhci.0,drive=source,bootindex=1']
        else:
            assert source=='nvme'
            argv[argv.index('-drive')+1]=f'file={raw},if=none,id=target,format=raw,readonly=on'
            argv[argv.index('-device')+1]+=',bootindex=1'
    if reject=='no-gate':
        index=argv.index('-fw_cfg');del argv[index:index+2]
    if reject=='geometry':argv[argv.index('-device')+1]+=',logical_block_size=4096,physical_block_size=4096'
    if reject=='timeout' or ui_capture:argv+=['-qmp',f'unix:{out}/qmp.sock,server=on,wait=off']
    if reject=='two-disks':
        excluded=out/'excluded.img'
        with excluded.open('xb') as stream:stream.truncate(384*1024**2)
        argv+=['-drive',f'file={excluded},if=none,id=excluded,format=raw','-device','nvme,drive=excluded,serial=EXCLUDED']
    if firmware=='uefi':
        variables=out/'installer-vars.fd';shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
        argv+=['-drive','if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
               '-drive',f'if=pflash,format=raw,unit=1,file={variables}']
    assert argv.count('-device')==1+(2 if source=='usb' else 0)+(reject=='two-disks')
    assert argv.count('-drive')==1+(2 if firmware=='uefi' else 0)+(source=='usb')+(reject=='two-disks')
    assert disk.resolve().parent==out.resolve() and not any('/dev/' in arg for arg in argv)
    (out/'installer-argv.json').write_text(json.dumps(argv,indent=2)+'\n')
    before=fixture.digest(iso);transcript=bytearray()
    untouched={p:fixture.digest(p) for p in out.glob('*.img')}
    with (out/'installer-stderr.log').open('wb') as err:
        proc=subprocess.Popen(argv,cwd=ROOT,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=err)
        selector=selectors.DefaultSelector();selector.register(proc.stdout,selectors.EVENT_READ)
        def text():return re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]','',transcript.decode(errors='replace')).replace('\r','')
        def wait(marker,start=0,timeout=600):
            deadline=time.monotonic()+timeout
            while time.monotonic()<deadline:
                for key,_ in selector.select(.1):
                    data=key.fileobj.read1(65536)
                    if data:transcript.extend(data)
                (out/'installer-serial.log').write_bytes(transcript)
                assert not any(s in text() for s in ('KERNEL PANIC','[PROCESS FAULT]','[FATAL]')),text()[-6000:]
                if reject is None:assert 'No admitted ' not in text(),text()[-6000:]
                assert 'Payload validation failed' not in text() and (reject=='timeout' or 'Installation failed at stage' not in text()),text()[-6000:]
                if marker in text()[start:]:return
                assert proc.poll() is None,text()[-6000:]
            raise TimeoutError(text()[-6000:])
        def send(value):proc.stdin.write((value+'\n').encode());proc.stdin.flush()
        try:
            if reject in ('no-gate','geometry','two-disks','source-parent','identity'):
                wait('Use live rescue or power off.')
                assert all(fixture.digest(p)==value for p,value in untouched.items())
                manifest['cases'].append({'installer_rejection':reject,'firmware':firmware,'result':'PASS'})
                return None
            selection='Choose disk 1, or cancel with Escape: '
            login='Choose login password (8..128 characters): '
            recovery_prompt='Choose independent recovery passphrase (8..128 characters): '
            confirmation='Type YES to erase this disk and install, or NO to cancel: '
            wait(selection)
            if exercise_inputs:
                baseline=fixture.digest(disk)
                for profile in ('escape','blank-selection','empty-password','mismatch','empty-recovery','same-secret','wrong-confirmation'):
                    start=len(text())
                    if profile=='escape':
                        proc.stdin.write(b'\x1b');proc.stdin.flush()
                    elif profile=='blank-selection':
                        send('');wait(selection,start)
                        proc.stdin.write(b'\x1b');proc.stdin.flush()
                    else:
                        send('1');wait('Choose username: ',start);send(username);wait(login,start)
                        if profile=='empty-password':
                            mark=len(text());send('');wait(login,mark)
                        else:
                            send(operator);wait('Confirm passphrase: ',start)
                            mark=len(text());send('mismatched-passphrase' if profile=='mismatch' else operator)
                            if profile=='mismatch':wait(login,mark)
                            else:
                                wait(recovery_prompt,mark)
                                if profile=='empty-recovery':
                                    mark=len(text());send('');wait(recovery_prompt,mark)
                                else:
                                    recovery_choice=operator if profile=='same-secret' else recovery
                                    mark=len(text());send(recovery_choice);wait('Confirm passphrase: ',mark)
                                    mark=len(text());send(recovery_choice)
                                    if profile=='same-secret':wait(recovery_prompt,mark)
                                    else:
                                        wait(confirmation,mark);mark=len(text());send('wrong');wait(confirmation,mark)
                        assert fixture.digest(disk)==baseline,profile+' wrote target'
                        proc.stdin.write(b'\x1b');proc.stdin.flush()
                    wait('Installer stopped; no success reported.',start);wait(selection,start)
                    assert fixture.digest(disk)==baseline,profile+' wrote target'
                    manifest['cases'].append({'input_retry_or_cancel':profile,'firmware':firmware,'result':'PASS'})
            target_before=fixture.digest(disk);start=len(text());send('1')
            wait('Choose username: ',start);send(username);wait(login,start)
            assert fixture.digest(disk)==target_before,'preview/dry-run wrote target'
            send(operator);wait('Confirm passphrase: ',start);start=len(text());send(operator)
            wait(recovery_prompt,start)
            send(recovery);wait('Confirm passphrase: ',start);start=len(text());send(recovery)
            wait(confirmation,start)
            assert fixture.digest(disk)==target_before,'credentials/confirmation wrote target'
            if ui_capture:
                connection=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);connection.settimeout(5);connection.connect(str(out/'qmp.sock'))
                channel=connection.makefile('rwb');json.loads(channel.readline())
                for command in ({'execute':'qmp_capabilities'},{'execute':'screendump','arguments':{'filename':str(out/'confirmation.png'),'format':'png'}}):
                    channel.write(json.dumps(command).encode()+b'\n');channel.flush()
                    while True:
                        response=json.loads(channel.readline());assert 'error' not in response,response
                        if 'return' in response:break
                channel.close();connection.close()
            if reject=='timeout':
                # QMP throttles only this disposable regular-file backend. This
                # exercises the real driver's monotonic timeout and quiescence.
                # https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html#command-block_set_io_throttle
                sock=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);sock.settimeout(5);sock.connect(str(out/'qmp.sock'))
                qmp=sock.makefile('rwb');json.loads(qmp.readline());history=[]
                for command in ({'execute':'qmp_capabilities'},{'execute':'block_set_io_throttle','arguments':
                    {'device':'target','bps':1,'bps_rd':0,'bps_wr':0,'iops':0,'iops_rd':0,'iops_wr':0}}):
                    qmp.write(json.dumps(command).encode()+b'\n');qmp.flush()
                    while True:
                        response=json.loads(qmp.readline());history.append(response)
                        assert 'error' not in response,response
                        if 'return' in response:break
                (out/'qmp.json').write_text(json.dumps(history,indent=2)+'\n');qmp.close();sock.close()
            started=time.monotonic();send('YES')
            if reject=='timeout':
                wait('Installer stopped; no success reported.',start,60)
                elapsed=time.monotonic()-started
                assert elapsed<45 and 'NVMe I/O command timeout!' in text() and 'Installation failed at stage 2' in text()
                assert 'Installation complete.' not in text()
                assert all(fixture.digest(p)==value for p,value in untouched.items())
                manifest['cases'].append({'installer_fault':'real NVMe deadline','firmware':firmware,'elapsed_seconds':elapsed,'result':'PASS'})
                return None
            if reject=='small':
                wait('Installer stopped; no success reported.',start)
                assert all(fixture.digest(p)==value for p,value in untouched.items())
                manifest['cases'].append({'installer_rejection':reject,'firmware':firmware,'result':'PASS'})
                return None
            wait('Installation complete.',start,5400)
            assert 'Installation failed at stage' not in text()
            assert all(secret.encode() not in transcript for secret in (operator,recovery))
            manifest['cases'].append({'install':firmware,'capacity_mib':capacity,'zero_write_preconfirmation':True,'no_echo':True,'result':'PASS'})
            print('PASS text install',firmware,capacity,flush=True)
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:proc.wait(timeout=5)
                except subprocess.TimeoutExpired:proc.kill();proc.wait(timeout=5)
            transcript.extend(proc.stdout.read());selector.close();(out/'installer-serial.log').write_bytes(transcript)
            assert fixture.digest(iso)==before
            if source!='iso':assert fixture.digest(raw)==untouched[raw]
    return disk


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--media',type=Path,required=True)
    parser.add_argument('--firmware',choices=('bios','uefi','all'),default='all');parser.add_argument('--quick',action='store_true')
    args=parser.parse_args();parent=ROOT/'build/root-phase4';parent.mkdir(parents=True,exist_ok=True)
    out=Path(tempfile.mkdtemp(prefix='guest-',dir=parent))
    sources={str(p.relative_to(ROOT)):fixture.digest(p) for base in ('src','tests','scripts','user')
             for p in (ROOT/base).rglob('*') if p.is_file() and p.suffix in ('.c','.h','.inc','.py','.asm')}
    normal={str(p):fixture.digest(p) for p in (ROOT/'bin/fortress.iso',ROOT/'bin/fortress.img') if p.exists()}
    manifest={'result':'FAIL','scope':'disposable text install/source-removal/boot/recovery guest gate',
              'media':str(args.media),'sources':sources,'normal_artifacts':normal,'cases':[],'commands':fixture.COMMANDS}
    try:
        for firmware in (('bios','uefi') if args.firmware=='all' else (args.firmware,)):
            retained=out/firmware;retained.mkdir()
            with tempfile.TemporaryDirectory(prefix='fortress-installer-'+firmware+'-') as tmp:
                native=Path(tmp)
                try:
                    disk=install(native,args.media,firmware,384,manifest,exercise_inputs=not args.quick)
                    audit(disk,native,args.media/'iso/boot/fortress.elf',OPERATOR,RECOVERY,manifest)
                    # Source is omitted on every boot; fresh OVMF vars are used.
                    for boot in range(3):root_gate.guest(native,disk,firmware,1,boot,OPERATOR,RECOVERY,manifest,accel=ACCEL)
                    if not args.quick:
                        for cpus in (4,8):root_gate.guest(native,disk,firmware,cpus,0,OPERATOR,RECOVERY,manifest,accel=ACCEL)
                    for kind in (('missing',) if args.quick else ('missing','manifest','readiness','unsupported','duplicate','degraded','conflict','login')):
                        rejected=native/(kind+'.img');shutil.copyfile(disk,rejected)
                        root_gate.corrupt(rejected,kind,native,lambda cmd:fixture.run(cmd,native/'mutations.log'))
                        before=fixture.digest(rejected);immutable=immutable_digest(rejected)
                        root_gate.guest(native,rejected,firmware,1,kind,OPERATOR,RECOVERY,manifest,recovery=True,accel=ACCEL)
                        if kind not in ('manifest','login'):
                            assert fixture.digest(rejected)==before,'pre-mount recovery modified rejected target'
                        else:
                            # Tree validation follows legitimate detached replay;
                            # its freeze may update root journal metadata. Boot
                            # artifacts stay byte-identical and canonical account
                            # bytes are never replaced by recovery authentication.
                            audited=native/(kind+'-rejected-root.img');root_gate.extract_root(rejected,audited)
                            fixture.run(['e2fsck','-fn',str(audited)],native/(kind+'-recovery-fsck.log'))
                            assert immutable_digest(rejected)==immutable,'recovery modified boot artifacts'
                            for name in ('passwd','group','shadow'):
                                hashes=[]
                                for suffix,image in (('before',native/(kind+'-root-input.img')),('after',audited)):
                                    dumped=native/(kind+'-'+suffix+'-'+name)
                                    fixture.run(['debugfs','-R',f'dump /etc/{name} {dumped}',str(image)],native/(kind+'-recovery-fsck.log'))
                                    hashes.append(fixture.digest(dumped))
                                assert hashes[0]==hashes[1],'recovery changed '+name
                    if not args.quick:
                        for kind in ('no-gate','geometry','two-disks','small','source-parent','timeout'):
                            case=native/('reject-'+kind);case.mkdir()
                            install(case,args.media,firmware,320 if kind=='small' else 384,manifest,reject=kind,source='nvme' if kind=='source-parent' else 'iso')
                        # Raw USB source is immutable and omitted from the installed boot.
                        case=native/'raw-usb';case.mkdir()
                        raw_disk=install(case,args.media,firmware,384,manifest,source='usb')
                        audit(raw_disk,case,args.media/'iso/boot/fortress.elf',OPERATOR,RECOVERY,manifest)
                        root_gate.guest(case,raw_disk,firmware,1,0,OPERATOR,RECOVERY,manifest,accel=ACCEL)
                        case=native/'capacity-512';case.mkdir()
                        second_operator='second-operator-passphrase';second_recovery='second-independent-recovery'
                        larger=install(case,args.media,firmware,512,manifest,operator=second_operator,recovery=second_recovery)
                        audit(larger,case,args.media/'iso/boot/fortress.elf',second_operator,second_recovery,manifest)
                        root_gate.guest(case,larger,firmware,1,0,second_operator,second_recovery,manifest,accel=ACCEL)
                        rejected=case/'missing.img';shutil.copyfile(larger,rejected)
                        root_gate.corrupt(rejected,'missing',case,lambda cmd:fixture.run(cmd,case/'mutations.log'))
                        before=fixture.digest(rejected)
                        root_gate.guest(case,rejected,firmware,1,'cross-secret',second_operator,second_recovery,manifest,recovery=True,wrong_secret=RECOVERY,accel=ACCEL)
                        assert fixture.digest(rejected)==before
                finally:
                    shutil.copytree(native,retained,dirs_exist_ok=True,ignore=shutil.ignore_patterns('*.sock'))
        audits=[case for case in manifest['cases'] if 'independent_linux_audit' in case]
        for key in ('disk_guid','root_partuuid','install_id','operator_salt','recovery_salt'):
            assert len({case[key] for case in audits})==len(audits),'reused '+key
        assert all(fixture.digest(ROOT/p)==value for p,value in sources.items())
        assert all(fixture.digest(Path(p))==value for p,value in normal.items())
        manifest['result']='PASS';print(out,flush=True)
    except BaseException as error:
        manifest['error']=type(error).__name__+': '+str(error);raise
    finally:(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')


if __name__=='__main__':main()
