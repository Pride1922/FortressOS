/* Copied-source actual EXT4/JBD2 primitive workbench. No production caller.
 * A verified external enrollment pins all 16 real inode/maps. No origin sidecar.
 * Account-service BEGIN/UPDATE/redo phase integration is deliberately separate. */
#include "pkg_enrollment_candidate.h"
#include "../user/tools/digest.h"
static bool ad_enabled,ad_pair_staged,ad_capture_partial;
static pe_enrollment ad_en;
static uint8_t ad_descriptor[PE_MAX],ad_state[1024],ad_slot[1024],ad_body[PE_BODY];
static uint8_t *ad_partial;
static unsigned ad_previews,ad_preview_newer,ad_pairs,ad_partial_commits;
static ext4_fault_disk_t *ad_disk;
static const char *ad_paths[16]={
    "/mnt/pkg/enrollment.v1","/mnt/pkg/grant.v1","/mnt/pkg/ownership.v1","/mnt/pkg/transaction.v1",
    "/mnt/pkg/origin.v1","/mnt/pkg/account-generation.v1","/mnt/pkg/state.v1","/mnt/pkg/trust.v1",
    "/mnt/pkg","/mnt/etc","/mnt/var","/mnt/var/lib","/mnt/var/lib/fortress",
    "/mnt/var/lib/fortress/accounts","/mnt/usr/bin","/mnt/usr/share/pkg"
};
static uint32_t ad_ids[16];
static void ad_p64(uint8_t *p,uint64_t v){for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(v>>(8*i));}
static uint64_t ad_u64(const uint8_t *p){return e4_u32(p)|(uint64_t)e4_u32(p+4)<<32;}
static void ad_hash(const void *p,size_t n,uint8_t h[32]){
    digest_t c;digest_init(&c,DIGEST_SHA256);assert(digest_update(&c,p,n));digest_final(&c,h);
}
static void ad_seal(uint8_t *p){ad_hash(p,992,p+992);}
static int ad_path(ext4_mount_t *fs,const char *path,uint32_t *out){
    uint32_t ino=2;const char *p=path+5; /* fixed /mnt/... inputs only */
    while(*p){
        if(*p=='/')p++;
        char part[64];unsigned n=0;while(*p&&*p!='/'){if(n==63)return -VFS_EIO;part[n++]=*p++;}part[n]=0;
        e4_inode_t dir;int r=e4_inode(fs,ino,&dir);if(r)return r;
        uint32_t child=0;r=e4_scan(&dir,part,0,NULL,&child);if(r<0)return r;if(!child)return -VFS_ENOENT;ino=child;
    }
    *out=ino;return 0;
}
static const uint8_t *ad_locator(unsigned i){return ad_en.wire+416+128*i;}
static int ad_io(ext4_engine_t *e,unsigned i,uint8_t *bytes,bool stage,bool planned){
    const uint8_t *r=ad_locator(i);ext4_mount_t *fs=e->fs;
    if(ad_u64(r+16)!=1024)return -VFS_EIO;
    uint32_t block=(uint32_t)ad_u64(r+48);
    if(e4_u16(r+32)!=1||e4_u32(r+40)||e4_u32(r+44)!=1)return -VFS_EIO;
    if(stage){
        uint8_t *p=e4_stage(e,block,E4_REFERENCE);if(!p)return e->error;memcpy(p,bytes,1024);
    }else{
        if(planned)for(unsigned k=0;k<e->images;k++)if(e->image[k].block==block){memcpy(bytes,e->image[k].bytes,1024);return 0;}
        if(!e4_bytes(fs,(uint64_t)block*fs->bs,bytes,1024))return -VFS_EIO;
    }
    return 0;
}
static int ad_pair(ext4_engine_t *e,bool planned,pe_origin *origin){
    if(ad_io(e,5,ad_state,false,planned)||ad_io(e,4,ad_slot,false,planned))return -VFS_EIO;
    static const uint8_t magic[16]="FORT-ACC-GEN-v1";uint8_t hash[32];
    uint64_t seq=ad_u64(ad_state+24),gen=ad_u64(ad_state+32);
    if(memcmp(ad_state,magic,16)||e4_u32(ad_state+16)!=1024||e4_u16(ad_state+20)!=1||
       e4_u16(ad_state+22)>1||!seq||seq==UINT64_MAX||!gen||gen==UINT64_MAX||
       ad_u64(ad_state+40)!=ad_u64(ad_en.wire+24)||memcmp(ad_state+48,ad_en.wire+104,16)||
       memcmp(ad_state+64,ad_en.wire+40,16)||memcmp(ad_state+80,ad_en.wire+312,32))return -VFS_EIO;
    for(unsigned i=144;i<992;i++)if(ad_state[i])return -VFS_EIO;
    ad_hash(ad_state,992,hash);if(memcmp(hash,ad_state+992,32))return -VFS_EIO;
    ad_hash(ad_slot,1024,hash);if(memcmp(hash,ad_state+112,32))return -VFS_EIO;
    if(seq<gen||(e4_u16(ad_state+22)&&gen<2)||
       pe_origin_decode(ad_slot,1024,&ad_en,seq,gen,origin)||
       (origin->active&&(!e4_u16(ad_state+22)||origin->phase||gen<2)))return -VFS_EIO;
    return 0;
}
static void ad_empty(void){memset(ad_slot+112,0,880);e4_p16(ad_slot+22,0);}
static int ad_store(ext4_engine_t *e){
    uint64_t seq=ad_u64(ad_state+24);if(seq>=UINT64_MAX-1)return -VFS_EFBIG;
    ad_p64(ad_state+24,seq+1);ad_p64(ad_slot+24,seq+1);
    ad_p64(ad_slot+40,ad_u64(ad_state+32));ad_seal(ad_slot);
    ad_hash(ad_slot,1024,ad_state+112);ad_seal(ad_state);
    int r=ad_io(e,4,ad_slot,true,false);if(!r)r=ad_io(e,5,ad_state,true,false);
    if(!r)ad_pair_staged=true;
    return r;
}
static int ad_operation(ext4_engine_t *e,bool begin){
    pe_origin o;int r=ad_pair(e,false,&o);if(r)return r;
    if(o.active||e->orphan_count||e4_u16(ad_state+22)!=(begin?0:1))return -VFS_EAGAIN;
    uint64_t gen=ad_u64(ad_state+32);
    if(begin&&(gen>=UINT64_MAX-1||ad_u64(ad_state+24)>UINT64_MAX-5))return -VFS_EFBIG;
    r=e4_namespace_begin(e);if(r)return r;
    e4_p16(ad_state+22,begin?1:0);if(begin)ad_p64(ad_state+32,gen+1);
    r=ad_store(e);return e4_namespace_end(e,r);
}
static int ad_activate(struct ext4_engine *e,uint32_t ino,uint32_t parent){
    if(!ad_enabled)return 0;
    /* Trusted dispatcher permits only canonical shadow in this primitive gate. */
    uint32_t actual;pe_origin o;int r=ad_pair(e,false,&o);
    if(r)return r;
    if(o.active||e->orphan_count||e4_u16(ad_state+22)!=1||
       ad_path(e->fs,"/mnt/etc/shadow",&actual)||actual!=ino||parent!=e4_u32(ad_locator(9)+4))return -VFS_EAGAIN;
    if(ad_u64(ad_state+24)>UINT64_MAX-4)return -VFS_EFBIG;
    e4_inode_t in;if(e4_inode(e->fs,ino,&in)||in.uid||in.gid||in.full_mode!=(0x8000|0600)||in.size>8192)return -VFS_EIO;
    e->fs->rw_ino=0;if(e4_mapping(&in)||in.map->count>16||e->fs->physical_count>32)return -VFS_EIO;
    ad_empty();e4_p16(ad_slot+22,1);e4_p16(ad_slot+112,2); /* ACCOUNT_SHADOW; pre-intent */
    e4_p32(ad_slot+116,ino);e4_p32(ad_slot+120,in.generation);
    e4_p32(ad_slot+124,parent);e4_p32(ad_slot+128,e4_u32(ad_locator(9)+8));
    e4_p32(ad_slot+132,0600);ad_p64(ad_slot+144,in.size);e4_p16(ad_slot+152,(uint16_t)in.map->count);
    for(unsigned i=0;i<in.map->count;i++){
        e4_extent_t x=in.map->entries[i];if(x.unwritten)return -VFS_EIO;
        uint8_t *p=ad_slot+160+16*i;e4_p32(p,x.logical);e4_p32(p+4,x.len);ad_p64(p+8,x.physical);
    }
    /* Sort and coalesce physical ranges; e4_tree also includes metadata. */
    e4_range_t ranges[32];unsigned count=e->fs->physical_count;
    memcpy(ranges,e->fs->physical,count*sizeof(*ranges));
    for(unsigned i=1;i<count;i++)for(unsigned j=i;j&&ranges[j-1].lo>ranges[j].lo;j--){
        e4_range_t t=ranges[j];ranges[j]=ranges[j-1];ranges[j-1]=t;
    }
    unsigned n=0;for(unsigned i=0;i<count;i++){
        if(n&&(uint64_t)ranges[n-1].lo+ranges[n-1].len==ranges[i].lo)ranges[n-1].len+=ranges[i].len;
        else ranges[n++]=ranges[i];
    }
    e4_p16(ad_slot+154,(uint16_t)n);
    for(unsigned i=0;i<n;i++){ad_p64(ad_slot+416+16*i,ranges[i].lo);e4_p32(ad_slot+424+16*i,ranges[i].len);}
    return ad_store(e);
}
static int ad_retire(struct ext4_engine *e,uint32_t ino){
    if(!ad_enabled)return 0;
    pe_origin o;int r=ad_pair(e,false,&o);if(r)return r;
    if(!o.active||o.inode!=ino||e->orphan_count!=1||e->orphans[0]!=ino)return -VFS_EIO;
    ad_empty();return ad_store(e);
}
static int e4_semantic_commit(struct ext4_engine *e){
    if(!ad_enabled)return ad_core_commit(e);
    pe_origin o;int r=ad_pair(e,true,&o);
    if(!r){
        unsigned found=0;
        for(unsigned i=0;i<e->images;i++)for(unsigned k=4;k<=5;k++)
            if(e->image[i].block==ad_u64(ad_locator(k)+48))found++;
        if((ad_pair_staged&&found!=2)||(!ad_pair_staged&&found))r=-VFS_EROFS;
    }
    if(!r)r=ad_core_commit(e);
    if(!r&&ad_pair_staged)ad_pairs++;
    ad_pair_staged=false;
    if(!r&&ad_capture_partial&&o.active){
        e4_inode_t in;e->fs->orphan_access=o.inode;
        int ir=e4_inode(e->fs,o.inode,&in);e->fs->orphan_access=0;
        if(!ir){
            uint32_t g=(o.inode-1)/e->fs->ipg,b=(o.inode-1)%e->fs->ipg;uint8_t raw[256];
            assert(e4_bytes(e->fs,(uint64_t)e4_u32(e->fs->gd[g].raw+8)*e->fs->bs+b*256,raw,256));
            if(!e4_u32(raw+28)){
                ext4_fault_disk_t *d=e->fs->dev->priv;memcpy(ad_partial,d->stable,d->bytes);ad_partial_commits++;
            }
        }
    }
    return r;
}
static int ad_validate(struct ext4_engine *e){
    if(!ad_enabled)return 0;
    ext4_mount_t *fs=e->fs;static uint8_t sb[1024];uint8_t raw[256];
    if(fs->bs!=e4_u32(ad_en.wire+36)||fs->blocks!=ad_u64(ad_en.wire+344)||
       fs->inodes!=e4_u32(ad_en.wire+352)||!e4_bytes(fs,1024,sb,1024)||memcmp(sb+104,ad_en.wire+40,16))return -VFS_EIO;
    for(unsigned i=0;i<16;i++){
        const uint8_t *r=ad_locator(i);uint32_t ino;e4_inode_t in;
        if(ad_path(fs,ad_paths[i],&ino)||ino!=e4_u32(r+4)||e4_inode(fs,ino,&in)||
           in.generation!=e4_u32(r+8)||in.size!=ad_u64(r+16)||in.uid||in.gid||
           in.full_mode!=((e4_u16(r+2)==2?0x4000:0x8000)|e4_u32(r+12)))return -VFS_EIO;
        uint32_t g=(ino-1)/fs->ipg,b=(ino-1)%fs->ipg;
        if(!e4_bytes(fs,(uint64_t)e4_u32(fs->gd[g].raw+8)*fs->bs+b*256,raw,256)||
           (in.mode==0x8000&&e4_u16(raw+26)!=1))return -VFS_EIO;
        fs->rw_ino=0;if(e4_mapping(&in)||in.map->count!=e4_u16(r+32)||fs->physical_count!=in.map->count)return -VFS_EIO;
        uint64_t total=0;
        for(unsigned j=0;j<in.map->count;j++){
            e4_extent_t x=in.map->entries[j];const uint8_t *p=r+40+16*j;
            if(x.unwritten||x.logical!=e4_u32(p)||x.len!=e4_u32(p+4)||x.physical!=ad_u64(p+8))return -VFS_EIO;
            total+=x.len;
        }
        if(total!=ad_u64(r+24)||total*(fs->bs/512)!=e4_u32(raw+28))return -VFS_EIO;
    }
    pe_origin o;if(ad_pair(e,false,&o))return -VFS_EIO;
    /* Four remaining pair transitions must fit before an operation is admitted.
     * This conservative primitive profile rejects near-exhausted roots before
     * replay/dirty marking rather than stranding an ACTIVE slot later. */
    if(ad_u64(ad_state+24)>UINT64_MAX-5)return -VFS_EFBIG;
    uint8_t marker_hash[32];
    if(!e4_bytes(fs,ad_u64(ad_locator(0)+48)*fs->bs,sb,512))return -VFS_EIO;
    ad_hash(sb,512,marker_hash);if(memcmp(marker_hash,ad_en.wire+120,32))return -VFS_EIO;
    uint32_t named=0;int lookup=ad_path(fs,"/mnt/etc/shadow",&named);
    if(lookup&&lookup!=-VFS_ENOENT)return -VFS_EIO;
    if(!o.active)return e->orphan_count?-VFS_EIO:0;
    if(o.role!=2||named||e->orphan_count!=1||e->orphans[0]!=o.inode)return -VFS_EIO;
    e4_inode_t in;fs->orphan_access=o.inode;int r=e4_inode(fs,o.inode,&in);fs->orphan_access=0;
    if(r||in.generation!=o.generation||in.size!=o.size||in.full_mode!=(0x8000|0600)||in.uid||in.gid)return -VFS_EIO;
    uint32_t g=(o.inode-1)/fs->ipg,b=(o.inode-1)%fs->ipg;
    if(!e4_bytes(fs,(uint64_t)e4_u32(fs->gd[g].raw+8)*fs->bs+b*256,raw,256)||e4_u16(raw+26))return -VFS_EIO;
    fs->rw_ino=0;if(e4_mapping(&in)||in.map->count>o.extents)return -VFS_EIO;
    uint64_t total=0,logical=0;
    for(unsigned i=0;i<in.map->count;i++){
        e4_extent_t x=in.map->entries[i];const uint8_t *p=ad_slot+160+16*i;
        if(x.unwritten||x.logical!=logical||x.logical!=e4_u32(p)||x.physical!=ad_u64(p+8)||x.len>e4_u32(p+4))return -VFS_EIO;
        logical+=x.len;
    }
    for(unsigned i=0;i<fs->physical_count;i++){
        e4_range_t x=fs->physical[i];unsigned found=0;
        for(unsigned j=0;j<o.ranges;j++){
            const uint8_t *p=ad_slot+416+16*j;
            if(x.lo>=ad_u64(p)&&(uint64_t)x.lo+x.len<=ad_u64(p)+e4_u32(p+8))found++;
        }
        if(found!=1)return -VFS_EIO;
        total+=x.len;
    }
    return total*(fs->bs/512)==e4_u32(raw+28)?0:-VFS_EIO;
}
static int ad_preview(struct ext4_engine *e){
    if(!ad_enabled)return 0;
    ad_previews++;int r=ad_validate(e);
    if(!r){
        uint64_t offset=ad_u64(ad_locator(5)+48)*e->fs->bs;
        if(ad_u64(ad_disk->stable+offset+24)!=ad_u64(ad_state+24))ad_preview_newer++;
    }
    return r;
}
static void ad_write(const char *path,const uint8_t *p,size_t n){
    file_t *f=vfs_open(path,VFS_O_CREAT|VFS_O_RDWR);assert(f);
    assert(vfs_write(f,p,n)==(int64_t)n);assert(!vfs_close(f));
    creds_t root;creds_init_root(&root);assert(!vfs_chmod_creds(path,0600,&root));
}
static void ad_initialize(ext4_mount_t *fs){
    const char *dirs[]={"/mnt/pkg","/mnt/pkg/transaction.v1","/mnt/etc","/mnt/var","/mnt/var/lib",
        "/mnt/var/lib/fortress","/mnt/var/lib/fortress/accounts","/mnt/usr","/mnt/usr/bin","/mnt/usr/share","/mnt/usr/share/pkg"};
    for(unsigned i=0;i<sizeof(dirs)/sizeof(*dirs);i++)assert(!vfs_mkdir(dirs[i],0700));
    static uint8_t bytes[8192];
    for(unsigned i=0;i<8;i++)if(i!=3)ad_write(ad_paths[i],bytes,i==0?512:i==2?8192:1024);
    memset(bytes,'S',sizeof bytes);ad_write("/mnt/etc/shadow",bytes,sizeof bytes);
    memset(ad_body,0,sizeof ad_body);memcpy(ad_body,"FORT-PKG-ENR-v2",16);
    e4_p32(ad_body+16,PE_BODY+192);e4_p16(ad_body+20,2);e4_p16(ad_body+22,2);
    ad_p64(ad_body+24,5);e4_p32(ad_body+32,2);e4_p32(ad_body+36,fs->bs);
    static uint8_t sb[1024];assert(e4_bytes(fs,1024,sb,1024));memcpy(ad_body+40,sb+104,16);
    for(unsigned i=1;i<5;i++)memset(ad_body+40+16*i,(int)i+1,16);
    for(unsigned i=0;i<7;i++)memset(ad_body+120+32*i,(int)i+1,32);
    uint8_t marker[512]={0};ad_hash(marker,512,ad_body+120);
    ad_p64(ad_body+344,fs->blocks);e4_p32(ad_body+352,fs->inodes);e4_p16(ad_body+356,16);e4_p16(ad_body+358,11);
    e4_p32(ad_body+360,8192);e4_p16(ad_body+364,16);e4_p16(ad_body+366,32);e4_p32(ad_body+368,64);e4_p32(ad_body+372,1);
    for(unsigned i=0;i<16;i++){
        uint32_t ino;assert(!ad_path(fs,ad_paths[i],&ino));ad_ids[i]=ino;
        e4_inode_t in;assert(!e4_inode(fs,ino,&in));fs->rw_ino=0;assert(!e4_mapping(&in)&&in.map->count<=4);
        uint8_t *r=ad_body+416+128*i;e4_p16(r,(uint16_t)(i+1));e4_p16(r+2,in.mode==0x4000?2:1);
        e4_p32(r+4,ino);e4_p32(r+8,in.generation);e4_p32(r+12,in.full_mode&07777);ad_p64(r+16,in.size);
        uint64_t blocks=0;e4_p16(r+32,(uint16_t)in.map->count);
        for(unsigned j=0;j<in.map->count;j++){
            e4_extent_t x=in.map->entries[j];e4_p32(r+40+16*j,x.logical);e4_p32(r+44+16*j,x.len);ad_p64(r+48+16*j,x.physical);blocks+=x.len;
        }
        ad_p64(r+24,blocks);
    }
    memset(ad_slot,0,1024);memcpy(ad_slot,"FORT-ACC-ORG-v1",16);e4_p32(ad_slot+16,1024);e4_p16(ad_slot+20,1);
    ad_p64(ad_slot+24,1);ad_p64(ad_slot+32,5);ad_p64(ad_slot+40,1);
    memcpy(ad_slot+48,ad_body+104,16);memcpy(ad_slot+64,ad_body+40,16);memcpy(ad_slot+80,ad_body+312,32);ad_seal(ad_slot);
    memset(ad_state,0,1024);memcpy(ad_state,"FORT-ACC-GEN-v1",16);e4_p32(ad_state+16,1024);e4_p16(ad_state+20,1);
    ad_p64(ad_state+24,1);ad_p64(ad_state+32,1);ad_p64(ad_state+40,5);
    memcpy(ad_state+48,ad_body+104,16);memcpy(ad_state+64,ad_body+40,16);memcpy(ad_state+80,ad_body+312,32);
    ad_hash(ad_slot,1024,ad_state+112);ad_seal(ad_state);
    ad_write(ad_paths[4],ad_slot,1024);ad_write(ad_paths[5],ad_state,1024);assert(!ext4_sync(fs));
}
static void ad_auth(const char *descriptor,const char *rules){
    size_t n,rn;uint8_t *p=load(descriptor,&n),*r=load(rules,&rn);assert(n<=PE_MAX&&rn==384);
    memcpy(ad_descriptor,p,n);pe_policy policy={0};memcpy(policy.keys,r,256);memcpy(policy.uuid,r+256,80);
    memcpy(policy.descriptor_sha256,r+336,32);policy.key_count=e4_u32(r+368);policy.threshold=e4_u32(r+372);policy.epoch=ad_u64(r+376);
    assert(!pe_verify(ad_descriptor,n,&policy,&ad_en));free(p);free(r);ad_enabled=true;
}
static void ad_mount(block_dev_t *dev,ext4_mount_t **fs){
    const char *stage=NULL;int r=e4_mount_journal_admitted(dev,"/mnt",true,fs,&stage,false);
    if(r)fprintf(stderr,"AD MOUNT stage=%s error=%d\n",stage,r);
    assert(!r);uint64_t irq=spin_lock_irqsave(&e4_lock);assert(!ad_validate((*fs)->engine));spin_unlock_irqrestore(&e4_lock,irq);
}
static int ad_op(ext4_mount_t *fs,bool begin){
    uint64_t irq=spin_lock_irqsave(&e4_lock);int r=ad_operation(fs->engine,begin);spin_unlock_irqrestore(&e4_lock,irq);return r;
}
static void ad_protect(ext4_mount_t *fs){
    for(unsigned k=4;k<=5;k++){
        ext4_fault_disk_t *d=fs->dev->priv;size_t events=d->events;
        pe_origin o;uint64_t irq=spin_lock_irqsave(&e4_lock);assert(!ad_pair(fs->engine,false,&o));spin_unlock_irqrestore(&e4_lock,irq);
        file_t *f=vfs_open(ad_paths[k],VFS_O_RDWR);assert(f);
        assert(vfs_write(f,k==4?ad_slot:ad_state,1024)<0);assert(!vfs_close(f));
        assert(!fs->engine->tainted&&d->events==events);
        assert(vfs_unlink(ad_paths[k])<0&&d->events==events);
    }
}
static void ad_finish(ext4_mount_t *fs){
    pe_origin o;uint64_t irq=spin_lock_irqsave(&e4_lock);assert(!ad_pair(fs->engine,false,&o));spin_unlock_irqrestore(&e4_lock,irq);
    if(e4_u16(ad_state+22)){
        vfs_node_t *n=vfs_lookup("/mnt/etc/shadow");if(n)assert(!vfs_unlink("/mnt/etc/shadow"));
        assert(!ad_op(fs,false));
    }
    assert(!ext4_freeze_and_sync(fs));
}
int main(int argc,char **argv){
    assert(argc==5||argc==7||argc==8);setvbuf(stdout,NULL,_IONBF,0);
    bool setup=!strcmp(argv[1],"setup"),recover=!strcmp(argv[1],"recover"),reject=!strcmp(argv[1],"reject"),exhaust=!strcmp(argv[1],"exhaust");assert(setup||recover||reject||exhaust||!strcmp(argv[1],"campaign"));
    size_t size;uint8_t *initial=load(argv[2],&size);unsigned ss=(unsigned)strtoul(argv[3],NULL,10);
    const char *prefix=argv[argc==5?4:6];int selected=argc==8?atoi(argv[7]):-1;assert(selected>=-1&&selected<8);
    bool retain=selected<=0;
    ext4_fault_disk_t d={.stable=malloc(size),.volatile_bytes=malloc(size),.bytes=size,.cut=-1};assert(d.stable&&d.volatile_bytes);
    block_dev_t dev={.sector_size=ss,.sector_count=size/ss,.read_sector=ext4_fault_read,.write_sector=ext4_fault_write,.flush=ext4_fault_flush,.priv=&d};
    ad_disk=&d;
    ext4_mount_t *fs;reset(&d,initial);
    if(setup){
        assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));ad_initialize(fs);assert(!ext4_freeze_and_sync(fs));
        save(prefix,"setup",d.stable,size);save(prefix,"body",ad_body,PE_BODY);
    }else{
        ad_auth(argv[4],argv[5]);
        if(reject){
            const char *stage=NULL;fs=NULL;int r=e4_mount_journal_admitted(&dev,"/mnt",true,&fs,&stage,false);
            assert(r&&!fs&&!e4_active&&!e4_engine_busy&&!d.events&&!d.writes&&!d.flushes&&!memcmp(initial,d.stable,size));
            assert(stage&&!strcmp(stage,"account-provenance-preview"));
            printf("PASS reject stage=%s writes=%zu flushes=%zu\n",stage,d.writes,d.flushes);
        }else if(recover){ad_mount(&dev,&fs);ad_finish(fs);save(prefix,"final",d.stable,size);printf("PASS fresh recovery previews=%u pairs=%u\n",ad_previews,ad_pairs);}
        else if(exhaust){
            ad_mount(&dev,&fs);assert(ad_u64(ad_state+24)==UINT64_MAX-5&&ad_u64(ad_state+32)==1);
            assert(!ad_op(fs,true));assert(!vfs_unlink("/mnt/etc/shadow"));assert(!ad_op(fs,false));
            assert(ad_u64(ad_state+24)==UINT64_MAX-1&&ad_u64(ad_state+32)==2);
            size_t events=d.events,writes=d.writes,flushes=d.flushes;
            assert(ad_op(fs,true)==-VFS_EFBIG&&d.events==events&&d.writes==writes&&d.flushes==flushes);
            assert(!ext4_freeze_and_sync(fs));save(prefix,"final",d.stable,size);
            printf("PASS exhaustion next-BEGIN=EFBIG writes=0 flushes=0\n");
        }
        else{
            ad_mount(&dev,&fs);
            ad_protect(fs);
            uint8_t *base=malloc(size),*begun=malloc(size),*orphan=malloc(size),*empty=malloc(size);ad_partial=malloc(size);
            assert(base&&begun&&orphan&&empty&&ad_partial);memcpy(base,d.stable,size);
            d.events=0;assert(!ad_op(fs,true));size_t begin_events=d.events;memcpy(begun,d.stable,size);
            size_t guarded=d.events;assert(ad_op(fs,true)==-VFS_EAGAIN&&d.events==guarded);
            file_t *held=vfs_open("/mnt/etc/shadow",VFS_O_RDONLY);assert(held);
            d.events=0;assert(!vfs_unlink("/mnt/etc/shadow"));size_t unlink_events=d.events;memcpy(orphan,d.stable,size);
            assert(e4_u16(ad_slot+22)==1);if(retain)save(prefix,"orphan",orphan,size);
            guarded=d.events;assert(ad_op(fs,true)==-VFS_EAGAIN&&ad_op(fs,false)==-VFS_EAGAIN&&d.events==guarded);
            ad_capture_partial=true;d.events=0;assert(!vfs_close(held));size_t close_events=d.events;ad_capture_partial=false;
            assert(ad_partial_commits==1);if(retain)save(prefix,"partial",ad_partial,size);memcpy(empty,d.stable,size);
            d.events=0;assert(!ad_op(fs,false));size_t end_events=d.events;
            assert(!ext4_freeze_and_sync(fs));if(retain)save(prefix,"normal",d.stable,size);
            reset(&d,orphan);d.events=0;ad_mount(&dev,&fs);size_t recovery_events=d.events;ad_finish(fs);
            size_t totals[]={begin_events,unlink_events,close_events,recovery_events,end_events};unsigned cuts=0;
            for(unsigned op=0;op<5;op++){
                for(unsigned persistence=0;persistence<4;persistence++)for(unsigned after=0;after<2;after++)for(size_t cut=0;cut<totals[op];cut++){
                    if(selected>=0&&(int)(persistence*2+after)!=selected)continue;
                    reset(&d,op==0?base:op==1?begun:op==2||op==3?orphan:empty);ad_pair_staged=false;held=NULL;
                    if(op!=3){ad_mount(&dev,&fs);if(op==1){held=vfs_open("/mnt/etc/shadow",VFS_O_RDONLY);assert(held);}}
                    /* close cuts start from a pinned orphan, before constructor cleanup. */
                    if(op==2){
                        reset(&d,begun);ad_mount(&dev,&fs);held=vfs_open("/mnt/etc/shadow",VFS_O_RDONLY);assert(held);assert(!vfs_unlink("/mnt/etc/shadow"));
                    }
                    d.events=d.writes=d.flushes=0;d.persistence=persistence;d.after=after!=0;d.cut=(long)cut;
                    int r;
                    if(op==0)r=ad_op(fs,true);
                    else if(op==1)r=vfs_unlink("/mnt/etc/shadow");
                    else if(op==2){assert(!vfs_close(held));held=NULL;r=fs->engine->tainted?-VFS_EIO:0;}
                    else if(op==3){const char *stage=NULL;fs=NULL;r=e4_mount_journal_admitted(&dev,"/mnt",true,&fs,&stage,false);assert(!fs&&!e4_active);}
                    else r=ad_op(fs,false);
                    if(r!=-VFS_EIO)fprintf(stderr,"AD CUT op=%u persistence=%u after=%u cut=%zu total=%zu error=%d\n",op,persistence,after,cut,totals[op],r);
                    assert(r==-VFS_EIO);if(held)assert(!vfs_close(held));ad_pair_staged=false;
                    uint64_t seq=0,gen=0;
                    for(unsigned pass=0;pass<2;pass++){
                        teardown();ext4_fault_restart(&d);vfs_init();ad_mount(&dev,&fs);ad_finish(fs);
                        pe_origin o;uint64_t irq=spin_lock_irqsave(&e4_lock);assert(!ad_pair(fs->engine,false,&o)&&!o.active&&!e4_u16(ad_state+22));spin_unlock_irqrestore(&e4_lock,irq);
                        uint64_t s=ad_u64(ad_state+24),g=ad_u64(ad_state+32);assert(g==(op?2:g)&&(g==1||g==2));
                        if(pass)assert(s==seq&&g==gen);
                        seq=s;gen=g;
                    }
                    cuts++;
                }
                printf("AD op=%u events=%zu cuts=%zu\n",op,totals[op],totals[op]*(selected<0?8:1));
            }
            save(prefix,"recovered",d.stable,size);
            assert(ad_preview_newer);
            printf("PASS durable cuts=%u previews=%u pairs=%u partial=%u newer=%u protections=7 two-restarts-per-cut\n",cuts,ad_previews,ad_pairs,ad_partial_commits,ad_preview_newer);
            free(base);free(begun);free(orphan);free(empty);free(ad_partial);ad_partial=NULL;
        }
    }
    ad_enabled=false;teardown();free(initial);free(d.stable);free(d.volatile_bytes);return 0;
}
