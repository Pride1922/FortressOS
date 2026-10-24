"""Offline TEST recovery images. No provisioning or live installation API."""
import struct
from pkg_enroll_records import H,sealed,header,build
from pkg_enroll_fixture import command,inode,BS
from pkg_mount_vectors import vectors
from pkg_current_records import advance

PATHS={1:'/pkg/transaction.v1/archive',17:'/pkg/transaction.v1/activation',18:'/pkg/transaction.v1/next.v1',19:'/pkg/transaction.v1/pending.v1'}

def observation(image,path,role):
    data=image.read_bytes();ino=inode(image,path);ipg=struct.unpack_from('<I',data,1064)[0];g,i=divmod(ino-1,ipg)
    table=struct.unpack_from('<I',data,BS+32*g+8)[0];raw=data[table*BS+256*i:table*BS+256*(i+1)]
    mode,uid=struct.unpack_from('<HH',raw);gid,links=struct.unpack_from('<HH',raw,24);gen=struct.unpack_from('<I',raw,100)[0]
    size=struct.unpack_from('<I',raw,4)[0]+(struct.unpack_from('<I',raw,108)[0]<<32)
    magic,count,maximum,depth=struct.unpack_from('<4H',raw,40);assert magic==0xf30a and maximum==4 and depth==0 and 1<=count<=4
    ext=[];body=b'';blocks=0
    for j in range(count):
        lo,n,hi,p=struct.unpack_from('<IHHI',raw,52+12*j);assert hi==0 and 0<n<32768 and lo==blocks
        ext.append(struct.pack('<IIQ',lo,n,p));body+=data[p*BS:(p+n)*BS];blocks+=n
    assert blocks==(size+BS-1)//BS and struct.unpack_from('<I',raw,28)[0]==blocks*8
    return struct.pack('<8IQQ',role,ino,gen,mode&0o7777,uid,gid,links,count,size,blocks)+H(body[:size])+b''.join(ext).ljust(64,b'\0')

def prepare(image,out,roles,kernel,bootstrap,phase=2,published=False,installed=False):
    out.mkdir();inputs=out/'payloads';vectors(inputs);artifact=(inputs/'artifact.fpk').read_bytes();exe=(inputs/'executable.bin').read_bytes()
    commands=[f'write {inputs/"artifact.fpk"} /pkg/artifact.fpk','set_inode_field /pkg/artifact.fpk mode 0100600','set_inode_field /pkg/artifact.fpk generation 900']
    for role,path in PATHS.items():
        if role==19 and phase==1:continue
        if role==17 and published:path='/usr/bin/demo'
        source=inputs/('artifact.fpk' if role==1 else 'executable.bin' if role==17 else f'placeholder-{role}')
        if role in (18,19):source.write_bytes(b'P'*1024)
        commands += [f'write {source} {path}',f'set_inode_field {path} mode {"0100755" if role==17 else "0100600"}',f'set_inode_field {path} generation {900+role}']
    script=out/'seed.commands';script.write_text('\n'.join(commands)+'\n');(out/'seed.log').write_text(command(['debugfs','-w','-f',str(script),str(image)]))
    baseline=build(image,out/'records',roles,kernel,bootstrap,artifact=artifact)
    parts=list(baseline['parts']);target=advance(baseline,artifact,exe)[0][6];txn=bytes([0xa1])*16
    plan=H(H(target)+H(artifact)+parts[2][104:136]);intent=header('FORT-PKG-PN-v1');struct.pack_into('<H',intent,22,1)
    intent[24:72]=txn+parts[2][56:72]+parts[2][88:104]
    intent[72:232]=H(parts[6])+H(target)+H(parts[7])+H(artifact)+plan
    struct.pack_into('<7Q',intent,232,1,2,3,7,1792843200,len(artifact),1);intent[288]=4;intent[289:293]=b'demo';intent[328:360]=H(exe);struct.pack_into('<Q',intent,360,len(exe));intent=sealed(intent)
    # Replace preallocated transient contents without changing maps.
    for role,body in ((18,target),(19,intent)):
        if role==19 and phase==1:continue
        obj=observation(image,PATHS[role],role);count=struct.unpack_from('<I',obj,28)[0]
        with image.open('r+b') as f:
            at=0
            for j in range(count):
                lo,n,p=struct.unpack_from('<IIQ',obj,80+16*j);take=min(n*BS,len(body)-at);f.seek(p*BS);f.write(body[at:at+take]);at+=take
            assert at==len(body)
    objects=[]
    for role,path in PATHS.items():
        if role==19 and phase==1:continue
        objects.append(observation(image,'/usr/bin/demo' if role==17 and published else path,role))
    table=bytearray(parts[3]);table[32:48]=txn;struct.pack_into('<H',table,22,len(objects));struct.pack_into('<Q',table,48,2)
    charged=[0,0]
    for i,obj in enumerate(objects):
        role,ino,gen=struct.unpack_from('<3I',obj);size,blocks=struct.unpack_from('<QQ',obj,32);charged[0]+=blocks;charged[1]+=1
        struct.pack_into('<HHIII16sQQQ',table,256+64*i,role,1,ino,gen,1,txn,size,blocks,1)
    table=sealed(table);grant=bytearray(parts[2]);struct.pack_into('<H',grant,22,phase);struct.pack_into('<Q',grant,24,2);grant[40:56]=txn
    if phase>=2:grant[136:168]=H(intent);grant[200:232]=H(target);struct.pack_into('<Q',grant,368,2)
    else:grant[200:232]=bytes(32);struct.pack_into('<Q',grant,368,0)
    grant[264:296]=H(table);total=[8+(len(artifact)+BS-1)//BS+(len(exe)+BS-1)//BS+2,8]
    struct.pack_into('<6Q',grant,296,*total,*(total[i]-charged[i] for i in range(2)),*charged);struct.pack_into('<H',grant,348,1);grant[392:424]=plan;grant=sealed(grant)
    account=bytearray(parts[4]);struct.pack_into('<Q',account,440,2);account[448:480]=H(grant)
    if installed:parts[6]=target;account[512:544]=H(target)
    if phase>=2:account[544:576]=H(intent)
    account[576:592]=txn;account=sealed(account);parts[2:5]=[grant,table,account]
    prepared=dict(baseline,parts=parts,baseline=baseline['bundle'],artifact=artifact,executable=exe,objects=b''.join(objects),intent=intent if phase>=2 else b'',published=published)
    prepared['policy']=baseline['policy']+struct.pack('<QQ',1,1)+baseline['bundle']
    prepared['bundle']=baseline['bundle'][:96]+b''.join(parts)
    return prepared
