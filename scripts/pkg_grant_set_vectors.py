"""Independent Python record construction for the private C cross-binding gate."""
import hashlib
import struct


def generate(out, fixture, seal, put):
    paths=[]
    def digest(b): return hashlib.sha256(b).digest()
    def context(m):
        return bytes(m[32:112]+m[112:144])+digest(m)+bytes(m[24:32]+m[144:168])
    def seal_marker(m): m[480:]=digest(m[:480])
    def link(l,t):
        t[8160:]=digest(t[:8160]); l[264:296]=digest(t); seal(l)
    def entry(t,index,role,cls,inode,blocks=0):
        p=256+64*index
        struct.pack_into('<HHIII',t,p,role,cls,inode,1,0 if cls==3 else 1)
        t[p+16:p+32]=b'T'*16
        if cls!=3:
            struct.pack_into('<QQQ',t,p+32,64,blocks,1)
    def base(phase=2,entries=None):
        m=bytearray(512); m[:16]=b'FORT-PKG-ENR-v1\0'
        put(m,16,512,'I'); put(m,20,1,'H'); put(m,24,1)
        for i in range(5): m[32+16*i:48+16*i]=bytes([i+1])*16
        m[112:144]=b'P'*32
        for i in range(3): struct.pack_into('<II',m,144+8*i,44+i,1)
        put(m,168,2,'I'); put(m,172,1,'I'); seal_marker(m)
        l=fixture(phase)
        l[56:72]=m[96:112]; l[72:88]=m[32:48]
        l[88:104]=m[80:96]; l[104:136]=m[112:144]
        put(l,352,46,'I')
        t=bytearray(8192); t[:16]=b'FORT-PKG-OWN-v1\0'
        put(t,16,8192,'I'); put(t,20,1,'H'); put(t,24,1)
        t[32:48]=l[40:56]; t[48:56]=l[24:32]
        t[56:72]=m[96:112]; t[72:88]=m[32:48]
        t[88:104]=m[80:96]; t[104:136]=m[112:144]
        if entries is None: entries=[] if not phase else [(1,1,100,2),(2,1,101,3),(21,2,102,4),(22,3,103,0)]
        put(t,22,len(entries),'H')
        for i,args in enumerate(entries): entry(t,i,*args)
        q=[sum(e[3] for e in entries if e[1]==1),sum(e[1]==1 for e in entries)]
        for i in range(2):
            put(l,328+8*i,q[i]); put(l,296+8*i,(4 if phase else 0)+q[i])
        link(l,t)
        return m,l,t
    def save(name,m,l,t,e=None):
        e=context(m) if e is None else e
        assert len(e)==176
        # Prefix filename classification remains compatible with C harness.
        p=out/(name+'.set')
        p.write_bytes(struct.pack('<III',len(m),len(l),len(t))+e+m+l+t)
        paths.append(str(p))
    for phase in range(4): save('ok-set-phase-'+str(phase),*base(phase))
    save('ok-set-empty-reservation',*base(1,[]))
    save('ok-set-role-boundaries',*base(2,[(16,1,100,0),(17,1,101,2),(18,1,102,1),
          (19,1,103,1),(20,1,104,3),(21,2,105,3),(36,3,106,0)]))
    save('ok-set-full-private-profile',*base(2,[(r,1 if r<=20 else 2,100+r,1) for r in range(1,37)]))
    m,l,t=base(); put(t,268,2,'I'); link(l,t); save('ok-set-orphan-flag',m,l,t)
    # Marker malformed fields are rehashed AND re-pinned, so checksum or pin
    # mismatch cannot mask missing structural validation.
    for offset,value,fmt in [(16,511,'I'),(20,2,'H'),(22,1,'H'),(24,0,'Q'),
            (144,0,'I'),(148,0,'I'),(152,44,'I'),(168,1,'I'),(172,2,'I'),
            (176,1,'B'),(479,1,'B')]:
        m,l,t=base(); put(m,offset,value,fmt); seal_marker(m)
        save('bad-set-marker-'+str(offset),m,l,t)
    for offset in (32,48,64,80,96,112):
        m,l,t=base(); m[offset:offset+(32 if offset==112 else 16)]=bytes(32 if offset==112 else 16)
        seal_marker(m); save('bad-set-marker-zero-'+str(offset),m,l,t)
    # External expectation mismatch, with valid records unchanged.
    for offset in (0,16,32,48,64,80,112,144,152,156,160,164,168,172):
        m,l,t=base(); e=bytearray(context(m)); e[offset]^=1
        save('bad-set-expectation-'+str(offset),m,l,t,e)
    for offset in (24,32,56,72,88,104,352,356):
        m,l,t=base(); l[offset]^=1; seal(l)
        save('bad-set-ledger-binding-'+str(offset),m,l,t)
    # Recompute both hashes after table mutations to isolate semantic checks.
    mutations=[(16,8191,'I'),(20,2,'H'),(22,65,'H'),(24,2,'Q'),(48,1,'Q'),
        (136,1,'B'),(255,1,'B'),(4352,1,'B'),(8159,1,'B'),
        (256,0,'H'),(256,37,'H'),(258,0,'H'),(258,4,'H'),(258,2,'H'),
        (260,0,'I'),(260,44,'I'),(260,45,'I'),(260,46,'I'),(264,0,'I'),
        (268,0,'I'),(268,3,'I'),(304,0,'Q'),(304,2,'Q'),(312,1,'B'),
        (320,1,'H'),(324,100,'I'),(256+64*2,20,'H'),
        (256+64*3+12,1,'I'),(256+64*3+32,1,'Q'),
        (256+64*3+40,1,'Q'),(256+64*3+48,1,'Q'),(512,1,'B')]
    for i,(offset,value,fmt) in enumerate(mutations):
        m,l,t=base(); put(t,offset,value,fmt); link(l,t)
        save('bad-set-table-semantic-'+str(i),m,l,t)
    for offset in (32,56,72,88,104,272):
        m,l,t=base(); t[offset]^=1; link(l,t)
        save('bad-set-table-binding-'+str(offset),m,l,t)
    for offset in (328,336):
        m,l,t=base(); put(l,offset,0)
        put(l,296+(offset-328),4); seal(l)
        save('bad-set-sum-'+str(offset),m,l,t)
    # Every unused slot and each byte of the table's reserved header/tail.
    # Rehashing isolates the zero-padding rules from integrity checks.
    for offset in list(range(136,256))+list(range(4352,8160))+list(range(512,4352,64)):
        m,l,t=base(); t[offset]=1; link(l,t)
        save('bad-set-padding-'+str(offset),m,l,t)
    m,l,t=base(); put(t,296,2**64-1); put(t,360,1); link(l,t)
    save('bad-set-sum-overflow',m,l,t)
    m,l,t=base(0,[(1,3,100,0)]); save('bad-set-idle-tags',m,l,t)
    for component in range(3):
        for length in (0,1,(512,1024,8192)[component]-1,(512,1024,8192)[component]+1):
            records=list(base()); e=context(records[0]); b=records[component]
            records[component]=b[:length] if length<=len(b) else b+b'x'
            save('bad-set-length-%d-%d'%(component,length),*records,e=e)
        for offset in (0,15,(480,992,8160)[component],(511,1023,8191)[component]):
            records=list(base()); e=context(records[0]); records[component][offset]^=1
            save('bad-set-corruption-%d-%d'%(component,offset),*records,e=e)
    return paths
