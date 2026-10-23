/* Private copied-source host overlay. Never linked into production. */
#include "pkg_retirement_candidate.h"
static bool rt_active;
static uint32_t rt_ids[3],rt_generations[3];
static uint8_t rt_records[3][1024];
static pr_binding rt_binding;
static unsigned rt_commits,rt_max_images;
static const char *rt_paths[3]={"/mnt/pkg/retirement.v1","/mnt/pkg/account.v4","/mnt/pkg/origin.v1"};
static int rt_io(ext4_engine_t *e,unsigned index,uint8_t *bytes,bool stage){
    ext4_mount_t *fs=e->fs;e4_inode_t in;uint64_t owned;
    if(object(fs,rt_ids[index],rt_generations[index],1024,false,true,&owned)||e4_inode(fs,rt_ids[index],&in))return -1;
    fs->rw_ino=0;if(e4_mapping(&in))return -1;
    uint32_t blocks[2];unsigned count=(1024+fs->bs-1)/fs->bs;
    for(unsigned i=0;i<count;i++){const e4_extent_t *x=e4_find(&in,i);if(!x||x->unwritten)return -1;blocks[i]=x->physical+i-x->logical;}
    for(unsigned i=0;i<count;i++){
        size_t n=1024-(size_t)i*fs->bs;if(n>fs->bs)n=fs->bs;
        if(stage){uint8_t *p=e4_stage(e,blocks[i],E4_REFERENCE);if(!p)return -1;memcpy(p,bytes+(size_t)i*fs->bs,n);}
        else if(!e4_bytes(fs,(uint64_t)blocks[i]*fs->bs,bytes+(size_t)i*fs->bs,n))return -1;
    }return 0;
}
static pr_set rt_set(const uint8_t *g,const uint8_t *t,uint8_t r[3][1024]){
    return (pr_set){.marker=marker,.marker_n=512,.grant=g,.grant_n=1024,.owners=t,.owners_n=8192,
        .ledger=r[0],.ledger_n=1024,.account=r[1],.account_n=1024,.origin=r[2],.origin_n=1024};
}
static void rt_seal(uint8_t *p){hash(p,992,p+992);}
static void rt_build(uint8_t r[3][1024],const uint8_t *g,const uint8_t *t,uint64_t account_sequence){
    uint8_t *l=r[0],*a=r[1],*o=r[2];
    p64(l+24,u64(g+24));hash(g,1024,l+136);hash(t,8192,l+168);p64(l+200,account_sequence);
    e4_p16(l+218,e4_u16(t+258));e4_p32(l+220,e4_u32(t+268));p64(l+232,u64(t+288));
    memcpy(l+256,g+312,32);memcpy(l+288,t+296,16);rt_seal(l);
    p64(o+24,account_sequence);rt_seal(o);p64(a+24,account_sequence);hash(o,1024,a+112);
    p64(a+440,u64(g+24));hash(g,1024,a+448);hash(l,1024,a+592);rt_seal(a);
}
static void rt_setup(ext4_mount_t *fs){
    life_active=false;
    for(unsigned i=0;i<3;i++){
        file_t *f=vfs_open(rt_paths[i],VFS_O_CREAT|VFS_O_RDWR);assert(f);memset(rt_records[i],0,1024);
        assert(vfs_write(f,rt_records[i],1024)==1024);assert(!vfs_close(f));
        creds_t root;creds_init_root(&root);assert(!vfs_chmod_creds(rt_paths[i],0600,&root));
        vfs_node_t *n=vfs_lookup(rt_paths[i]);assert(n);rt_ids[i]=((e4_inode_t *)n->fs_private)->ino;rt_generations[i]=e4_u32(raw_inode(fs,rt_ids[i])+100);
    }
    e4_p16(ledger+22,PG_RETIRING);memset(ledger+136,'I',32);memset(ledger+200,'N',32);p64(ledger+368,2);seal_records();
    write_file(1,ledger,1024);write_file(2,table,8192);
    memset(&rt_binding,0,sizeof rt_binding);rt_binding.grant=binding;memset(rt_binding.capability,'C',32);memset(rt_binding.account_profile,'A',32);
    memcpy(rt_binding.transaction,ledger+40,16);memcpy(rt_binding.trust,ledger+232,32);memcpy(rt_binding.state,ledger+200,32);memcpy(rt_binding.intent,ledger+136,32);
    rt_binding.role=1;rt_binding.inode=life_target;rt_binding.generation=e4_u32(table+264);
    uint8_t *l=rt_records[0],*a=rt_records[1],*o=rt_records[2];
    memcpy(l,"FORT-PKG-RLG-v1",16);e4_p32(l+16,1024);e4_p16(l+20,1);p64(l+32,1);memcpy(l+40,ledger+40,64);memcpy(l+104,rt_binding.capability,32);
    p64(l+208,9);e4_p16(l+216,1);e4_p32(l+224,life_target);e4_p32(l+228,rt_binding.generation);memcpy(l+240,ledger+296,16);
    memcpy(a,"FORT-ACC-GEN-v1",16);e4_p32(a+16,1024);e4_p16(a+20,4);p64(a+32,4);p64(a+40,1);memcpy(a+48,binding.uuid[4],16);memcpy(a+64,binding.uuid[0],16);memcpy(a+80,rt_binding.account_profile,32);p64(a+144,9);
    for(unsigned i=0;i<3;i++){memset(a+328+32*i,10+i,32);e4_p32(a+424+4*i,32);}
    memcpy(a+480,ledger+232,32);memcpy(a+512,ledger+200,32);memcpy(a+544,ledger+136,32);memcpy(a+576,ledger+40,16);
    memcpy(o,"FORT-ACC-ORG-v1",16);e4_p32(o+16,1024);e4_p16(o+20,1);p64(o+32,1);p64(o+40,4);memcpy(o+48,a+48,64);
    rt_build(rt_records,ledger,table,50);
    for(unsigned i=0;i<3;i++){file_t *f=vfs_open(rt_paths[i],VFS_O_RDWR);assert(f);assert(vfs_write(f,rt_records[i],1024)==1024);assert(!vfs_close(f));}
    assert(!ext4_sync(fs));life_active=true;rt_active=true;
    pr_set s=rt_set(ledger,table,rt_records);pr_view v;assert(!pr_validate(&s,&rt_binding,&v));
}
static int rt_check(ext4_engine_t *e){
    if(!rt_active)return 0;
    uint64_t allocated;
    for(unsigned i=0;i<3;i++)if(object(e->fs,ids[i],i?binding.generation[i-1]:marker_generation,
        i==0?512:i==1?1024:8192,false,true,&allocated))return -1;
    for(unsigned i=0;i<3;i++)if(rt_io(e,i,rt_records[i],false))return -1;
    pr_set s=rt_set(ledger,table,rt_records);pr_view v;return pr_validate(&s,&rt_binding,&v);
}
/* life wrapper first audits staged physical allocation. Both complete sets
 * validate before the real commit can issue its first write. */
static int pg_life_core_commit(ext4_engine_t *e){
    if(!rt_active)return rt_core_commit(e);
    static uint8_t old_g[1024],old_t[8192],old_r[3][1024],next_r[3][1024];
    if(record_io(e->fs,1,old_g,1024,false)||record_io(e->fs,2,old_t,8192,false))return -VFS_EIO;
    for(unsigned i=0;i<3;i++)if(rt_io(e,i,old_r[i],false))return -VFS_EIO;
    memcpy(next_r,old_r,sizeof next_r);uint64_t seq=u64(old_r[1]+24);if(seq==UINT64_MAX)return -VFS_EINVAL;
    rt_build(next_r,ledger,table,seq+1);
    uint8_t sb[1024];if(!e4_bytes(e->fs,1024,sb,1024))return -VFS_EIO;
    uint8_t *planned=e4_super(e);if(!planned)return -VFS_EIO;
    uint64_t before[2]={e4_u32(sb+12),e4_u32(sb+16)},after[2]={e4_u32(planned+12),e4_u32(planned+16)};
    pr_set x=rt_set(old_g,old_t,old_r),y=rt_set(ledger,table,next_r);pr_change change;
    if(pr_transition(&x,&y,&rt_binding,before,after,&change))return -VFS_EINVAL;
    for(unsigned i=0;i<3;i++)if(rt_io(e,i,next_r[i],true))return -VFS_EIO;
    if(e->images>rt_max_images)rt_max_images=e->images;
    unsigned images=e->images;int result=rt_core_commit(e);
    if(!result){
        if(rt_commits<3)printf("RETIRE op=%u seq=%llu account=%llu F=%llu/%llu->%llu/%llu Q=%llu/%llu->%llu/%llu images=%u\n",
            change.operation,(unsigned long long)change.sequence,(unsigned long long)(seq+1),
            (unsigned long long)before[0],(unsigned long long)before[1],(unsigned long long)after[0],(unsigned long long)after[1],
            (unsigned long long)u64(old_g+328),(unsigned long long)u64(old_g+336),
            (unsigned long long)u64(ledger+328),(unsigned long long)u64(ledger+336),images);
        rt_commits++;
    }
    return result;
}

static unsigned long rt_account_sequence(void){return (unsigned long)u64(rt_records[1]+24);}

static void rt_report(void){printf("RETIRE committed=%u max-images=%u limit=%u five-record-coupling\n",rt_commits,rt_max_images,E4_IMAGES);}
