"""Assemble the authorized capacity-matched image; no physical device access."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import uuid
import zlib
from create_ext4_fixtures import ROOT
from create_boot_img import create_esp_partition,GUID_ESP,GUID_LINUX_FS

def sha(path):
    h=hashlib.sha256()
    with path.open('rb') as stream:
        while chunk:=stream.read(1024*1024):h.update(chunk)
    return h.hexdigest()

def main():
    parser=argparse.ArgumentParser();parser.add_argument('workspace',type=Path);args=parser.parse_args()
    root=ROOT/'.codex-remote-attachments/ext4-phase9';workspace=args.workspace.resolve();assert workspace.is_relative_to(root.resolve())
    manifest=json.loads((workspace/'workspace-manifest.json').read_text());capacity=manifest['target']['capacity_bytes'];assert capacity==4026531840
    out=workspace/'physical-artifact';out.mkdir(exist_ok=True);image=out/'fortress-ext4-9.6-dell5590.img'
    seed=ROOT/'.codex-remote-attachments/ext4-phase8-5/host-frz8r6nu/4096-normal-512-pending-seed.img'
    data=seed.read_bytes();assert len(data)==32*1024*1024
    entries=bytearray(16384);data_start=133120;data_end=data_start+len(data)//512-1
    for index,(kind,guid,start,end,label) in enumerate(((GUID_ESP,uuid.uuid4().bytes_le,2048,133119,'EXT4 9.6 BOOT'),(GUID_LINUX_FS,uuid.UUID(manifest['data_partuuid']).bytes_le,data_start,data_end,'EXT4 9.6 DISPOSABLE'))):
        struct.pack_into('<16s16sQQQ72s',entries,index*128,kind,guid,start,end,0,label.encode('utf-16le').ljust(72,b'\0'))
    sectors=capacity//512;disk_guid=uuid.uuid4().bytes_le
    def header(current,alternate,array):
        result=bytearray(struct.pack('<8sIIIIQQQQ16sQIII',b'EFI PART',0x10000,92,0,0,current,alternate,34,sectors-34,disk_guid,array,128,128,zlib.crc32(entries)))
        struct.pack_into('<I',result,16,zlib.crc32(result));return result.ljust(512,b'\0')
    esp=out/'esp.img'
    create_esp_partition(esp,None,workspace/'limine',workspace/'bin/fortress.elf',workspace/'bin/initramfs.tar',workspace/'limine.conf',workspace/'assets/splash.png')
    mbr=bytearray(512);struct.pack_into('<B3sB3sII',mbr,446,0,b'\0\2\0',0xee,b'\xff'*3,1,sectors-1);mbr[510:]=b'\x55\xaa'
    with image.open('xb') as stream:
        stream.truncate(capacity)
        for offset,payload in ((0,mbr),(512,header(1,sectors-1,2)),(1024,entries),(2048*512,esp.read_bytes()),(data_start*512,data),((sectors-33)*512,entries),((sectors-1)*512,header(sectors-1,1,sectors-33))):stream.seek(offset);stream.write(payload)
    subprocess.run([str(workspace/'limine/limine'),'bios-install',str(image)],capture_output=True,text=True,check=True)
    info=subprocess.run(['dumpe2fs','-h',str(seed)],capture_output=True,text=True,check=True)
    (out/'fixture-dumpe2fs.txt').write_text(info.stdout+info.stderr)
    # The seed deliberately needs journal/orphan recovery; retain that state.
    shutil.copyfile(seed,out/'pending-seed.ext4')
    (out/'manifest.json').write_text(json.dumps({'workspace':str(workspace),'target':manifest['target'],'image':str(image),'image_sha256':sha(image),'data_partuuid':manifest['data_partuuid'],
        'data_start_lba':data_start,'data_bytes':len(data),'seed_sha256':sha(seed),'kernel_sha256':sha(workspace/'bin/fortress.elf'),'physical_writes':False,'tests':'pending'},indent=2)+'\n')
    (out/'SHA256SUMS').write_text(sha(image)+'  '+image.name+'\n')
    print(f'Prepared regular image: {image}',flush=True)

if __name__=='__main__':main()
