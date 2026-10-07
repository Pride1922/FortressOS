"""Negative mounted diagnostics on owned images; no physical device access."""
import argparse,json,shutil,struct,subprocess,tempfile
from pathlib import Path
from test_ext4_physical_image import REPO,config,put_config,reject
from test_jbd2_replay_host import crc

def main():
    parser=argparse.ArgumentParser();parser.add_argument('workspace',type=Path);args=parser.parse_args()
    root=(REPO/'.codex-remote-attachments/ext4-phase9').resolve();ws=args.workspace.resolve()
    assert ws.is_relative_to(root)
    info=json.loads((ws/'physical-artifact/manifest.json').read_text())
    out=Path(tempfile.mkdtemp(prefix='physical-diagnostics-',dir=root));rows=[];errors=[]
    try:
        for mode in ('bios','uefi'):
            label=mode+'-unsupported-profile';disk=out/(label+'.img')
            subprocess.run(['cp','--sparse=always',info['image'],str(disk)],check=True)
            for name in ('fortress.elf','initramfs.tar'):
                subprocess.run(['mcopy','-o','-i',str(disk)+'@@1048576',str(ws/'bin'/name),'::boot/'+name],capture_output=True,check=True)
            conf=out/(label+'.conf');put_config(disk,conf,config(info['data_partuuid']))
            # CRC-valid unsupported superblock revision must fail before replay.
            offset=info['data_start_lba']*512+1024
            with disk.open('r+b') as stream:
                stream.seek(offset);sb=bytearray(stream.read(1024));struct.pack_into('<I',sb,76,99)
                struct.pack_into('<I',sb,1020,crc(0xffffffff,sb[:1020]));stream.seek(offset);stream.write(sb)
            iso=out/(label+'.iso')
            subprocess.run(['xorriso','-indev',str(ws/'bin/fortress.iso'),'-outdev',str(iso),'-boot_image','any','replay','-map',str(conf),'/boot/limine/limine.conf','-map',str(conf),'/boot/limine.conf'],capture_output=True,check=True)
            subprocess.run([str(ws/'limine/limine'),'bios-install',str(iso)],capture_output=True,check=True)
            row=reject(out,disk,iso,ws/'bin/fortress.elf',mode,label,b'[FAIL] ext2/audit: ')
            log=(out/(label+'.serial.log')).read_text()
            assert '[EXT4 DEBUG] mount stage=journal-source errno=95 published=0' in log
            assert '[USB DEBUG] last-op=' in log and 'command-failed=0 transport-failed=0 offline=0' in log
            row['mount_stage']='journal-source';row['errno']=95;rows.append(row)
            print('PASS mounted diagnostics '+mode,flush=True)
    except Exception as error:errors.append(repr(error));raise
    finally:
        (out/'manifest.json').write_text(json.dumps({'workspace':str(ws),'cases':rows,'errors':errors,'scope':'Injected unsupported filesystem; unchanged FS; zero USB write callback hits; no hardware claim'},indent=2)+'\n')
        print('Evidence: '+str(out),flush=True)

if __name__=='__main__':main()
