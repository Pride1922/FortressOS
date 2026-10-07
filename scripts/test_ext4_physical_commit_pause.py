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
from test_jbd2_replay_host import oracle
from test_net_pci import VARS
from test_nmi_transitions import REPO

def main():
    workspace=Path(sys.argv[1]).resolve()
    root=REPO/'.codex-remote-attachments/ext4-phase9'
    assert workspace.is_relative_to(root.resolve())
    manifest=Path(sys.argv[2]).resolve() if len(sys.argv)>2 else workspace/'physical-artifact/manifest.json'
    assert manifest.is_relative_to(root.resolve())
    info=json.loads(manifest.read_text())
    out=Path(tempfile.mkdtemp(prefix='physical-commit-pause-',dir=root))
    rows=[];errors=[];original=guest.command;guest.command=physical.command
    try:
        for mode in ('bios','uefi'):
            disk=out/f'{mode}.img';subprocess.run(['cp','--sparse=always',info['image'],str(disk)],check=True)
            for name in ('fortress.elf','initramfs.tar'):
                subprocess.run(['mcopy','-o','-i',str(disk)+'@@1048576',str(workspace/'bin'/name),'::boot/'+name],check=True)
            physical.put_config(disk,out/f'{mode}.conf',physical.config(info['data_partuuid'],'cut-commit'))
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
                    marker=b'[EXT4 CUT] DURABLE COMMIT BEFORE CHECKPOINT;'
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
            before=subprocess.run(['debugfs','-R','stat /cut-commit.txt',str(pending)],capture_output=True,text=True,check=True)
            assert 'File not found' in before.stdout+before.stderr,'metadata reached home before pause'
            recovered=out/f'{mode}-linux-recovered.ext4'
            oracle(pending,recovered,out/f'{mode}-linux-recovery.log')
            stat=subprocess.run(['debugfs','-R','stat /cut-commit.txt',str(recovered)],capture_output=True,text=True,check=True)
            assert 'Type: regular' in stat.stdout and 'Size: 0' in stat.stdout,stat.stdout
            (out/f'{mode}-linux-cut-stat.log').write_text(stat.stdout+stat.stderr)
            physical.put_config(disk,out/f'{mode}.conf',physical.config(info['data_partuuid'],'verify'))
            guest.boot(mode,disk,variables,2,iso,out,mode)
            physical.audit(disk,out,mode,2)
            final=out/f'{mode}-boot2.ext4'
            stat=subprocess.run(['debugfs','-R','stat /cut-commit.txt',str(final)],capture_output=True,text=True,check=True)
            assert 'Type: regular' in stat.stdout and 'Size: 0' in stat.stdout,stat.stdout
            (out/f'{mode}-fortress-cut-stat.log').write_text(stat.stdout+stat.stderr)
            rows.append({'mode':mode,'pause':True,'home_file_absent_before_recovery':True,'linux_recovery':True,'fortress_recovery':True})
            print(f'PASS {mode}: durable pause, home exclusion, Linux and Fortress recovery',flush=True)
    except Exception as error:errors.append(repr(error));raise
    finally:
        guest.command=original
        (out/'manifest.json').write_text(json.dumps({'cases':rows,'errors':errors,'workspace':str(workspace),'physical_writes':False},indent=2)+'\n')
        print(f'Evidence: {out}',flush=True)

if __name__=='__main__':main()
