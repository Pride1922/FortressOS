"""Verify/copy an already-built normal image; never accesses a device."""
from pathlib import Path
import argparse,binascii,hashlib,json,shutil,struct,subprocess,tarfile,uuid
root=Path(__file__).resolve().parent.parent
p=argparse.ArgumentParser();p.add_argument('--workspace',type=Path,required=True);a=p.parse_args()
workspace=a.workspace.resolve();assert workspace.is_relative_to(root/'.codex-remote-attachments/ext4-phase9')
out=root/'build/permissions-phase5/delivery';out.mkdir(exist_ok=False)
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
source=workspace/'bin/fortress.img';raw=source.read_bytes();assert len(raw)==266240*512
def header(lba):
    h=bytearray(raw[lba*512:(lba+1)*512]);assert h[:8]==b'EFI PART'
    size,crc=struct.unpack_from('<II',h,12);assert size==92
    struct.pack_into('<I',h,16,0);assert binascii.crc32(h[:size])&0xffffffff==crc
    entries,count,stride,entry_crc=struct.unpack_from('<QIII',h,72)
    table=raw[entries*512:entries*512+count*stride]
    assert len(table)==count*stride and binascii.crc32(table)&0xffffffff==entry_crc
    return table
table=header(1);assert header(266239)==table
assert struct.unpack_from('<QQ',table,32)==(2048,133119)
assert struct.unpack_from('<QQ',table,128+32)==(133120,264191)
for name in ('fortress.elf','initramfs.tar'):
    dest=out/('embedded-'+name)
    subprocess.run(['mcopy','-i',str(source)+'@@1048576','::boot/'+name,str(dest)],check=True)
    assert sha(dest)==sha(workspace/'bin'/name),'stale embedded artifact'
symbols=subprocess.check_output(['nm',str(out/'embedded-fortress.elf')],text=True)
assert 'sys_test_setcreds' not in symbols and b'login=0' not in (out/'embedded-fortress.elf').read_bytes()
(out/'normal-symbols.txt').write_text(symbols)
with tarfile.open(out/'embedded-initramfs.tar') as tar:
    shadow=tar.extractfile('etc/shadow').read()
    assert shadow==b'root:!:::::::\noperator::::::::\n'
    for name,mode in [('bin/sudo',0o4755),('etc/shadow',0o600),('etc/passwd',0o644),('etc/group',0o644)]:
        member=tar.getmember(name);assert (member.uid,member.gid,member.mode)==(0,0,mode)
    assert not any(m.name in ('bin/suid-probe','bin/phase4-probe','bin/perm-probe') for m in tar.getmembers())
part=out/'shipped-data.ext4';part.write_bytes(raw[133120*512:264192*512])
audit=subprocess.run(['e2fsck','-fn',str(part)],capture_output=True,text=True)
(out/'shipped-data-fsck.log').write_text(audit.stdout+audit.stderr);assert audit.returncode==0
conf=subprocess.check_output(['mtype','-i',str(source)+'@@1048576','::boot/limine.conf'],text=True)
assert 'login=0' not in conf and 'usb_data_mode=rw' in conf and 'usb_data_mode=ro' in conf
(out/'embedded-limine.conf').write_text(conf)
identity=str(uuid.UUID(bytes_le=table[144:160]))
assert identity.upper() in conf.upper()
inventory=json.loads((workspace/'workspace-manifest.json').read_text())
for name,digest in inventory['sources'].items():
    assert sha(workspace/name)==digest,'production snapshot changed'
runtime={name:digest for name,digest in inventory['sources'].items() if name.startswith(('src/','user/'))}
assert all(sha(root/name)==digest for name,digest in runtime.items()),'current runtime differs from tested image'
shutil.copyfile(workspace/'workspace-manifest.json',out/'production-source-manifest.json')
baseline=root/'build/permissions-phase5/baseline';prior={}
for name in ('fortress.img','fortress.elf','initramfs.tar','fortress.iso'):
    target=root/'bin'/name
    if target.is_file():
        backup=baseline/('prior-'+name);shutil.copyfile(target,backup);prior[name]=sha(backup)
    shutil.copyfile(workspace/'bin'/name,target)
    assert sha(target)==sha(workspace/'bin'/name)
image=out/'fortress-permissions-phase5.img';shutil.copyfile(source,image)
(out/'SHA256SUMS').write_text(sha(image)+'  fortress-permissions-phase5.img\n'+sha(root/'bin/fortress.img')+'  bin/fortress.img\n')
(out/'manifest.json').write_text(json.dumps({'git_base':'700ba89','physical_acceptance':'PENDING user Dell 5590 results',
 'production_workspace':str(workspace),'runtime_inputs_verified':len(runtime),'data_partuuid':identity,
 'image_sha256':sha(image),'prior_artifacts':prior,'artifacts':{q.name:sha(q) for q in out.iterdir() if q.is_file()},
 'checks':['primary/backup GPT CRC','embedded ELF/initramfs hashes','normal symbols/no login escape',
 'locked root/passwordless operator/normalized sudo and database metadata','Linux fsck clean shipped EXT4',
 'current runtime byte hashes match immutable build snapshot'],
 'scope':'regular build files only; no flash/hardware/device I/O; pending physical acceptance'},indent=2)+'\n')
print('Verified fresh image:',image);print('SHA-256:',sha(image));print('Data PARTUUID:',identity)
