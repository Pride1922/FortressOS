#!/usr/bin/env python3
"""Drive-bound policy on disposable files only; never physical acceptance."""
import argparse,json,shutil,tempfile
from pathlib import Path
import test_installer as gate

FIXTURE_DIGEST='a465886eab4c2d302aee9d774a864189a79e808d356b97485f9c993329629bc6'

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--media',type=Path,required=True)
    parser.add_argument('--accept-fixture',action='store_true')
    parser.add_argument('--final-ui-check',action='store_true',help='Exact release bytes: raw USB boot, physical banner, wrong drive unchanged')
    args=parser.parse_args();media=args.media.resolve()
    assert not (args.accept_fixture and args.final_ui_check)
    assert media.is_relative_to(gate.ROOT/'build/root-phase4')
    producer=json.loads((media/'manifest.json').read_text());assert producer['result']=='PASS'
    if args.accept_fixture:assert producer['bound_inspector_digest']==FIXTURE_DIGEST
    else:assert producer['bound_inspector_digest']!=FIXTURE_DIGEST
    out=Path(tempfile.mkdtemp(prefix='bound-',dir=gate.ROOT/'build/root-phase4'))
    sources={str(p.relative_to(gate.ROOT)):gate.fixture.digest(p) for base in ('src','tests','scripts','user')
             for p in (gate.ROOT/base).rglob('*') if p.is_file() and p.suffix in ('.c','.h','.py','.asm')}
    normal={str(p):gate.fixture.digest(p) for p in (gate.ROOT/'bin/fortress.iso',gate.ROOT/'bin/fortress.img')}
    artifacts={str(p):gate.fixture.digest(p) for p in (media/'fortress-installer-bound.iso',media/'fortress-installer-bound.img')}
    manifest={'result':'FAIL','scope':'bound profile virtual acceptance' if args.accept_fixture else 'real-bound image rejects virtual hardware',
              'media':str(media),'sources':sources,'normal_artifacts':normal,'media_hashes':artifacts,'cases':[]}
    kwargs={'media_stem':'fortress-installer-bound','serial':'SPARE-INSPECT-TEST','disposable_gate':False}
    try:
        for firmware in ('bios','uefi'):
            retained=out/firmware;retained.mkdir()
            with tempfile.TemporaryDirectory(prefix='fortress-bound-') as tmp:
                native=Path(tmp)
                try:
                    if args.final_ui_check:
                        gate.install(native,media,firmware,384,manifest,reject='identity',source='usb',**kwargs)
                        transcript=(native/'installer-serial.log').read_text(errors='replace')
                        assert 'FortressOS installer — identified spare NVMe only' in transcript
                        assert 'No admitted bound NVMe target.' in transcript
                        continue
                    if args.accept_fixture:
                        disk=gate.install(native,media,firmware,384,manifest,source='usb',exercise_inputs=True,username='fabio',operator='eight888',**kwargs)
                        gate.audit(disk,native,media/'iso/boot/fortress.elf','eight888',gate.RECOVERY,manifest,username='fabio')
                        for boot in range(3):
                            gate.root_gate.guest(native,disk,firmware,1,boot,'eight888',gate.RECOVERY,manifest,accel=gate.ACCEL,username='fabio')
                        rejected=native/'missing.img';shutil.copyfile(disk,rejected)
                        gate.root_gate.corrupt(rejected,'missing',native,lambda cmd:gate.fixture.run(cmd,native/'mutations.log'))
                        before=gate.fixture.digest(rejected)
                        gate.root_gate.guest(native,rejected,firmware,1,'missing','eight888',gate.RECOVERY,manifest,recovery=True,accel=gate.ACCEL,username='fabio')
                        assert gate.fixture.digest(rejected)==before
                    for source in ('iso','usb'):
                        case=native/('wrong-identity-'+source);case.mkdir()
                        bad=dict(kwargs);bad['serial']='OTHER-NVME'
                        gate.install(case,media,firmware,384,manifest,reject='identity',source=source,**bad)
                    for rejection in ('geometry','two-disks','source-parent'):
                        case=native/rejection;case.mkdir()
                        gate.install(case,media,firmware,384,manifest,reject=rejection,source='nvme' if rejection=='source-parent' else 'iso',**kwargs)
                    case=native/'different-capacity';case.mkdir()
                    gate.install(case,media,firmware,512,manifest,reject='identity',**kwargs)
                    print('PASS bound profile',firmware,flush=True)
                finally:shutil.copytree(native,retained,dirs_exist_ok=True,ignore=shutil.ignore_patterns('*.sock'))
        assert all(gate.fixture.digest(gate.ROOT/p)==h for p,h in sources.items())
        assert all(gate.fixture.digest(Path(p))==h for p,h in {**normal,**artifacts}.items())
        manifest['result']='PASS';print(out,flush=True)
    except BaseException as error:
        manifest['error']=type(error).__name__+': '+str(error);raise
    finally:(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')

if __name__=='__main__':main()
