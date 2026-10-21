/* Private actual EXT4/JBD2 adapter. No production hooks or grant provisioning. */
#define TEST_PERMISSIONS_ENFORCEMENT
#define main previous_mount_main
#include "ext4_mount_host.c"
#undef main
#include "pkg_grant_candidate.h"
#include "../user/tools/digest.h"
static uint8_t marker[512],ledger[1024],table[8192],payload[4096];
static pg_binding binding;
static const char *paths[]={"/mnt/pkg/enrollment.v1","/mnt/pkg/grant.v1",
    "/mnt/pkg/ownership.v1","/mnt/pkg/transaction.v1","/mnt/pkg/payload"};
static uint32_t ids[5];
static uint32_t marker_ino,marker_generation;
/* This private resource profile requires the existing recovery bootstrap's
 * corroborating group-1 identities. Single-group filesystems fail closed. */
static int eligible(ext4_mount_t *fs){
    uint8_t sb[1024],descriptor[32];
    if(fs->groups<2 || !e4_bytes(fs,1024,sb,sizeof sb) || !e4_journal_backup(fs,sb))return 0;
    for(uint32_t g=0;g<fs->groups;g++){
        memcpy(descriptor,fs->gd[g].raw,32);
        if(!e4_journal_descriptor_backup(fs,g,descriptor))return 0;
    }
    return 1;
}
static void p64(uint8_t *p,uint64_t v){for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(v>>(8*i));}
static uint64_t u64(const uint8_t *p){return e4_u32(p)|(uint64_t)e4_u32(p+4)<<32;}
static void hash(const void *p,size_t n,uint8_t h[32]){
    digest_t d;digest_init(&d,DIGEST_SHA256);assert(digest_update(&d,p,n));digest_final(&d,h);
}
static void seal_records(void){
    hash(table,8160,table+8160);hash(table,8192,ledger+264);hash(ledger,992,ledger+992);
}
static uint8_t *raw_inode(ext4_mount_t *fs,uint32_t ino){
    static uint8_t raw[256];uint32_t g=(ino-1)/fs->ipg,b=(ino-1)%fs->ipg;
    if(!e4_bytes(fs,(uint64_t)e4_u32(fs->gd[g].raw+8)*fs->bs+b*256,raw,256))return NULL;
    return raw;
}
/* Always traverse afresh: physical[] includes external extent-tree nodes too. */
static int object(ext4_mount_t *fs,uint32_t ino,uint32_t gen,uint64_t size,
                  bool directory,bool fixed,uint64_t *allocated){
    e4_inode_t in;int r=e4_inode(fs,ino,&in);if(r)return r;
    if(in.generation!=gen || in.size!=size || in.uid || in.gid ||
       in.full_mode!=((directory?0x4000:0x8000)|(directory?0700:0600)))return -1;
    uint8_t *raw=raw_inode(fs,ino);if(!raw || e4_u16(raw+26)!=(directory?2:1))return -1;
    uint32_t sectors=e4_u32(raw+28);
    fs->rw_ino=0;if(e4_mapping(&in))return -1;
    uint64_t count=0,next=0;
    for(unsigned i=0;i<fs->physical_count;i++)count+=fs->physical[i].len;
    for(unsigned i=0;i<in.map->count;i++){
        e4_extent_t x=in.map->entries[i];
        if(x.unwritten || x.logical!=next)return -1;
        next+=x.len;
    }
    if(next!=(size+fs->bs-1)/fs->bs || count*(fs->bs/512)!=sectors)return -1;
    if(fixed && (e4_u16(in.extent+6) || in.map->count>4 || count!=next))return -1;
    *allocated=count;return 0;
}
static int record_io(ext4_mount_t *fs,unsigned index,uint8_t *bytes,size_t n,bool stage){
    e4_inode_t in;if(e4_inode(fs,ids[index],&in) || in.size!=n)return -1;
    fs->rw_ino=0;if(e4_mapping(&in))return -1;
    /* Capture map first: staging reads may reuse filesystem scratch. */
    uint32_t blocks[8];unsigned count=(unsigned)((n+fs->bs-1)/fs->bs);
    if(count>8)return -1;
    for(unsigned i=0;i<count;i++){
        const e4_extent_t *x=e4_find(&in,i);if(!x||x->unwritten)return -1;
        blocks[i]=x->physical+i-x->logical;
    }
    for(unsigned i=0;i<count;i++){
        size_t take=n-(size_t)i*fs->bs;if(take>fs->bs)take=fs->bs;
        if(stage){
            uint8_t *p=e4_stage(fs->engine,blocks[i],E4_REFERENCE);if(!p)return -1;
            memcpy(p,bytes+(size_t)i*fs->bs,take);
        }else if(!e4_bytes(fs,(uint64_t)blocks[i]*fs->bs,bytes+(size_t)i*fs->bs,take))return -1;
    }return 0;
}
static int validate(ext4_mount_t *fs,pg_view *out){
    pg_view v;
    if(!eligible(fs))return -1;
    if(record_io(fs,0,marker,sizeof marker,false)||record_io(fs,1,ledger,sizeof ledger,false)||
       record_io(fs,2,table,sizeof table,false)||
       pg_validate_set(marker,sizeof marker,ledger,sizeof ledger,table,sizeof table,&binding,&v))return -1;
    if(ids[0]!=marker_ino)return -1;
    for(unsigned i=0;i<3;i++)if(ids[i+1]!=binding.inode[i])return -1;
    uint8_t sb[1024];if(!e4_bytes(fs,1024,sb,sizeof sb)||memcmp(sb+104,binding.uuid[0],16))return -1;
    /* Whole-filesystem audit proves uniqueness against unrelated owners, not
     * merely pairwise uniqueness among package objects. Read-only planner. */
    if(e4_orphan_validate(fs->engine))return -1;
    uint64_t blocks;
    for(unsigned i=0;i<3;i++)if(object(fs,ids[i],i?binding.generation[i-1]:marker_generation,
         i==0?512:i==1?1024:8192,false,true,&blocks))return -1;
    if(object(fs,ids[3],binding.generation[2],fs->bs,true,true,&blocks))return -1;
    /* First integration deliberately admits exactly one linked payload role.
     * Orphans, tombstones, baseline victims and other role layouts refuse. */
    const uint8_t *p=table+256;
    if(e4_u16(table+22)!=1 || e4_u16(p)!=1 || e4_u16(p+2)!=1 ||
       e4_u32(p+12)!=1 || e4_u32(p+4)!=ids[4] ||
       object(fs,ids[4],e4_u32(p+8),u64(p+32),false,false,&blocks)||
       blocks!=u64(p+40)||u64(p+48)!=1)return -1;
    if((uint64_t)e4_u32(sb+12)<v.remaining[0] || e4_u32(sb+12)-v.remaining[0]<1 ||
       (uint64_t)e4_u32(sb+16)<v.remaining[1] || e4_u32(sb+16)-v.remaining[1]<1)return -1;
    *out=v;return 0;
}
static void discover(void){
    for(unsigned i=0;i<5;i++){vfs_node_t *n=vfs_lookup(paths[i]);assert(n);ids[i]=((e4_inode_t *)n->fs_private)->ino;}
}
static void write_file(unsigned i,const void *p,size_t n){
    file_t *f=vfs_open(paths[i],VFS_O_CREAT|VFS_O_RDWR);assert(f);
    if(n)assert(vfs_write(f,p,n)==(int64_t)n);
    assert(!vfs_close(f));
    creds_t root;creds_init_root(&root);assert(!vfs_chmod_creds(paths[i],0600,&root));
}
static void seed(ext4_mount_t *fs){
    assert(eligible(fs));
    assert(!vfs_mkdir("/mnt/pkg",0700));
    assert(!vfs_mkdir(paths[3],0700));
    memset(marker,0,sizeof marker);memset(ledger,0,sizeof ledger);memset(table,0,sizeof table);
    write_file(0,marker,sizeof marker);write_file(1,ledger,sizeof ledger);write_file(2,table,sizeof table);write_file(4,NULL,0);
    discover();memset(&binding,0,sizeof binding);binding.epoch=1;
    marker_ino=ids[0];marker_generation=e4_u32(raw_inode(fs,ids[0])+100);
    uint8_t sb[1024];assert(e4_bytes(fs,1024,sb,sizeof sb));memcpy(binding.uuid[0],sb+104,16);
    for(unsigned i=1;i<5;i++)memset(binding.uuid[i],(int)i,16);
    memset(binding.profile,'P',32);
    for(unsigned i=0;i<3;i++){binding.inode[i]=ids[i+1];binding.generation[i]=e4_u32(raw_inode(fs,ids[i+1])+100);}
    memcpy(marker,"FORT-PKG-ENR-v1",16);e4_p32(marker+16,512);e4_p16(marker+20,1);p64(marker+24,1);
    memcpy(marker+32,binding.uuid,80);memcpy(marker+112,binding.profile,32);
    for(unsigned i=0;i<3;i++){e4_p32(marker+144+8*i,binding.inode[i]);e4_p32(marker+148+8*i,binding.generation[i]);}
    e4_p32(marker+168,2);e4_p32(marker+172,1);hash(marker,480,marker+480);hash(marker,512,binding.marker_sha256);
    memcpy(ledger,"FORT-PKG-GRT-v1",16);e4_p32(ledger+16,1024);e4_p16(ledger+20,1);e4_p16(ledger+22,PG_RESERVED);
    p64(ledger+24,1);p64(ledger+32,1);memset(ledger+40,'T',16);
    memcpy(ledger+56,binding.uuid[4],16);memcpy(ledger+72,binding.uuid[0],16);memcpy(ledger+88,binding.uuid[3],16);
    memcpy(ledger+104,binding.profile,32);memset(ledger+168,'S',32);memset(ledger+232,'A',32);
    p64(ledger+296,8);p64(ledger+304,1);p64(ledger+312,8);p64(ledger+336,1);
    e4_p32(ledger+344,1);e4_p16(ledger+348,1);e4_p32(ledger+352,ids[3]);e4_p32(ledger+356,binding.generation[2]);
    p64(ledger+360,1);p64(ledger+376,1);p64(ledger+384,1);memset(ledger+392,'R',32);
    memcpy(table,"FORT-PKG-OWN-v1",16);e4_p32(table+16,8192);e4_p16(table+20,1);e4_p16(table+22,1);
    p64(table+24,1);memset(table+32,'T',16);p64(table+48,1);
    memcpy(table+56,binding.uuid[4],16);memcpy(table+72,binding.uuid[0],16);memcpy(table+88,binding.uuid[3],16);memcpy(table+104,binding.profile,32);
    e4_p16(table+256,1);e4_p16(table+258,1);e4_p32(table+260,ids[4]);e4_p32(table+264,e4_u32(raw_inode(fs,ids[4])+100));
    e4_p32(table+268,1);memset(table+272,'T',16);p64(table+304,1);seal_records();
    write_file(0,marker,sizeof marker);write_file(1,ledger,sizeof ledger);write_file(2,table,sizeof table);
    assert(!ext4_sync(fs));
}
static int change(ext4_mount_t *fs,bool refund){
    uint64_t lock=spin_lock_irqsave(&e4_lock);pg_view v;assert(!validate(fs,&v));
    ext4_engine_t *e=fs->engine;uint64_t offset=0;int r;
    e->file_planning=true;
    if(refund)r=e4_change(e,ids[4],0,0,true);
    else {memset(payload,'X',fs->bs);r=e4_file_plan(e,ids[4],&offset,false,payload,fs->bs);}
    if(!r){
        p64(ledger+24,v.sequence+1);p64(table+48,v.sequence+1);
        p64(ledger+312,refund?8:7);p64(ledger+328,refund?0:1);
        p64(table+288,refund?0:fs->bs);p64(table+296,refund?0:1);seal_records();
        r=record_io(fs,1,ledger,sizeof ledger,true);if(!r)r=record_io(fs,2,table,sizeof table,true);
    }
    e->file_planning=false;if(!r)r=e4_semantic_commit(e);
    if(r)ext4_engine_abort(e);
    e4_semantic_invalidate(e);
    spin_unlock_irqrestore(&e4_lock,lock);return r;
}
static void reboot(block_dev_t *dev,ext4_mount_t **fs){
    teardown();ext4_fault_restart(dev->priv);vfs_init();assert(!ext4_mount_journal_fixture(dev,"/mnt",admitted,fs));discover();
}
static void check(ext4_mount_t *fs){
    pg_view v;ext4_fault_disk_t *d=fs->dev->priv;size_t writes=d->writes,events=d->events;
    uint64_t lock=spin_lock_irqsave(&e4_lock);assert(!validate(fs,&v));
    assert(v.charged[0]<=1 && v.remaining[0]+v.charged[0]==8);
    assert(d->writes==writes && d->events==events);spin_unlock_irqrestore(&e4_lock,lock);
}
int main(int argc,char **argv){
    assert(argc==4);size_t size;uint8_t *initial=load(argv[1],&size);unsigned ss=(unsigned)strtoul(argv[2],NULL,10);
    ext4_fault_disk_t d={.stable=malloc(size),.volatile_bytes=malloc(size),.bytes=size,.cut=-1};assert(d.stable&&d.volatile_bytes);
    block_dev_t dev={.sector_size=ss,.sector_count=size/ss,.read_sector=ext4_fault_read,.write_sector=ext4_fault_write,.flush=ext4_fault_flush,.priv=&d};
    ext4_mount_t *fs;reset(&d,initial);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));seed(fs);check(fs);
    uint8_t *before=malloc(size),*charged=malloc(size);assert(before&&charged);memcpy(before,d.stable,size);
    d.events=d.writes=d.flushes=0;assert(!change(fs,false));size_t charge_events=d.events;check(fs);memcpy(charged,d.stable,size);
    assert(!ext4_freeze_and_sync(fs));save(argv[3],"charged",d.stable,size);
    reboot(&dev,&fs);d.events=d.writes=d.flushes=0;assert(!change(fs,true));size_t refund_events=d.events;check(fs);
    assert(!ext4_freeze_and_sync(fs));save(argv[3],"refund",d.stable,size);
    unsigned cuts=0;
    for(unsigned op=0;op<2;op++)for(unsigned persistence=0;persistence<4;persistence++)for(unsigned after=0;after<2;after++)
    for(size_t cut=0;cut<(op?refund_events:charge_events);cut++){
        reset(&d,op?charged:before);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));discover();
        d.events=d.writes=d.flushes=0;d.persistence=persistence;d.cut=(long)cut;d.after=after!=0;
        assert(change(fs,op!=0)==-VFS_EIO);assert(fs->engine->tainted);
        for(unsigned pass=0;pass<2;pass++){reboot(&dev,&fs);check(fs);assert(!ext4_freeze_and_sync(fs));}
        cuts++;
    }
    save(argv[3],"recovered",d.stable,size);
    teardown();free(initial);free(before);free(charged);free(d.stable);free(d.volatile_bytes);
    printf("PASS actual-filesystem charge-events=%zu refund-events=%zu cuts=%u two-restarts-per-cut\n",charge_events,refund_events,cuts);
    return 0;
}
