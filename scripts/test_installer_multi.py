#!/usr/bin/env python3
"""Two NVMe selection/boot isolation, using disposable regular files only."""
import argparse
import json
from pathlib import Path
import shutil
import tempfile
import test_installer as gate


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--media',type=Path,required=True)
    args=parser.parse_args();media=args.media.resolve()
    assert media.is_relative_to(gate.ROOT/'build/root-phase4')
    producer=json.loads((media/'manifest.json').read_text())
    assert producer['result']=='PASS' and producer['profile'].startswith('explicit user-selected')
    out=Path(tempfile.mkdtemp(prefix='multi-',dir=gate.ROOT/'build/root-phase4'))
    sources={str(p.relative_to(gate.ROOT)):gate.fixture.digest(p) for base in ('src','tests','scripts','user')
             for p in (gate.ROOT/base).rglob('*') if p.is_file() and p.suffix in ('.c','.h','.py','.asm','.inc')}
    normal={str(p):gate.fixture.digest(p) for p in (gate.ROOT/'bin/fortress.iso',gate.ROOT/'bin/fortress.img') if p.exists()}
    artifacts={str(p):gate.fixture.digest(p) for p in (media/'fortress-installer-multi.iso',media/'fortress-installer-multi.img')}
    manifest={'result':'FAIL','scope':'two NVMe selection, non-target isolation, root discovery and recovery; regular files only',
              'media':str(media),'sources':sources,'normal_artifacts':normal,'media_hashes':artifacts,'cases':[]}
    options={'media_stem':'fortress-installer-multi','disposable_gate':False,'source':'usb','username':'fabio',
             'operator':'eight888','recovery':'recover8'}
    try:
        for firmware in ('bios','uefi'):
            retained=out/firmware;retained.mkdir()
            with tempfile.TemporaryDirectory(prefix='fortress-multi-') as tmp:
                native=Path(tmp)
                try:
                    cancel=native/'cancel';cancel.mkdir()
                    gate.install(cancel,media,firmware,384,manifest,multi_selection=2,cancel_only=True,**options)
                    for selected in (1,2):
                        work=native/('disk-'+str(selected));work.mkdir()
                        if selected==1:
                            seed=work/'existing-mbr.img'
                            with seed.open('xb') as stream:
                                stream.truncate(384*1024**2)
                                mbr=bytearray(512);mbr[510:]=b'\x55\xaa';mbr[450]=0x07
                                mbr[454:458]=(2048).to_bytes(4,'little');mbr[458:462]=(4096).to_bytes(4,'little')
                                stream.write(mbr)
                        else:
                            seed=native/'disk-1/target.img' # Valid unrelated GPT from the first installed target.
                        seed_hash=gate.fixture.digest(seed)
                        disk=gate.install(work,media,firmware,384,manifest,multi_selection=selected,ui_capture=True,seed=seed,**options)
                        assert gate.fixture.digest(seed)==seed_hash
                        serial=(work/'installer-serial.log').read_text()
                        assert 'Existing partitions:' in serial and ('Windows' if selected==1 else 'Linux') in serial
                        other=work/'excluded.img';before=gate.fixture.digest(other)
                        gate.audit(disk,work,media/'iso/boot/fortress.elf','eight888','recover8',manifest,username='fabio')
                        log=work/'installed-fortress-install.log'
                        gate.fixture.run(['debugfs','-R',f'dump /var/log/fortress-install.log {log}',work/'installed-root.img'],work/'independent-audit.log')
                        record=log.read_text();assert record.startswith('FORTRESS-INSTALL-LOG-1\n') and '\nuser=fabio\n' in record
                        assert 'eight888' not in record and 'recover8' not in record
                        for boot in range(3):
                            gate.root_gate.guest(work,disk,firmware,1,boot,'eight888','recover8',manifest,
                                accel=gate.ACCEL,username='fabio',extra=other,extra_first=selected==2)
                            assert gate.fixture.digest(other)==before
                        clone=work/'cloned-root.img';shutil.copyfile(disk,clone)
                        hashes={p:gate.fixture.digest(p) for p in (disk,clone)}
                        gate.root_gate.guest(work,disk,firmware,1,'cross-drive-duplicate','eight888','recover8',manifest,
                            recovery=True,extra=clone,accel=gate.ACCEL,username='fabio',extra_first=selected==2)
                        assert all(gate.fixture.digest(p)==h for p,h in hashes.items())
                        manifest['cases'].append({'selected_disk':selected,'firmware':firmware,'other_sha256':before,
                            'existing_layout':'MBR' if selected==1 else 'GPT',
                            'three_boots_with_both_drives':True,'duplicate_root_no_writes':True,'result':'PASS'})
                    source=native/'source-exclusion';source.mkdir()
                    disk=gate.install(source,media,firmware,384,manifest,multi_selection=2,source_clone=True,**options)
                    gate.audit(disk,source,media/'iso/boot/fortress.elf','eight888','recover8',manifest,username='fabio')
                    manifest['cases'].append({'installer_source_nvme_excluded':firmware,'result':'PASS'})
                    print('PASS two NVMe selection/boot/recovery',firmware,flush=True)
                finally:
                    shutil.copytree(native,retained,dirs_exist_ok=True,ignore=shutil.ignore_patterns('*.sock'))
        assert all(gate.fixture.digest(gate.ROOT/p)==h for p,h in sources.items())
        assert all(gate.fixture.digest(Path(p))==h for p,h in {**normal,**artifacts}.items())
        manifest['result']='PASS';print(out,flush=True)
    except BaseException as error:
        manifest['error']=type(error).__name__+': '+str(error);raise
    finally:
        (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')


if __name__=='__main__':main()
