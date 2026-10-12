"""Independent Linux oracle for a completed disposable installation."""
import binascii
import hashlib
import io
import re
import struct
import tarfile
import uuid
import test_root_phase0 as fixture


def audit(disk,out,kernel,operator,recovery,manifest,username="operator"):
    def run(args):return fixture.run(list(map(str,args)),out/'independent-audit.log')
    with disk.open('rb') as stream:
        sectors=disk.stat().st_size//512
        mbr=stream.read(512)
        assert mbr[450]==0xee and mbr[510:]==b'\x55\xaa'
        assert struct.unpack_from('<II',mbr,454)==(1,sectors-1)
        tables=[];headers=[]
        for current,backup,array in ((1,sectors-1,2),(sectors-1,1,sectors-33)):
            stream.seek(current*512);header=bytearray(stream.read(512));headers.append(header)
            assert header[:8]==b'EFI PART' and struct.unpack_from('<I',header,12)[0]==92
            crc=struct.unpack_from('<I',header,16)[0];struct.pack_into('<I',header,16,0)
            assert binascii.crc32(header[:92])==crc
            assert struct.unpack_from('<QQQQ',header,24)==(current,backup,34,sectors-34)
            assert struct.unpack_from('<QII',header,72)==(array,128,128)
            stream.seek(array*512);table=stream.read(16384);tables.append(table)
            assert binascii.crc32(table)==struct.unpack_from('<I',header,88)[0]
        assert tables[0]==tables[1] and headers[0][56:72]==headers[1][56:72]
        ids={uuid.UUID(bytes_le=bytes(headers[0][56:72]))};assert next(iter(ids)).version==4
        roles=((4096,131072,'c12a7328-f81f-11d2-ba4b-00a0c93ec93b'),
               (135168,524288,'0fc63daf-8483-4772-8e79-3d69d8477de4'),
               (2048,2048,'21686148-6449-6e6f-744e-656564454649'))
        for index,(start,count,kind) in enumerate(roles):
            entry=tables[0][index*128:(index+1)*128]
            assert str(uuid.UUID(bytes_le=entry[:16]))==kind
            identity=uuid.UUID(bytes_le=entry[16:32]);assert identity.version==4 and identity not in ids;ids.add(identity)
            assert struct.unpack_from('<QQ',entry,32)==(start,start+count-1)
        assert not any(tables[0][384:])
        root_id=str(uuid.UUID(bytes_le=tables[0][144:160]))
        for name,start,count in (('installed-esp.img',4096,131072),('installed-root.img',135168,524288)):
            stream.seek(start*512)
            with (out/name).open('xb') as target:
                remaining=count*512
                while remaining:
                    data=stream.read(min(1024**2,remaining));assert data;target.write(data);remaining-=len(data)
    esp=out/'installed-esp.img';root=out/'installed-root.img'
    run(['fsck.fat','-n',esp]);run(['e2fsck','-fn',root])
    for name in ('fortress.elf','bootstrap.tar','readiness.v1'):
        run(['mcopy','-i',esp,'::/boot/'+name,out/('installed-'+name)])
    assert fixture.digest(out/'installed-fortress.elf')==fixture.digest(kernel)
    for name in ('passwd','group','shadow','fortress-install.v1','hostname'):
        run(['debugfs','-R',f'dump /etc/{name} {out / ("installed-"+name)}',root])
    assert (out/'installed-hostname').read_bytes()==b'fortress\n'
    for path,mode,uid,gid in (('/etc/shadow','0600',0,0),('/home/'+username,'0700',1000,1000),('/etc/fortress-install.v1','0644',0,0)):
        text=run(['debugfs','-R','stat '+path,root])
        assert re.search(r'Mode:\s+'+mode,text) and re.search(r'User:\s+'+str(uid)+r'\s+Group:\s+'+str(gid),text)
    def record(data,magic):
        lines=data.rstrip(b'\0').decode('ascii').splitlines();assert lines.pop(0)==magic
        result={}
        for line in lines:
            key,value=line.split('=',1);assert key not in result;result[key]=value
        return result
    bootstrap=(out/'installed-bootstrap.tar').read_bytes()
    with tarfile.open(fileobj=io.BytesIO(bootstrap)) as archive:
        member=archive.getmember('bootstrap/recovery.v1');assert member.uid==member.gid==0 and member.mode==0o600
        recovery_record=record(archive.extractfile(member).read(),'FORTRESS-RECOVERY-1')
    ready_bytes=(out/'installed-readiness.v1').read_bytes();assert len(ready_bytes)==512
    ready=record(ready_bytes,'FORTRESS-READY-1')
    installed=record((out/'installed-fortress-install.v1').read_bytes(),'FORTRESS-INSTALL-1')
    for key in ('install_id','root_partuuid','filesystem_uuid'):
        assert ready[key]==installed[key]==recovery_record[key]
    assert ready['root_partuuid']==root_id
    assert ready['bootstrap_sha256']==hashlib.sha256(bootstrap).hexdigest()
    assert ready['kernel_sha256']==fixture.digest(kernel)
    assert ready['checksum']==hashlib.sha256(ready_bytes[:ready_bytes.index(b'checksum=')]).hexdigest()
    assert (out/'installed-passwd').read_text()==f'root:x:0:0::/root:/bin/shell\n{username}:x:1000:1000::/home/{username}:/bin/shell\n'
    assert (out/'installed-group').read_text()==f'root:x:0:\n{username}:x:1000:\nwheel:x:10:{username}\n'
    shadow=(out/'installed-shadow').read_text();assert shadow.splitlines()[1].split(':')[0]==username
    operator_hash=shadow.splitlines()[1].split(':')[1]
    recovery_hash=recovery_record['password_hash']
    assert fixture.crypt_hash(operator,operator_hash)==operator_hash
    assert fixture.crypt_hash(recovery,recovery_hash)==recovery_hash
    assert fixture.crypt_hash(operator,recovery_hash)!=recovery_hash and fixture.crypt_hash(recovery,operator_hash)!=operator_hash
    assert operator_hash.split('$')[3]!=recovery_hash.split('$')[3]
    assert all(secret.encode() not in bootstrap+ready_bytes+(out/'installed-shadow').read_bytes() for secret in (operator,recovery))
    manifest['cases'].append({'independent_linux_audit':'PASS','disk_guid':str(uuid.UUID(bytes_le=bytes(headers[0][56:72]))),'root_partuuid':root_id,'install_id':ready['install_id'],'operator_salt':operator_hash.split('$')[3],'recovery_salt':recovery_hash.split('$')[3]})
