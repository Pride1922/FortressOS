"""External TEST signatures and fresh fixture floor; never a device/provisioner."""
import json,os,pathlib,struct
from pkg_enroll_records import H,keys,signed
from pkg_enroll_fixture import command,BS
from pkg_pending_records import observation
from pkg_running_root_records import build as running_build

def certificate(policy,locator,epoch,counters,hashes,signers,selector=None,contract=3,issued=1792843199,expires=1792929600):
    wire=bytearray(640);wire[:16]=b'FORT-PKG-FLOOR1\0';struct.pack_into('<IHHQI',wire,16,640+96*2,1,2,epoch,contract)
    wire[40:120]=policy[792:872];wire[120:152]=policy[904:936];wire[152:184]=policy[936:968];wire[184:216]=policy[968:1000]
    struct.pack_into('<5Q',wire,216,*counters);wire[256:352]=b''.join(hashes);wire[352:384]=selector if selector is not None else policy[1064:1096]
    struct.pack_into('<QQ',wire,384,issued,expires);wire[448:480]=H(locator);wire[480:608]=locator;wire[608:640]=H(wire[:608]);return signed(wire,signers).ljust(1024,b'\0')

def build(image,out,roles,kernel,bootstrap):
    out=pathlib.Path(out);directory=out.parent/'trust-input';directory.mkdir();placeholder=directory/'floor';placeholder.write_bytes(b'F'*1024)
    cmd=directory/'commands';cmd.write_text(f'write {placeholder} /pkg/trust-floor.v1\nset_inode_field /pkg/trust-floor.v1 mode 0100600\nset_inode_field /pkg/trust-floor.v1 generation 990\n');(directory/'seed.log').write_text(command(['debugfs','-w','-f',str(cmd),str(image)]))
    prepared=running_build(image,out,roles,kernel,bootstrap,root_version=4,catalog_revision=8)
    obj=observation(image,'/pkg/trust-floor.v1',49);locator=bytearray(128);role,ino,gen,mode,uid,gid,links,count,size,blocks=struct.unpack_from('<8IQQ',obj);assert uid==gid==0 and links==1 and count==blocks==1
    struct.pack_into('<HHIIIQQH',locator,0,49,1,ino,gen,mode,size,blocks,count);locator[40:56]=obj[80:96];locator=bytes(locator)
    policy=prepared['policy'][:1136];base=prepared['baseline'];parts=[];at=96
    for i in range(8):n=struct.unpack_from('<I',base,32+8*i)[0];parts.append(base[at:at+n]);at+=n
    root=json.loads(prepared['root_authority']);root['version']=3;oldroot=H(json.dumps(root,sort_keys=True,separators=(',',':')).encode())
    trust=parts[7];cn=struct.unpack_from('<I',trust,96)[0];body=bytearray(trust[256:256+cn-192]);struct.pack_into('<QQ',body,32,3,7);oldcatalog=H(signed(body,keys(roles['catalog'])))
    signers=keys(roles['selector']);newroot=policy[1000:1032];newcatalog=policy[1032:1064];artifact=H(prepared['artifact'])
    initial=certificate(policy,locator,1,(3,7,0,1,1),(oldroot,oldcatalog,bytes(32)),signers,selector=H(b'authorized predecessor selector'))
    pre=certificate(policy,locator,2,(4,8,0,1,1),(newroot,newcatalog,bytes(32)),signers)
    end=certificate(policy,locator,3,(4,8,1,7,2),(newroot,newcatalog,artifact),signers)
    physical=struct.unpack_from('<Q',locator,48)[0]
    with image.open('r+b') as f:f.seek(physical*BS);assert f.read(1024)==b'F'*1024;f.seek(physical*BS);f.write(initial);f.flush();os.fsync(f.fileno())
    prepared['policy']+=initial+pre+end;prepared['trust_floors']=[initial,pre,end]
    return prepared

def verify_readback_and_raise(root_image,policy_bytes):
    """Offline checkpoint publication only after durable signed-record readback.
    This selects a stronger minimum; it never modifies the target root image.
    """
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
    if root_image.is_symlink() or not root_image.is_file():raise ValueError('regular root readback required')
    baseline_n=struct.unpack_from('<I',policy_bytes,1168)[0];offset=1152+baseline_n;minimum,pre,end=[policy_bytes[offset+1024*i:offset+1024*(i+1)] for i in range(3)];assert len(policy_bytes)==offset+3072
    for record in (minimum,pre,end):
        assert record[:16]==b'FORT-PKG-FLOOR1\0' and H(record[:608])==record[608:640]
        size,version,count=struct.unpack_from('<IHH',record,16);assert version==1 and count==2 and size==832 and not any(record[size:]);seen=[]
        for i in range(count):
            row=record[640+96*i:736+96*i];matches=[policy_bytes[j*32:(j+1)*32] for j in range(3) if H(policy_bytes[j*32:(j+1)*32])==row[:32]];assert len(matches)==1 and row[:32] not in seen;seen.append(row[:32]);Ed25519PublicKey.from_public_bytes(matches[0]).verify(row[32:],record[:640])
        assert seen==sorted(seen)
    data=root_image.read_bytes();physical=struct.unpack_from('<Q',end,528)[0];assert data[physical*BS:physical*BS+1024]==end
    # Current grant/state must satisfy the completion floor, independently of
    # certificate presence. Read actual fixed files via the raw inode witness.
    from pkg_enroll_identity import inventory
    witness,audit=inventory(root_image)
    for index,offset_in_file,value in ((1,24,7),(6,24,2)):
        row=witness[32+128*index:32+128*(index+1)];p=struct.unpack_from('<Q',row,48)[0];assert struct.unpack_from('<Q',data,p*BS+offset_in_file)[0]==value
    return policy_bytes[:offset]+end+pre+end
