"""Actual recovery reader, Linux-produced journals and independent replay oracle.
Only fresh regular files beneath build; no mounts, devices or physical claims.
"""
import hashlib,json,shutil,struct,subprocess,tempfile
from pathlib import Path
from create_ext4_fixtures import ROOT,run

FEATURES='none,extent,filetype,sparse_super,large_file,metadata_csum,has_journal'
TABLE=[]
for n in range(256):
    for _ in range(8):n=(n>>1)^(0x82f63b78 if n&1 else 0)
    TABLE.append(n)
def crc(seed,data):
    for n in data:seed=TABLE[(seed^n)&255]^(seed>>8)
    return seed
def be(data,off):return struct.unpack_from('>I',data,off)[0]
def put(data,off,n):struct.pack_into('>I',data,off,n&0xffffffff)
def seal(data,offset,seed):
    put(data,offset,0);put(data,offset,crc(seed,data))
def sbseal(disk):
    struct.pack_into('<I',disk,2044,crc(0xffffffff,disk[1024:2044]))
def blocks(image,name):
    r=subprocess.run(['debugfs','-R',f'blocks {name}',str(image)],capture_output=True,text=True,check=True)
    return [int(x) for x in r.stdout.split()]
def journal_put(disk,jmap,bs,index,data):disk[jmap[index]*bs:(jmap[index]+1)*bs]=data
def header(bs,kind,seq):
    b=bytearray(bs);put(b,0,0xc03b3998);put(b,4,kind);put(b,8,seq);return b
def oracle(source,target,log):
    shutil.copyfile(source,target)
    r=subprocess.run(['e2fsck','-fy','-E','journal_only',str(target)],capture_output=True,text=True)
    assert r.returncode in (0,1),r.stdout+r.stderr
    log.write_text(r.stdout+r.stderr+run(['e2fsck','-fn',str(target)]))
def case(out,bs):
    base=out/f'base-{bs}.img'
    with base.open('xb') as f:f.truncate(64*1024*1024)
    mkfs=['mke2fs','-q','-t','ext4','-b',str(bs),'-I','256','-O',FEATURES,'-E','lazy_itable_init=0','-m','0','-g','4096','-N','512','-J','size=4',str(base)]
    log=run(mkfs);seed=out/f'seed-{bs}.bin';seed.write_bytes(b'O'*(bs*2))
    log+=run(['debugfs','-w','-R',f'write {seed} /target.bin',str(base)])
    d=bytearray(base.read_bytes());ino=struct.unpack_from('<I',d,1248)[0];jmap=blocks(base,f'<{ino}>');target=blocks(base,'/target.bin');assert len(target)==2
    # inode 12 is produced by the sole debugfs write; derive actual identity.
    stat=run(['debugfs','-R','stat /target.bin',str(base)])
    import re
    file_ino=int(re.search(r'Inode: (\d+)',stat)[1]);ipg=struct.unpack_from('<I',d,1064)[0]
    group=(file_ino-1)//ipg;index=(file_ino-1)%ipg
    first=struct.unpack_from('<I',d,1044)[0];table=struct.unpack_from('<I',d,(first+1)*bs+group*32+8)[0]
    where=table*bs+index*256;inodeblock=where//bs
    escaped=bytes.fromhex('c03b3998')+b'A'*(bs-4)
    checkpoint=bytearray(d)
    struct.pack_into('<H',checkpoint,1076,struct.unpack_from('<H',checkpoint,1076)[0]+1)
    struct.pack_into('<I',checkpoint,1120,struct.unpack_from('<I',checkpoint,1120)[0]|4)
    sbseal(checkpoint);sbblock=1024//bs
    sbimage=bytes(checkpoint[sbblock*bs:(sbblock+1)*bs])
    payload=out/f'payload-{bs}.bin';payload.write_bytes(sbimage+d[inodeblock*bs:(inodeblock+1)*bs]+escaped+b'B'*bs)
    reuse=out/f'reuse-{bs}.bin';reuse.write_bytes(b'C'*bs)
    tail=out/f'tail-{bs}.bin';tail.write_bytes(b'D'*bs)
    commands=out/f'journal-{bs}.commands'
    commands.write_text(f'journal_open -c -v 3\njournal_write -b {sbblock},{inodeblock},{target[0]},{target[1]} {payload}\njournal_write -r {target[1]}\njournal_write -b {target[1]} {reuse}\njournal_write -c -b {target[0]} {tail}\njournal_close\n')
    log+=run(['debugfs','-w','-f',str(commands),str(base)])
    d=bytearray(base.read_bytes());js=d[jmap[0]*bs:(jmap[0]+1)*bs]
    assert be(js,40)==0x11 and be(js,28)>0,log
    # e2fsprogs 1.47 debugfs writes zero UUID slots (typed-pointer arithmetic
    # in its experimental journal writer). Keep raw output and normalize only
    # those descriptor UUID slots, independently resealing their checksums.
    uuid=bytes(d[1128:1144]);seed_crc=crc(0xffffffff,uuid);cursor=be(js,28)
    normalized=0
    while cursor<len(jmap):
        descriptor=cursor
        b=bytearray(d[jmap[cursor]*bs:(jmap[cursor]+1)*bs])
        if be(b,0)!=0xc03b3998:break
        kind=be(b,4);cursor+=1
        if kind!=1:continue
        at=12;changed=False
        while at+16<=bs-4:
            flags=be(b,at+4);at+=16;cursor+=1
            if not flags&2:
                if b[at:at+16]!=uuid:
                    assert not any(b[at:at+16]),'unexpected producer UUID mismatch'
                    b[at:at+16]=uuid;changed=True;normalized+=1
                at+=16
            if flags&8:break
        if changed:
            seal(b,bs-4,seed_crc)
            journal_put(d,jmap,bs,descriptor,b)
    log+=f'Normalized {normalized} zero tag UUID slots; raw producer image retained as {base.name}\n'
    # Torn home inode metadata, not the immutable journal identity, is restored.
    d[where:where+256]=bytes(256)
    linux=out/f'linux-{bs}.img';linux.write_bytes(d)
    expected=out/f'oracle-{bs}.img';oracle(linux,expected,out/f'linux-{bs}-oracle.log')
    dumped=out/f'expected-{bs}.bin';run(['debugfs','-R',f'dump /target.bin {dumped}',str(expected)])
    assert dumped.read_bytes()==escaped+b'C'*bs
    (out/f'linux-{bs}-generation.log').write_text(log+stat+run(['dumpe2fs','-h',str(expected)]))
    variants=[linux]
    # Independent circular/sequence-wrap vectors, using a real Linux inode map.
    wrapped=bytearray(d);uuid=bytes(d[1128:1144]);seed_crc=crc(0xffffffff,uuid)
    for n in range(1,len(jmap)):journal_put(wrapped,jmap,bs,n,bytes(bs))
    cursor=len(jmap)-2;start=cursor
    def append(data):
        nonlocal cursor
        journal_put(wrapped,jmap,bs,cursor,data);cursor=1 if cursor+1==len(jmap) else cursor+1
    for seq,kind in [(0xffffffff,0),(0,1),(1,2)]:
        if kind==1:
            b=header(bs,5,seq);put(b,12,20);put(b,16,target[1]);seal(b,bs-4,seed_crc);append(b)
        else:
            entries=[(inodeblock,bytes(d[inodeblock*bs:(inodeblock+1)*bs])),(target[0],escaped),(target[1],b'B'*bs)] if kind==0 else [(target[1],b'C'*bs)]
            # Obtain the valid inode image from the Linux replay oracle.
            if kind==0:entries=[(sbblock,sbimage),(inodeblock,expected.read_bytes()[inodeblock*bs:(inodeblock+1)*bs])]+entries[1:]
            b=header(bs,1,seq);at=12;data=[]
            for i,(home,value) in enumerate(entries):
                value=bytearray(value);flags=(2 if i else 0)|(8 if i+1==len(entries) else 0)
                if value[:4]==bytes.fromhex('c03b3998'):value[:4]=bytes(4);flags|=1
                put(b,at,home);put(b,at+4,flags);put(b,at+12,crc(crc(seed_crc,struct.pack('>I',seq)),value));at+=16
                if not i:b[at:at+16]=uuid;at+=16
                data.append(value)
            seal(b,bs-4,seed_crc);append(b)
            for value in data:append(value)
        b=header(bs,2,seq);seal(b,16,seed_crc);append(b)
    js=bytearray(js);put(js,24,0xffffffff);put(js,28,start)
    # Journal super checksum covers 1024 bytes, not the whole FS block.
    put(js,252,0);put(js,252,crc(0xffffffff,js[:1024]));journal_put(wrapped,jmap,bs,0,js)
    wrap=out/f'wrap-{bs}.img';wrap.write_bytes(wrapped);variants.append(wrap)
    records=[]
    for source in variants:
        linux_oracle=out/f'{source.stem}-linux-oracle.img';oracle(source,linux_oracle,out/f'{source.stem}-linux.log')
        # Linux journal_only also clears EXT4 RECOVER. The Phase-6 core leaves
        # that filesystem-level transition to Phase 8; compare home images
        # against the independently replayed oracle before that transition.
        home_oracle=out/f'{source.stem}-home-oracle.img'
        recovered=bytearray(linux_oracle.read_bytes())
        # e2fsck updates mount/write/check times, mount count and lifetime
        # writes while opening its recovery handle. They are tool housekeeping,
        # not journal replay results; retain the exact committed payload values.
        for offset,length in ((44,8),(52,2),(64,4),(376,8)):
            recovered[1024+offset:1024+offset+length]=sbimage[1024%bs+offset:1024%bs+offset+length]
        struct.pack_into('<I',recovered,1120,struct.unpack_from('<I',recovered,1120)[0]|4)
        sbseal(recovered);home_oracle.write_bytes(recovered)
        for ss in (512,4096):
            result=out/f'{source.stem}-{ss}-replayed.img'
            cmd=[str(ROOT/'build/jbd2_replay_host'),str(source),str(home_oracle),str(result),str(ss)]
            text=run(cmd);print(text,end='',flush=True)
            text+=run(['e2fsck','-fy','-E','journal_only',str(result)])+run(['e2fsck','-fn',str(result)])
            dumped=out/f'{source.stem}-{ss}.bin';run(['debugfs','-R',f'dump /target.bin {dumped}',str(result)])
            assert dumped.read_bytes()==escaped+b'C'*bs
            (out/f'{source.stem}-{ss}.log').write_text(text);records.append({'case':source.stem,'sector':ss,'argv':cmd,'sha256':hashlib.sha256(result.read_bytes()).hexdigest()})
    # Corrupt or unsupported structures must reject before any write.
    start=be(d[jmap[0]*bs:(jmap[0]+1)*bs],28)
    for name in ('super-csum','uuid','feature','descriptor-csum','payload-csum','tag-flags','tag-high','tag-uuid','journal-alias','outside-home','missing-last','geometry','external','orphan','commit-csum','sequence-gap'):
        bad=bytearray(d);super_off=jmap[0]*bs;desc_off=jmap[start]*bs
        if name=='super-csum':bad[super_off+252]^=1
        elif name in ('uuid','feature','geometry'):
            if name=='uuid':bad[super_off+48]^=1
            elif name=='feature':put(bad,super_off+40,0x13)
            else:put(bad,super_off+28,len(jmap))
            b=bytearray(bad[super_off:super_off+1024]);seal(b,252,0xffffffff);bad[super_off:super_off+1024]=b
        elif name in ('external','orphan'):
            struct.pack_into('<I',bad,1252 if name=='external' else 1256,1);sbseal(bad)
        elif name=='commit-csum':bad[jmap[start+5]*bs+16]^=1
        elif name=='sequence-gap':
            b=bytearray(bad[desc_off:desc_off+bs]);put(b,8,be(b,8)+1)
            seal(b,bs-4,seed_crc);bad[desc_off:desc_off+bs]=b
        elif name=='payload-csum':bad[jmap[start+1]*bs+100]^=1
        elif name=='descriptor-csum':bad[desc_off+bs-4]^=1
        else:
            b=bytearray(bad[desc_off:desc_off+bs])
            if name=='tag-flags':put(b,16,0x80)
            elif name=='tag-high':put(b,20,1)
            elif name=='tag-uuid':b[28]^=1
            elif name=='journal-alias':put(b,12,jmap[0])
            elif name=='outside-home':put(b,12,len(d)//bs)
            else:
                # Four tags: first 32 bytes (UUID), then three 16-byte tags.
                put(b,12+32+32+4,2)
            seal(b,bs-4,seed_crc);bad[desc_off:desc_off+bs]=b
        path=out/f'bad-{bs}-{name}.img';path.write_bytes(bad)
        text=run([str(ROOT/'build/jbd2_replay_host'),str(path),'reject','unused','512']);print(text,end='',flush=True)
    return records
def main():
    parent=ROOT/'build/jbd2-replay';parent.mkdir(exist_ok=True);out=Path(tempfile.mkdtemp(prefix='run-',dir=parent))
    (out/'versions.txt').write_text(run(['mke2fs','-V'])+run(['debugfs','-V']))
    records=[]
    for bs in (1024,2048,4096):records+=case(out,bs)
    (out/'manifest.json').write_text(json.dumps(records,indent=2)+'\n')
    print(f'JBD2 recovery host PASS; evidence: {out}; no writer/mount/physical crash guarantee.')
if __name__=='__main__':main()
