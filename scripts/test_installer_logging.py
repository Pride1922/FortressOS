"""Install/boot plus USB log success/cancel/failure, regular-file fixtures only."""
import argparse
import json
from pathlib import Path
import shutil
import tempfile
import test_installer as gate


def extract(work,firmware):
    disk=work/('source.img' if firmware=='bios' else 'log-source.img')
    volume=work/'log-volume.img'
    with disk.open('rb') as stream:stream.seek(514*1024**2);volume.write_bytes(stream.read(8*1024**2))
    gate.fixture.run(['fsck.fat','-n',volume],work/'log-audit.txt')
    path=work/'fortress-install.log';gate.fixture.run(['mcopy','-i',volume,'::fortress-install.log',path],work/'log-audit.txt')
    record=path.read_text();assert '$5$' not in record and 'eight888' not in record and 'recover8' not in record
    return record


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--media',type=Path,required=True)
    parser.add_argument('--firmware',choices=('bios','uefi','all'),default='all');args=parser.parse_args()
    media=args.media.resolve();assert media.is_relative_to(gate.ROOT/'build/root-phase4')
    out=Path(tempfile.mkdtemp(prefix='log-guest-',dir=gate.ROOT/'build/root-phase4'))
    manifest={'result':'FAIL','media':str(media),'scope':'BIOS xHCI source; UEFI EHCI boot with xHCI source clone; regular-file install/boot/log audits','cases':[]}
    options={'source':'usb','disposable_gate':False,'media_stem':'fortress-installer-multi','username':'fabio',
             'operator':'eight888','recovery':'recover8','usb_logging':True,'ui_capture':True}
    def verify_root_log(boot,password,send,wait,command,prompt,transcript):
        command('cat /var/log/fortress-install.log','result=installation complete')
        return password
    gate.root_gate.SESSION_HOOK=verify_root_log
    try:
        with tempfile.TemporaryDirectory(prefix='fortress-log-guest-') as tmp:
            native=Path(tmp)
            try:
                for firmware in (('bios','uefi') if args.firmware=='all' else (args.firmware,)):
                    work=native/firmware;work.mkdir()
                    disk=gate.install(work,media,firmware,384,manifest,**options)
                    record=extract(work,firmware);assert 'result=installation complete' in record and 'stage=6 Verifying installed files' in record
                    serial=(work/'installer-serial.log').read_text();assert 'USB log could not be saved' not in serial
                    gate.audit(disk,work,media/'iso/boot/fortress.elf','eight888','recover8',manifest,username='fabio')
                    rootlog=work/'root-install.log';gate.fixture.run(['debugfs','-R',f'dump /var/log/fortress-install.log {rootlog}',work/'installed-root.img'],work/'log-audit.txt')
                    assert 'result=installation complete' in rootlog.read_text() and '$5$' not in rootlog.read_text()
                    gate.root_gate.guest(work,disk,firmware,1,0,'eight888','recover8',manifest,accel=gate.ACCEL,username='fabio')
                    gate.root_gate.guest(work,disk,firmware,1,1,'eight888','recover8',manifest,accel=gate.ACCEL,username='fabio')
                    gate.root_gate.guest(work,disk,firmware,1,2,'eight888','recover8',manifest,accel=gate.ACCEL,username='fabio')
                    rejected=work/'missing-root.img';shutil.copyfile(disk,rejected)
                    gate.root_gate.corrupt(rejected,'missing',work,lambda cmd:gate.fixture.run(cmd,work/'recovery-fixture.log'))
                    recovery_hash=gate.fixture.digest(rejected)
                    gate.root_gate.guest(work,rejected,firmware,1,'logging-recovery','eight888','recover8',manifest,recovery=True,accel=gate.ACCEL,username='fabio')
                    assert gate.fixture.digest(rejected)==recovery_hash
                    readonly=native/(firmware+'-readonly');readonly.mkdir()
                    ro_options={**options,'usb_logging':False}
                    gate.install(readonly,media,firmware,384,manifest,**ro_options)
                    assert 'USB log could not be saved' in (readonly/'installer-serial.log').read_text()
                    for case in ('cancel','timeout'):
                        negative=native/(firmware+'-'+case);negative.mkdir()
                        gate.install(negative,media,firmware,384,manifest,confirmation_cancel=case=='cancel',reject='timeout' if case=='timeout' else None,**options)
                        record=extract(negative,firmware)
                        assert ('Installation cancelled' if case=='cancel' else 'Installation failed: stage=2') in record
                    print('PASS install/boot/laptop/USB logs and cancel/failure',firmware,flush=True)
            finally:shutil.copytree(native,out,dirs_exist_ok=True,ignore=shutil.ignore_patterns('*.sock'))
        manifest['result']='PASS';print(out,flush=True)
    except BaseException as error:manifest['error']=type(error).__name__+': '+str(error);raise
    finally:
        gate.root_gate.SESSION_HOOK=None
        (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')


if __name__=='__main__':main()
