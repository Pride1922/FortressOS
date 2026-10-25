"""Fresh disposable executor fixture; no enrollment or physical provisioning."""
import pathlib,struct,hmac,hashlib,json
from pkg_enroll_fixture import create,command
from pkg_enroll_records import H,PUB,signed
from pkg_pending_records import observation
from pkg_artifact_vectors import elf,tar_header,pack
from pkg_manifest_vectors import text
def fixture(out,signers,record_key,docs=15,large=False,low_space=False):
    out=pathlib.Path(out);out.mkdir();image,_=create(out,identities=True)
    executable=elf()+(bytes(4*1024*1024-121) if large else b'')
    members=[('bin/hello',executable,0o755,1)]+[(f'doc/shared/{i:02}/a/b/c/readme' if not large else f'doc/{i:02}/readme',b'documentation\n'*(i+1),0o644,2) for i in range(docs)]
    manifest=struct.pack('<4sHHQQQ',b'FMN1',1,len(members),73,1,3)+bytes(range(32))+text('toolbox')+text('hello')+text('73')+text('executor fixture');tar=b''
    for path,data,mode,role in members:manifest+=struct.pack('<BBHQ',role,0,mode,len(data))+H(data)+text(path);tar+=tar_header(path,data,mode)+data+bytes((-len(data))%512)
    artifact=pack(manifest,tar+bytes(1024));now=1792929600
    body=struct.pack('<16s16sQQQQHHI',b'FORT-PKG-CAT-v1\0',bytes([4])*16,3,7,now-2,now+86400,1,2,0)+bytes([7])+b'toolbox'.ljust(32,b'\0')+struct.pack('<QQ',73,len(artifact))+H(artifact);catalog=signed(body,signers)
    policy=bytes([4])*16+b''.join(PUB(k) for k in signers).ljust(256,b'\0')+struct.pack('<IIQQQ',3,2,3,7,now)+H(catalog)
    seed=out/'placeholder';seed.write_bytes(b'F'*8192);commands=out/'extra.commands';commands.write_text(f'write {seed} /pkg/install.v2\nset_inode_field /pkg/install.v2 mode 0100600\nset_inode_field /pkg/install.v2 generation 999\nmkdir /pkg/work-v2\nset_inode_field /pkg/work-v2 mode 040700\n')
    with commands.open('a') as f:
        for path in ('/pkg','/usr','/usr/bin','/usr/share','/usr/share/pkg'):
            f.write(f'set_inode_field {path} mode {"040700" if path=="/pkg" else "040755"}\n')
        for path in ('/pkg/install.v2','/pkg/work-v2'):
            f.write(f'set_inode_field {path} uid 0\nset_inode_field {path} gid 0\n')
    (out/'extra.log').write_text(command(['debugfs','-w','-f',str(commands),str(image)]))
    if low_space:
        filler=out/'filler';filler.write_bytes(b'x'*(24*1024*1024))
        (out/'low-space.log').write_text(command(['debugfs','-w','-R',f'write {filler} /root/unrelated-filler',str(image)]))
    paths=('/pkg/install.v2','/pkg/work-v2','/usr/bin','/usr/share/pkg','/','/pkg','/usr','/usr/share');anchors=b''
    for i,path in enumerate(paths):
        obj=observation(image,path,100+i);ino,gen=struct.unpack_from('<II',obj,4);physical=struct.unpack_from('<Q',obj,88)[0];anchors+=struct.pack('<IIQ',ino,gen,physical)
    raw=image.read_bytes();fsid=raw[1128:1144];layout=H(fsid+anchors);record=bytearray(8192);record[:16]=b'FORT-PKG-EXEC1\0\0';struct.pack_into('<IH',record,16,8192,1);struct.pack_into('<QQ',record,24,1,1)
    record[320:352]=b'toolbox'.ljust(32,b'\0');record[352:384]=b'hello'.ljust(32,b'\0');record[384:416]=bytes(range(32));record[416:432]=fsid;record[432:464]=layout;record[8160:]=hmac.new(record_key,record[:8160],hashlib.sha256).digest();record=bytes(record)
    physical=struct.unpack_from('<Q',anchors,8)[0]
    with image.open('r+b') as f:f.seek(physical*4096);assert f.read(8192)==b'F'*8192;f.seek(physical*4096);f.write(record)
    config=fsid+anchors+struct.pack('<Q',1)+H(record)+layout+struct.pack('<QQ',struct.unpack_from('<I',raw,1028)[0],struct.unpack_from('<I',raw,1024)[0])+bytes(64);assert len(config)==296
    for name,data in (('artifact',artifact),('catalog',catalog),('policy',policy),('config',config)):(out/name).write_bytes(data)
    expected=[]
    for path,data,mode,role in members:
        public='/usr/bin/hello' if role==1 else '/usr/share/pkg/toolbox/73/'+path
        expected.append({'path':public,'sha256':H(data).hex(),'bytes':len(data),'mode':mode})
    (out/'expected.json').write_text(json.dumps(expected,indent=2)+'\n')
    return image
