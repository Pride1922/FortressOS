"""Fresh TEST-only EXT4/GPT geometry fixture; contains no signed enrollment.

Only creates new regular files in an existing private output directory. Never
opens a device, existing filesystem, production key or release build output.
"""
import pathlib, re, struct, subprocess, uuid, zlib
from test_jbd2_replay_host import FEATURES, blocks, crc, be, put, seal, sbseal

BS=4096
PART=uuid.UUID('12345678-1234-4567-89ab-123456789abc')
FS=uuid.UUID('22345678-1234-4567-89ab-123456789abc')
FILES={'/etc/passwd':b'root:x:0:0:root:/root:/bin/shell\n',
       '/etc/group':b'root:x:0:\n', '/etc/shadow':b'root:!:::::::\n'}
SIZES={'enrollment.v1':512,'grant.v1':1024,'ownership.v1':8192,
       'origin.v1':1024,'account-generation.v1':1024,'state.v1':1024,
       'trust.v1':1024,'descriptor.v2':2656}

def command(args):
    p=subprocess.run(args,capture_output=True,text=True,timeout=60)
    if p.returncode:raise RuntimeError((args,p.returncode,p.stdout,p.stderr))
    return p.stdout+p.stderr

def inode(image,path):
    text=command(['debugfs','-R','stat '+path,str(image)])
    return int(re.search(r'Inode:\s*(\d+)',text)[1])

def directory(data,ino,parent,entries,slots,physical):
    """Canonical fixed slots and ext4 directory checksum, independent of C."""
    b=bytearray(BS);at=0
    def entry(number,name,kind,n):
        nonlocal at
        name=name.encode();struct.pack_into('<IHBB',b,at,number,n,len(name),kind)
        b[at+8:at+8+len(name)]=name;at+=n
    entry(ino,'.',2,12);entry(parent,'..',2,12)
    for number,name in entries:entry(number,name,1,24)
    for _ in range(slots-len(entries)):entry(0,'',0,24)
    while at<BS-12:entry(0,'',0,16 if (BS-12-at)%12 else 12)
    assert at==BS-12
    struct.pack_into('<H',b,BS-8,12);b[BS-5]=0xde
    ipg=struct.unpack_from('<I',data,1064)[0];g,index=divmod(ino-1,ipg)
    table=struct.unpack_from('<I',data,BS+g*32+8)[0]
    generation=struct.unpack_from('<I',data,table*BS+index*256+100)[0]
    seed=crc(crc(0xffffffff,data[1128:1144]),struct.pack('<II',ino,generation))
    struct.pack_into('<I',b,BS-4,crc(seed,b[:BS-12]))
    data[physical*BS:(physical+1)*BS]=b

def gpt(root):
    d=bytearray(64*1024*1024);last=len(d)//512-1
    d[450]=0xee;struct.pack_into('<II',d,454,1,last);d[510:512]=b'\x55\xaa'
    table=bytearray(128*128)
    kind=uuid.UUID('0fc63daf-8483-4772-8e79-3d69d8477de4').bytes_le
    for i,(start,size,identity,label) in enumerate(((2048,len(root)//512,PART,'PKG TEST ONLY'),
         (69632,4096,uuid.UUID('32345678-1234-4567-89ab-123456789abc'),'UNRELATED SENTINEL'))):
        off=i*128;table[off:off+16]=kind;table[off+16:off+32]=identity.bytes_le
        struct.pack_into('<QQQ',table,off+32,start,start+size-1,0)
        name=label.encode('utf-16le');table[off+56:off+56+len(name)]=name
    for here,other,array in ((1,last,2),(last,1,last-32)):
        h=bytearray(512)
        struct.pack_into('<8sIIIIQQQQ16sQIII',h,0,b'EFI PART',0x10000,92,0,0,
            here,other,34,last-33,uuid.UUID('42345678-1234-4567-89ab-123456789abc').bytes_le,
            array,128,128,zlib.crc32(table))
        struct.pack_into('<I',h,16,zlib.crc32(h[:92]))
        d[here*512:(here+1)*512]=h;d[array*512:array*512+len(table)]=table
    d[2048*512:2048*512+len(root)]=root
    d[69632*512:(69632+4096)*512]=b'UNRELATED-TEST!!'*(4096*512//16)
    return d

def create(out,slot_layout=True,identities=False):
    out=pathlib.Path(out);assert out.is_dir() and not out.is_symlink()
    tree=out/'tree';tree.mkdir(mode=0o700)
    dirs={'etc':0o755,'var':0o755,'var/lib':0o755,'var/lib/fortress':0o755,
          'var/lib/fortress/accounts':0o700,'usr':0o755,'usr/bin':0o755,
          'usr/share':0o755,'usr/share/pkg':0o755,'root':0o700,
          'pkg':0o700,'pkg/transaction.v1':0o700}
    for path,mode in dirs.items():p=tree/path;p.mkdir(mode=mode);p.chmod(mode)
    for path,content in FILES.items():
        p=tree/path.lstrip('/');p.write_bytes(content);p.chmod(0o600 if path.endswith('shadow') else 0o644)
    for name,size in SIZES.items():
        p=tree/'pkg'/name;p.write_bytes(b'P'*size);p.chmod(0o600)
    image=out/'root.img'
    with image.open('xb') as f:f.truncate(32*1024*1024)
    log=command(['mke2fs','-q','-t','ext4','-b','4096','-I','256','-O',FEATURES,
         '-E','lazy_itable_init=0','-m','0','-g','4096','-N','512','-J','size=4',
         '-U',str(FS),'-d',str(tree),str(image)])
    # Host UID must not become target authority. debugfs reseals inode checksums.
    paths=['/']+['/'+p for p in dirs]+list(FILES)+['/pkg/'+p for p in SIZES]
    path_ids={p:inode(image,p) for p in paths}
    dir_maps={p:blocks(image,p) for p in ['/']+['/'+p for p in dirs]}
    commands=out/'prepare.commands'
    commands.write_text(''.join(f'set_inode_field <{path_ids[p]}> uid 0\nset_inode_field <{path_ids[p]}> gid 0\n'+
                        (f'set_inode_field <{path_ids[p]}> generation {100+i}\n' if identities else '') for i,p in enumerate(paths))+
                        'set_inode_field <2> mode 040755\njournal_open -c -v 3\njournal_close\n')
    log+=command(['debugfs','-w','-f',str(commands),str(image)])
    data=bytearray(image.read_bytes());jino=struct.unpack_from('<I',data,1248)[0]
    jmap=blocks(image,f'<{jino}>');start=jmap[0]*BS;journal=bytearray(data[start:start+BS])
    assert be(journal,40) in (0x10,0x11)
    put(journal,24,1);put(journal,28,0);put(journal,88,1);put(journal,40,0x11)
    head=bytearray(journal[:1024]);seal(head,252,0xffffffff);journal[:1024]=head
    data[start:start+BS]=journal
    struct.pack_into('<I',data,1120,0x42);struct.pack_into('<H',data,1082,1);sbseal(data)
    if identities:
        # Changing generation changes directory checksum seeds too. debugfs
        # updates inode checksums but does not reseal these directory blocks.
        for path in ['/']+['/'+p for p in dirs]:
            ino=path_ids[path];ipg=struct.unpack_from('<I',data,1064)[0]
            g,index=divmod(ino-1,ipg);table=struct.unpack_from('<I',data,BS+g*32+8)[0]
            generation=struct.unpack_from('<I',data,table*BS+index*256+100)[0]
            seed=crc(crc(0xffffffff,data[1128:1144]),struct.pack('<II',ino,generation))
            for physical in dir_maps[path]:
                start=physical*BS;b=data[start:start+BS]
                assert b[-12:-4]==b'\0\0\0\0\x0c\0\0\xde'
                struct.pack_into('<I',data,start+BS-4,crc(seed,b[:-12]))
    if slot_layout:
        for path,parent,entries,slots in (('/etc','/',[(path_ids[p],p.rsplit('/',1)[1]) for p in FILES],6),
                                         ('/var/lib/fortress/accounts','/var/lib/fortress',[],5)):
            mapping=dir_maps[path];assert len(mapping)==1
            directory(data,path_ids[path],path_ids[parent],entries,slots,mapping[0])
    image.write_bytes(data)
    disk=out/'disk.img'
    with disk.open('xb') as f:f.write(gpt(data))
    (out/'generation.log').write_text(log)
    return image,disk
