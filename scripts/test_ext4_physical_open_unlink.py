"""Disposable BIOS/UEFI durable-commit stop and independent recovery audit."""
import json
import selectors
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
import test_ext4_physical_image as physical
import test_ext4_journal_mount as guest
import test_ext4_integration as integration
from audit_ext4_open_unlink import verify_pending,verify_reclaimed,command
from test_net_pci import VARS
from test_nmi_transitions import REPO

def main():
    workspace=Path(sys.argv[1]).resolve()
    root=REPO/'.codex-remote-attachments/ext4-phase9'
    assert workspace.is_relative_to(root.resolve())
    manifest=Path(sys.argv[2]).resolve() if len(sys.argv)>2 else workspace/'physical-artifact/manifest.json'
    assert manifest.is_relative_to(root.resolve())
    info=json.loads(manifest.read_text())
    out=Path(tempfile.mkdtemp(prefix='physical-open-unlink-pause-',dir=root))
    rows=[];errors=[];original=guest.command;guest.command=physical.command
    try:
        for mode in ('bios','uefi'):
            disk=out/f'{mode}.img';subprocess.run(['cp','--sparse=always',info['image'],str(disk)],check=True)
            for name in ('fortress.elf','initramfs.tar'):
                subprocess.run(['mcopy','-o','-i',str(disk)+'@@1048576',str(workspace/'bin'/name),'::boot/'+name],check=True)
            physical.put_config(disk,out/f'{mode}.conf',physical.config(info['data_partuuid'],'cut-open-unlink'))
            variables=out/f'{mode}-vars.fd'
            if mode=='uefi':shutil.copyfile(VARS,variables)
            iso=out/'fixture.iso'
            if not iso.exists():shutil.copyfile(workspace/'bin/fortress.iso',iso)
            cmd=physical.command(mode,disk,variables,1,iso)
            (out/f'{mode}-pause-argv.json').write_text(json.dumps(cmd,indent=2)+'\n')
            transcript=bytearray();proc=None;selector=selectors.DefaultSelector()
            with (out/f'{mode}-pause.stderr.log').open('wb') as stderr:
                try:
                    proc=subprocess.Popen(cmd,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=stderr)
                    selector.register(proc.stdout,selectors.EVENT_READ)
                    deadline=time.monotonic()+180
                    marker=b'[EXT4 CUT] OPEN UNLINK DURABLE; REFERENCE RETAINED'
                    while marker not in transcript:
                        assert time.monotonic()<deadline,'pause deadline'
                        assert proc.poll() is None,'guest exited before pause'
                        for key,_ in selector.select(1):
                            transcript.extend(key.fileobj.read1(65536))
                    proc.kill();proc.wait(timeout=10)
                finally:
                    if proc and proc.poll() is None:proc.kill();proc.wait(timeout=10)
                    if proc:transcript.extend(proc.stdout.read())
                    selector.close()
                    (out/f'{mode}-pause.log').write_bytes(transcript)
            pending=out/f'{mode}-pending.ext4';physical.partition(disk,pending)
            baseline=Path(info['data_source'])
            ino=verify_pending(pending,baseline,out)
            recovered=out/f'{mode}-linux-recovered.ext4';shutil.copyfile(pending,recovered)
            code,log=command(['e2fsck','-fy',str(recovered)]);assert code in (0,1),log
            (out/f'{mode}-linux-recovery.log').write_text(log)
            verify_reclaimed(recovered,baseline,out)
            physical.put_config(disk,out/f'{mode}.conf',physical.config(info['data_partuuid'],'verify'))
            guest.boot(mode,disk,variables,2,iso,out,mode,removed=('ring-later.bin',))
            view=out/f'{mode}-view.img';physical.partition(disk,out/f'{mode}-source.ext4');physical.gpt(out/f'{mode}-source.ext4',view)
            guest.audit(view,out,mode,2,removed=('ring-later.bin',));integration.SMP=4;integration.audit(view,out,mode,2,base=False)
            final=out/f'{mode}-boot2.ext4'
            verify_reclaimed(final,baseline,out)
            # A second boot also exercises fixture allocation/reuse after cleanup.
            guest.boot(mode,disk,variables,2,iso,out,mode+'-repeat',removed=('ring-later.bin',))
            view=out/f'{mode}-repeat-view.img';physical.partition(disk,out/f'{mode}-repeat-source.ext4');physical.gpt(out/f'{mode}-repeat-source.ext4',view)
            guest.audit(view,out,mode+'-repeat',2,removed=('ring-later.bin',));integration.audit(view,out,mode+'-repeat',2,base=False)
            verify_reclaimed(out/f'{mode}-repeat-boot2.ext4',baseline,out)
            rows.append({'mode':mode,'pause':True,'durable_open_orphan':ino,'linux_cleanup':True,'fortress_cleanup':True,'repeat_reuse':True})
            print(f'PASS {mode}: durable open orphan, Linux/Fortress reclamation, repeat/reuse',flush=True)
    except Exception as error:errors.append(repr(error));raise
    finally:
        guest.command=original
        (out/'manifest.json').write_text(json.dumps({'cases':rows,'errors':errors,'workspace':str(workspace),'physical_writes':False},indent=2)+'\n')
        print(f'Evidence: {out}',flush=True)

if __name__=='__main__':main()
