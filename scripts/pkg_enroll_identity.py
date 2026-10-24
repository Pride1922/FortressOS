"""Independent raw-inode inventory for fresh, private enrollment fixtures.

Unsigned witness only, not a root of trust or a persistent package format.
The first sixteen rows use the reviewed enrollment locator representation.
"""
import hashlib, struct
from pkg_enroll_fixture import BS, inode

PATHS=('/pkg/enrollment.v1','/pkg/grant.v1','/pkg/ownership.v1',
       '/pkg/transaction.v1','/pkg/origin.v1','/pkg/account-generation.v1',
       '/pkg/state.v1','/pkg/trust.v1','/pkg','/etc','/var','/var/lib',
       '/var/lib/fortress','/var/lib/fortress/accounts','/usr/bin',
       '/usr/share/pkg','/pkg/descriptor.v2','/etc/passwd','/etc/group','/etc/shadow')

def inventory(image):
    data=image.read_bytes();assert len(data)==32*1024*1024
    assert struct.unpack_from('<I',data,1048)[0]==2
    assert struct.unpack_from('<H',data,1112)[0]==256
    ipg=struct.unpack_from('<I',data,1064)[0];rows=[];audit=[];seen=set();owned=set()
    for role,path in enumerate(PATHS,1):
        ino=inode(image,path);assert ino>=11 and ino not in seen;seen.add(ino)
        g,index=divmod(ino-1,ipg);gd=BS+g*32
        table=struct.unpack_from('<I',data,gd+8)[0]
        imap=struct.unpack_from('<I',data,gd+4)[0]*BS
        assert data[imap+index//8]&(1<<(index%8))
        raw=data[table*BS+index*256:table*BS+(index+1)*256]
        mode,uid=struct.unpack_from('<HH',raw);gid,links=struct.unpack_from('<HH',raw,24)
        size=struct.unpack_from('<I',raw,4)[0]+(struct.unpack_from('<I',raw,108)[0]<<32)
        generation=struct.unpack_from('<I',raw,100)[0]
        assert generation and uid==gid==0 and not any(raw[120:124])
        kind=2 if mode&0xf000==0x4000 else 1
        assert mode&0xf000 in (0x4000,0x8000) and (kind==2 or links==1)
        magic,count,maximum,depth=struct.unpack_from('<4H',raw,40)
        assert magic==0xf30a and 1<=count<=4 and maximum==4 and depth==0
        row=bytearray(128);struct.pack_into('<HHIIIQ',row,0,role,kind,ino,generation,mode&0o7777,size)
        struct.pack_into('<H',row,32,count);logical=0;content=bytearray();extents=[]
        for j in range(count):
            lo,n,high,physical=struct.unpack_from('<IHHI',raw,52+12*j)
            assert lo==logical and 0<n<=32768 and high==0 and physical+n<=len(data)//BS
            for block in range(physical,physical+n):
                assert block not in owned;owned.add(block)
                bg,bi=divmod(block,4096);bitmap=struct.unpack_from('<I',data,BS+bg*32)[0]*BS
                assert data[bitmap+bi//8]&(1<<(bi%8))
            struct.pack_into('<IIQ',row,40+16*j,lo,n,physical)
            logical+=n;content.extend(data[physical*BS:(physical+n)*BS]);extents.append([lo,n,physical])
        assert logical==(size+BS-1)//BS
        assert struct.unpack_from('<I',raw,28)[0]==logical*(BS//512)
        struct.pack_into('<Q',row,24,logical);rows.append(bytes(row))
        audit.append({'path':path,'inode':ino,'generation':generation,'kind':kind,
            'mode':oct(mode&0o7777),'size':size,'blocks':logical,'extents':extents,
            'content_sha256':hashlib.sha256(content[:size]).hexdigest()})
    header=bytearray(32);header[:16]=b'FORT-PKG-MAP-v1\0'
    # Explicit trailing NUL occupies byte 15; slice assignment must not resize.
    assert len(header)==32
    struct.pack_into('<IHH',header,16,32+128*len(rows),1,len(rows))
    return bytes(header)+b''.join(rows),audit
