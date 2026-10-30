/* Legacy bounded EXT4/VFS/JBD2 transaction engine, retained for reuse.
 * Included after filesystem definitions; dormant in ordinary builds. */
#include "pkg_executor.h"
#include "crypto.h"
#include "../../user/tools/gzip.h"
#ifndef PX_REJECT
#define PX_REJECT(code) (code)
#endif
enum {PX_META=1,PX_CREATE_FILE,PX_CREATE_DIR,PX_WRITE,PX_PUBLISH,PX_HIDE,PX_TRUNCATE,PX_UNLINK,PX_RMDIR,PX_ORPHAN,PX_OTHER_ORPHAN};
typedef struct {
    px_policy policy;ext4_mount_t *fs;px_record state,backup,pending;
    pi_workspace verify;pi_plan shape,plan;pi_inventory inventory;pi_object observed[16];
    uint8_t *artifact,*catalog,*payload,*nodes;size_t artifact_n,catalog_n,payload_n;
    size_t offsets[16];unsigned directory_count;char directories[PX_DIRS][192],files[16][192];
    fpk_span names[256];uint8_t name_bytes[256][32],node_used[128];
    uint8_t raw[8192],encoded[8192],scratch[16384],compare[16384];
    uint8_t root_hash[32];e4_move_cache_t publication;unsigned publication_busy;
    uintptr_t actor;unsigned action,role,busy,transactions;uint64_t reader_at,decode_at;
    char mount[32];
} px_context;
static px_context *px;
/* Bounded FPK payloads can exceed the heap's single expansion batch. Use its
 * existing multi-batch allocator, retaining zeroed-state semantics. */
static void *px_allocate(size_t bytes){
    if(!bytes||bytes>32u*1024*1024)return NULL;
    void *out=kmalloc_large(bytes);if(out)memset(out,0,bytes);return out;
}
static bool px_released(ext4_engine_t *,uint32_t,bool);
static void px_copy(char *out,const char *in){memcpy(out,in,strlen(in)+1);}
static bool px_actor(void){uintptr_t actor=px?__atomic_load_n(&px->actor,__ATOMIC_ACQUIRE):0;return actor&&px->policy.current_actor&&px->policy.current_actor()==actor;}
static void *px_node_new(ext4_mount_t *fs){
    if(!px||px->fs!=fs||!px_actor())return kcalloc(1,sizeof(e4_node_t));
    _Static_assert(sizeof(e4_node_t)<=1024,"private node credit");
    for(unsigned i=0;i<128;i++){uint8_t zero=0;if(__atomic_compare_exchange_n(px->node_used+i,&zero,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){
        void *p=px->nodes+1024*i;memset(p,0,1024);return p;
    }}return NULL;
}
static void px_node_drop(void *p){
    if(px&&p&&(uintptr_t)p>=(uintptr_t)px->nodes&&(uintptr_t)p<(uintptr_t)px->nodes+128*1024){
        unsigned i=((uintptr_t)p-(uintptr_t)px->nodes)/1024;__atomic_store_n(px->node_used+i,0,__ATOMIC_RELEASE);return;
    }kfree(p);
}
static void *px_publication_new(void){
    if(!px_actor())return kcalloc(1,sizeof(e4_move_cache_t));
    unsigned zero=0;if(!__atomic_compare_exchange_n(&px->publication_busy,&zero,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return NULL;
    memset(&px->publication,0,sizeof px->publication);return &px->publication;
}
static void px_publication_drop(void *p){if(px&&p==&px->publication){__atomic_store_n(&px->publication_busy,0,__ATOMIC_RELEASE);return;}kfree(p);}
static bool px_cache_room(ext4_mount_t *fs){
    if(!px||px->fs!=fs||!px->state.phase||px_actor())return true;
    unsigned owned=0;for(unsigned i=0;i<128;i++)owned+=__atomic_load_n(px->node_used+i,__ATOMIC_ACQUIRE)!=0;
    unsigned need=px->directory_count+px->shape.file_count+9;need=need>owned?need-owned:0;
    return fs->nodes+need<E4_NODES;
}
static bool px_orphan_room(ext4_engine_t *e){return !px||px->fs!=e->fs||!px->state.phase||px_actor()||e->orphan_count<48;}
static bool px_orphan_pause(ext4_engine_t *e){return px&&px->fs==e->fs&&px_actor()&&px->transactions>=1;}
static uint64_t px_get(const uint8_t *p,unsigned n){uint64_t v=0;for(unsigned i=0;i<n;i++)v|=(uint64_t)p[i]<<(8*i);return v;}
static void px_put(uint8_t *p,unsigned n,uint64_t v){for(unsigned i=0;i<n;i++)p[i]=(uint8_t)(v>>(8*i));}
static bool px_zero(const void *p,size_t n){const uint8_t *b=p;for(size_t i=0;i<n;i++)if(b[i])return false;return true;}
static unsigned px_mask(void){return (1u<<px->shape.file_count)-1u;}
static bool px_dir_gone(unsigned i){return (px->state.deleted[i/8]>>(i%8))&1;}
static void px_encode(const px_record *s,uint8_t out[8192]){
    memset(out,0,8192);memcpy(out,"FORT-PKG-EXEC1",14);px_put(out+16,4,8192);px_put(out+20,2,1);
    px_put(out+24,8,s->sequence);px_put(out+32,8,s->generation);px_put(out+40,8,s->floor);px_put(out+48,8,s->root);px_put(out+56,8,s->catalog);
    const uint32_t fields[]={s->phase,s->installed,s->operation,s->made,s->published,s->trash,s->gone};for(unsigned i=0;i<7;i++)px_put(out+64+4*i,4,fields[i]);
    px_put(out+96,8,s->start_sequence);px_put(out+104,8,s->start_generation);memcpy(out+112,s->floor_hash,32);memcpy(out+144,s->start_hash,32);memcpy(out+176,s->plan_hash,32);
    for(unsigned i=0;i<2;i++){px_put(out+208+8*i,8,s->total[i]);px_put(out+224+8*i,8,s->remaining[i]);px_put(out+240+8*i,8,s->charged[i]);}
    memcpy(out+256,s->catalog_hash,32);memcpy(out+288,s->root_hash,32);memcpy(out+320,px->shape.package,32);memcpy(out+352,px->shape.command,32);memcpy(out+384,px->shape.platform,32);
    memcpy(out+416,px->policy.filesystem,16);memcpy(out+432,px->policy.layout,32);
    for(unsigned i=0;i<17;i++){uint8_t *r=out+512+16*i;px_put(r,4,s->files[i].inode);px_put(r+4,4,s->files[i].generation);px_put(r+8,8,s->files[i].size);}
    memcpy(out+896,s->deleted,8);for(unsigned i=0;i<64;i++){px_put(out+1024+8*i,4,s->directories[i].inode);px_put(out+1028+8*i,4,s->directories[i].generation);}
    (void)crypto_hmac_sha256(px->policy.record_key,32,out,8160,out+8160);
}
static void px_decode(const uint8_t *in,px_record *s){
    memset(s,0,sizeof *s);s->sequence=px_get(in+24,8);s->generation=px_get(in+32,8);s->floor=px_get(in+40,8);s->root=px_get(in+48,8);s->catalog=px_get(in+56,8);
    s->phase=px_get(in+64,4);s->installed=px_get(in+68,4);s->operation=px_get(in+72,4);s->made=px_get(in+76,4);s->published=px_get(in+80,4);s->trash=px_get(in+84,4);s->gone=px_get(in+88,4);
    s->start_sequence=px_get(in+96,8);s->start_generation=px_get(in+104,8);memcpy(s->floor_hash,in+112,32);memcpy(s->start_hash,in+144,32);memcpy(s->plan_hash,in+176,32);
    for(unsigned i=0;i<2;i++){s->total[i]=px_get(in+208+8*i,8);s->remaining[i]=px_get(in+224+8*i,8);s->charged[i]=px_get(in+240+8*i,8);}
    memcpy(s->catalog_hash,in+256,32);memcpy(s->root_hash,in+288,32);
    for(unsigned i=0;i<17;i++){s->files[i].inode=px_get(in+512+16*i,4);s->files[i].generation=px_get(in+516+16*i,4);s->files[i].size=px_get(in+520+16*i,8);}
    memcpy(s->deleted,in+896,8);for(unsigned i=0;i<64;i++){s->directories[i].inode=px_get(in+1024+8*i,4);s->directories[i].generation=px_get(in+1028+8*i,4);}
}
static int px_lookup(ext4_mount_t *fs,const char *path,e4_inode_t *out){
    uint32_t ino=2;while(*path){if(*path=='/'){path++;continue;}char name[64];unsigned n=0;while(*path&&*path!='/'){if(n==63)return -VFS_EINVAL;name[n++]=*path++;}name[n]=0;
        e4_inode_t p;int r=e4_inode(fs,ino,&p);if(r)return r;uint32_t child=0;r=e4_scan(&p,name,0,NULL,&child);if(r<0)return r;if(!child)return -VFS_ENOENT;ino=child;
    }return e4_inode(fs,ino,out);
}
static int px_inode_bytes(ext4_engine_t *e,uint32_t ino,uint8_t out[256],bool planned){
    unsigned group=(ino-1)/e->fs->ipg,index=(ino-1)%e->fs->ipg;if(!ino||group>=e->fs->groups)return -VFS_EINVAL;
    uint64_t at=(uint64_t)e4_u32(e->fs->gd[group].raw+8)*4096+(uint64_t)index*256;
    if(planned)for(unsigned i=0;i<e->images;i++)if(e->image[i].block==at/4096){memcpy(out,e->image[i].bytes+at%4096,256);return 0;}
    return e4_bytes(e->fs,at,out,256)?0:-VFS_EIO;
}
static uint64_t px_free(ext4_engine_t *e,bool inode,bool planned){
    uint64_t n=0;for(unsigned g=0;g<e->fs->groups;g++){
        const uint8_t *raw=e->fs->gd[g].raw;unsigned block=e->fs->first+1+g*32/4096;
        if(planned)for(unsigned i=0;i<e->images;i++)if(e->image[i].block==block)raw=e->image[i].bytes+g*32%4096;
        n+=e4_u16(raw+(inode?14:12));
    }return n;
}
static int px_record_read(ext4_mount_t *fs){
    e4_inode_t in;int r=px_lookup(fs,"/pkg/install.v2",&in);if(r)return r;px_anchor a=px->policy.anchors[0];
    if(in.ino!=a.inode||in.generation!=a.generation||in.mode!=VFS_S_IFREG||(in.full_mode&07777)!=0600||in.uid||in.gid||in.size!=8192)return PX_REJECT(-VFS_EACCES);
    r=e4_mapping(&in);if(r)return r;if(in.map->count!=1||in.map->entries[0].unwritten||in.map->entries[0].logical||in.map->entries[0].len!=2||in.map->entries[0].physical!=a.physical)return PX_REJECT(-VFS_EACCES);
    if(!e4_bytes(fs,a.physical*4096,px->raw,8192))return -VFS_EIO;
    px_decode(px->raw,&px->state);px_encode(&px->state,px->encoded);
    if(memcmp(px->raw,px->encoded,8192)||!px->state.sequence||px->state.sequence==UINT64_MAX||!px->state.generation||px->state.generation==UINT64_MAX||px->state.phase>PX_ABORT||px->state.installed>1)return PX_REJECT(-VFS_EACCES);
    if(px->state.sequence<px->policy.minimum_sequence)return PX_REJECT(-VFS_EACCES);
    uint8_t hash[32];if(crypto_sha256(px->raw,8192,hash)||(px->state.sequence==px->policy.minimum_sequence&&memcmp(hash,px->policy.minimum_hash,32)))return PX_REJECT(-VFS_EACCES);
    return 0;
}
static int px_owned_cost(ext4_engine_t *e,bool planned,uint64_t out[2]){
    out[0]=out[1]=0;uint8_t raw[256];
    for(unsigned i=0;i<17;i++)if((px->state.made&(1u<<i))&&!(px->state.gone&(1u<<i))){
        int r=px_inode_bytes(e,px->state.files[i].inode,raw,planned);if(r)return r;
        if(e4_u32(raw+100)!=px->state.files[i].generation)return -VFS_EIO;
        out[0]+=e4_u32(raw+28)/8;out[1]++;
    }
    for(unsigned i=0;i<px->directory_count;i++)if(px->state.directories[i].inode&&!px_dir_gone(i)){
        int r=px_inode_bytes(e,px->state.directories[i].inode,raw,planned);if(r)return r;
        if(e4_u32(raw+100)!=px->state.directories[i].generation)return -VFS_EIO;
        out[0]+=e4_u32(raw+28)/8;out[1]++;
    }return 0;
}
static int px_protect(ext4_engine_t *e,uint32_t ino){
    e4_inode_t in;int r=e4_inode(e->fs,ino,&in);if(r)return r;uint8_t original[256];r=px_inode_bytes(e,ino,original,false);if(r)return r;
    unsigned g=(ino-1)/e->fs->ipg,index=(ino-1)%e->fs->ipg;uint64_t slot=(uint64_t)e4_u32(e->fs->gd[g].raw+8)*4096+(uint64_t)index*256;
    r=e4_mapping(&in);if(r)return r;
    for(unsigned i=0;i<e->images;i++){
        e4_image_t *im=e->image+i;if(im->block==slot/4096&&memcmp(im->bytes+slot%4096,original,256))return PX_REJECT(-VFS_EACCES);
        if(im->block==e4_u32(e->fs->gd[g].raw+4)&&!(im->bytes[index/8]&(1u<<(index%8))))return PX_REJECT(-VFS_EACCES);
        for(unsigned j=0;j<in.map->count;j++){
            e4_extent_t x=in.map->entries[j];if(im->block>=x.physical&&im->block-x.physical<x.len)return PX_REJECT(-VFS_EACCES);
            for(unsigned k=0;k<x.len;k++){unsigned block=x.physical+k,bg=(block-e->fs->first)/e->fs->bpg,bit=(block-e->fs->first)%e->fs->bpg;
                if(im->block==e4_u32(e->fs->gd[bg].raw)&&!(im->bytes[bit/8]&(1u<<(bit%8))))return PX_REJECT(-VFS_EACCES);
            }
        }
    }return 0;
}
static int px_fence(ext4_engine_t *e){
    if(!px||px->fs!=e->fs)return 0;
    for(unsigned d=0;d<2;d++){
        uint64_t after=px_free(e,d!=0,true),before=px_free(e,d!=0,false),floor=px->state.remaining[d];
        if(after<before&&!e->fs->account_allocation)floor+=d?16:64;
        if(after<floor)return -VFS_ENOSPC;
    }
    for(unsigned i=0;i<8;i++){int r=px_protect(e,px->policy.anchors[i].inode);if(r)return r;}
    for(unsigned i=0;i<17;i++)if((px->state.made&(1u<<i))&&!(px->state.gone&(1u<<i))){int r=px_protect(e,px->state.files[i].inode);if(r)return r;}
    for(unsigned i=0;i<px->directory_count;i++)if(px->state.directories[i].inode&&!px_dir_gone(i)){int r=px_protect(e,px->state.directories[i].inode);if(r)return r;}
    return 0;
}
static int e4_semantic_commit(ext4_engine_t *e){
    if(!px||px->fs!=e->fs)return px_core_commit(e);
    if(e->tainted)return -VFS_EIO;
    if(!px_actor()){int r=px_fence(e);return r?r:px_core_commit(e);}
    if(px->action==PX_OTHER_ORPHAN){int r=px_fence(e);if(!r)r=px_core_commit(e);if(!r)px->transactions++;return r;}
    if(px->action==PX_PUBLISH||px->action==PX_HIDE||px->action==PX_TRUNCATE||px->action==PX_UNLINK||px->action==PX_ORPHAN){
        uint32_t ino=px->state.files[px->role].inode;unsigned own=px->action==PX_TRUNCATE?1:0;
        for(unsigned i=0;i<e->fs->nodes;i++){e4_node_t *n=e->fs->cached[i];if(n->inode.ino==ino&&(n->opens||n->refs>own||n->legacy))return -VFS_EAGAIN;}
    }
    px->backup=px->state;uint64_t owned[2],free_before[2]={px_free(e,false,false),px_free(e,true,false)},free_after[2]={px_free(e,false,true),px_free(e,true,true)};
    if(px->action==PX_TRUNCATE||px->action==PX_UNLINK||px->action==PX_ORPHAN)px->state.trash|=1u<<px->role;
    if(px->action==PX_META)px->state=px->pending;
    else if(px->action==PX_CREATE_FILE||px->action==PX_CREATE_DIR){
        uint8_t raw[256];int r=px_inode_bytes(e,px_created_inode,raw,true);if(r){px->state=px->backup;return r;}
        if(px->action==PX_CREATE_FILE){px->state.made|=1u<<px->role;px->state.files[px->role].inode=px_created_inode;px->state.files[px->role].generation=e4_u32(raw+100);}
        else{px->state.directories[px->role].inode=px_created_inode;px->state.directories[px->role].generation=e4_u32(raw+100);}
    }else if(px->action==PX_PUBLISH)px->state.published|=1u<<px->role;
    else if(px->action==PX_HIDE){px->state.published&=~(1u<<px->role);px->state.trash|=1u<<px->role;}
    else if(px->action==PX_WRITE||px->action==PX_TRUNCATE||px->action==PX_UNLINK||px->action==PX_ORPHAN){
        uint8_t raw[256];int r=px_inode_bytes(e,px->state.files[px->role].inode,raw,true);if(r){px->state=px->backup;return r;}
        if(e4_u32(raw+100)!=px->state.files[px->role].generation){px->state=px->backup;return -VFS_EIO;}
        if(e4_u16(raw)){if(e4_u16(raw+46)||e4_u16(raw+42)>4){px->state=px->backup;return -VFS_EFBIG;}px->state.files[px->role].size=e4_u32(raw+4)|((uint64_t)e4_u32(raw+108)<<32);}
        else if(px_released(e,px->state.files[px->role].inode,true))px->state.gone|=1u<<px->role;else{px->state=px->backup;return -VFS_EIO;}
    }else if(px->action==PX_RMDIR){uint8_t raw[256];int r=px_inode_bytes(e,px->state.directories[px->role].inode,raw,true);if(r){px->state=px->backup;return r;}if(!e4_u16(raw)){if(!px_released(e,px->state.directories[px->role].inode,true)){px->state=px->backup;return -VFS_EIO;}px->state.deleted[px->role/8]|=1u<<(px->role%8);}}
    else{px->state=px->backup;return -VFS_EINVAL;}
    int r=0;
    if(px->action==PX_META&&!px->backup.phase&&px->state.phase){
        for(unsigned d=0;d<2;d++)if(px->state.remaining[d]>free_after[d]||(d?16u:64u)>free_after[d]-px->state.remaining[d])r=-VFS_ENOSPC;
        if(e->fs->nodes+px->directory_count+px->shape.file_count+9>E4_NODES||e->orphan_count>48)r=-VFS_EAGAIN;
    }
    if(px->action!=PX_META){
        if(!r)r=px_owned_cost(e,true,owned);
        for(unsigned d=0;!r&&d<2;d++){
            if(px->state.operation==PI_INSTALL){
                if(free_after[d]<free_before[d]){uint64_t cost=free_before[d]-free_after[d];if(cost>px->state.remaining[d]){r=-VFS_ENOSPC;break;}px->state.remaining[d]-=cost;}
                else{uint64_t refund=free_after[d]-free_before[d];if(refund>px->state.charged[d]||refund>UINT64_MAX-px->state.remaining[d]){r=-VFS_EIO;break;}px->state.remaining[d]+=refund;}
                px->state.charged[d]=owned[d];px->state.total[d]=owned[d]+px->state.remaining[d];
                if(px->state.total[d]>px->plan.total[d])r=-VFS_EIO;
            }else if(free_after[d]<free_before[d])r=-VFS_EIO;
        }
    }
    for(unsigned d=0;!r&&d<2;d++)if(px->state.remaining[d]>free_after[d])r=-VFS_ENOSPC;
    if(!r&&px->backup.sequence==UINT64_MAX-1)r=-VFS_EFBIG;
    if(!r){px->state.sequence=px->backup.sequence+1;px_encode(&px->state,px->encoded);
        for(unsigned i=0;i<2;i++){uint8_t *b=e4_stage(e,px->policy.anchors[0].physical+i,E4_NEW);if(!b){r=e->error;break;}memcpy(b,px->encoded+4096*i,4096);}}
    if(!r)r=px_core_commit(e);
    if(r)px->state=px->backup;else px->transactions++;return r;
}
static bool px_add(char out[192],const char *p,size_t n){size_t at=strlen(out);if(n>=192-at)return false;memcpy(out+at,p,n);out[at+n]=0;return true;}
static bool px_decimal(char out[192],uint64_t n){char b[20];unsigned used=0;do{b[used++]=(char)('0'+n%10);n/=10;}while(n);while(used)if(!px_add(out,b+--used,1))return false;return true;}
static int px_paths(void){
    char root[192]="/usr/share/pkg/";if(!px_add(root,(char *)px->shape.package,px->shape.package_length))return -VFS_EFBIG;
    unsigned docs=0;for(unsigned i=0;i<px->shape.file_count;i++)docs+=px->shape.files[i].role==2;
    if(docs){px_copy(px->directories[px->directory_count++],root);if(!px_add(root,"/",1)||!px_decimal(root,px->shape.build))return -VFS_EFBIG;px_copy(px->directories[px->directory_count++],root);}
    for(unsigned i=0;i<px->shape.file_count;i++){
        pi_file *f=px->shape.files+i;char path[192]={0};
        if(f->role==1){px_copy(path,"/usr/bin/");if(!px_add(path,(char *)px->shape.command,px->shape.command_length))return -VFS_EFBIG;}
        else{
            px_copy(path,root);if(!px_add(path,"/",1)||!px_add(path,(char *)f->path,f->path_length))return -VFS_EFBIG;
            size_t prefix=strlen(root)+1;for(size_t at=prefix;path[at];at++)if(path[at]=='/'){
                char saved=path[at];path[at]=0;bool found=false;for(unsigned j=0;j<px->directory_count;j++)if(!strcmp(path,px->directories[j]))found=true;
                if(!found){if(px->directory_count==PX_DIRS)return -VFS_EFBIG;px_copy(px->directories[px->directory_count++],path);}path[at]=saved;
            }
        }px_copy(px->files[i],path);
    }return px->directory_count==px->shape.parent_count?0:-VFS_EINVAL;
}
typedef struct {const uint8_t *p;size_t n,at;} px_input;
static long px_source_read(void *arg,void *out,size_t n){px_input *s=arg;if(n>s->n-s->at)n=s->n-s->at;memcpy(out,s->p+s->at,n);s->at+=n;return (long)n;}
static long px_decode_read(void *arg,void *out,size_t n){px_context *c=arg;if(n>c->artifact_n-c->reader_at)n=c->artifact_n-c->reader_at;memcpy(out,c->artifact+c->reader_at,n);c->reader_at+=n;return (long)n;}
static long px_decode_write(void *arg,const void *in,size_t n){px_context *c=arg;if(n>c->payload_n-c->decode_at)return -1;memcpy(c->payload+c->decode_at,in,n);c->decode_at+=n;return (long)n;}
static void px_destroy(void){
    if(!px)return;
    for(unsigned i=0;i<128;i++)if(px->node_used[i])return; /* mounted references must drain first */
    kfree(px->artifact);kfree(px->catalog);kfree(px->payload);kfree(px->nodes);crypto_wipe(px->policy.record_key,32);kfree(px);px=NULL;
}
static int px_prepare(const px_policy *policy,const uint8_t *artifact,size_t artifact_n,const uint8_t *catalog,size_t catalog_n,const uint8_t *name,size_t name_n,bool admitted){
    if(!admitted)return -VFS_EROFS;
    if(px||!policy||!policy->authorized||!policy->current_actor||!artifact||!catalog||!fpk_identity_valid(name,name_n)||
       artifact_n>FPK_HEADER_SIZE+FPK_MANIFEST_MAX+FPK_COMPRESSED_MAX||catalog_n>FPK_CATALOG_MAX||!policy->minimum_sequence||px_zero(policy->record_key,32)||policy->resources.base_count>256||!policy->resources.base_count)return -VFS_EINVAL;
    px=px_allocate(sizeof *px);if(!px)return -VFS_ENOMEM;px->policy=*policy;
    px->artifact=px_allocate(artifact_n);px->catalog=px_allocate(catalog_n);px->nodes=px_allocate(128*1024);
    if(!px->artifact||!px->catalog||!px->nodes){px_destroy();return -VFS_ENOMEM;}
    memcpy(px->artifact,artifact,artifact_n);memcpy(px->catalog,catalog,catalog_n);px->artifact_n=artifact_n;px->catalog_n=catalog_n;
    for(size_t i=0;i<policy->resources.base_count;i++){
        fpk_span s=policy->resources.base_names[i];if(!fpk_identity_valid(s.bytes,s.length)){px_destroy();return -VFS_EINVAL;}
        memcpy(px->name_bytes[i],s.bytes,s.length);px->names[i]=(fpk_span){px->name_bytes[i],s.length};
    }px->policy.resources.base_names=px->names;
    pi_inventory *inv=&px->inventory;inv->sequence=inv->generation=1;inv->inventory_bytes=8192;memcpy(inv->hash,policy->minimum_hash,32);
    pi_resources *res=&px->policy.resources;res->block_size=4096;res->free[0]=res->capacity[0];res->free[1]=res->capacity[1];res->remaining[0]=res->remaining[1]=0;
    px_input input={px->artifact,px->artifact_n,0};
    if(pi_prepare(PI_INSTALL,px->catalog,px->catalog_n,&policy->catalog,name,name_n,px_source_read,&input,inv,px->observed,res,&px->verify,&px->shape)){
        px_destroy();return PX_REJECT(-VFS_EACCES);
    }
    px->payload_n=px->verify.verify.envelope.expanded_bytes;
    uint64_t memory=sizeof *px+artifact_n+catalog_n+128*1024+px->payload_n;
    if(memory>32u*1024*1024||memory>policy->resources.memory_bytes){px_destroy();return -VFS_ENOMEM;}
    px->payload=px_allocate(px->payload_n);
    if(!px->payload){px_destroy();return -VFS_ENOMEM;}
    px->reader_at=128+px->verify.verify.envelope.manifest_bytes;
    if(gzip_decode_callbacks(px_decode_read,px_decode_write,px)||px->decode_at!=px->payload_n||px->reader_at!=px->artifact_n){px_destroy();return PX_REJECT(-VFS_EACCES);}
    size_t at=0;for(unsigned i=0;i<px->shape.file_count;i++){px->offsets[i]=at+512;at+=512+(size_t)((px->shape.files[i].size+511)/512)*512;}
    if(at+1024!=px->payload_n){px_destroy();return PX_REJECT(-VFS_EACCES);}
    uint8_t root[288];memcpy(root,policy->catalog.repository,16);memcpy(root+16,policy->catalog.keys,256);px_put(root+272,4,policy->catalog.key_count);px_put(root+276,4,policy->catalog.threshold);px_put(root+280,8,policy->catalog.root_version);
    (void)crypto_sha256(root,sizeof root,px->root_hash);int r=px_paths();if(r)px_destroy();return r;
}
static void px_stage_path(unsigned i,char out[192]){px_copy(out,"/pkg/work-v2/m00");size_t n=strlen(out);out[n-2]='0'+i/10;out[n-1]='0'+i%10;}
static unsigned px_member(unsigned i){if(i!=16)return i;for(unsigned j=0;j<px->shape.file_count;j++)if(px->shape.files[j].role==1)return j;return 0;}
static uint64_t px_member_size(unsigned i){return px->shape.files[px_member(i)].size;}
static bool px_released(ext4_engine_t *e,uint32_t ino,bool planned){
    unsigned group=(ino-1)/e->fs->ipg,bit=(ino-1)%e->fs->ipg;uint32_t block=e4_u32(e->fs->gd[group].raw+4);
    if(planned)for(unsigned i=0;i<e->images;i++)if(e->image[i].block==block)return !(e->image[i].bytes[bit/8]&(1u<<(bit%8)));
    if(!e4_bitmap(e->fs,group,true))return false;
    return !e4_bit(e->fs->scratch,bit);
}
/* Raw prefix comparison also admits the mapped prefix of a trusted trash inode
 * undergoing bounded right-edge orphan cleanup beyond its new zero EOF. */
static int px_contents(ext4_mount_t *fs,unsigned role,e4_inode_t *in){
    int r=e4_mapping(in);if(r)return r;uint64_t logical=0,compared=0;uint64_t limit=(px->state.trash&(1u<<role))?px_member_size(role):px->state.files[role].size;
    const uint8_t *expected=px->payload+px->offsets[px_member(role)];
    for(unsigned i=0;i<in->map->count;i++){
        e4_extent_t x=in->map->entries[i];if(x.unwritten||x.logical!=logical)return PX_REJECT(-VFS_EACCES);
        uint64_t bytes=(uint64_t)x.len*4096,offset=(uint64_t)x.logical*4096;if(offset>=limit&&limit)return PX_REJECT(-VFS_EACCES);
        uint64_t take=offset<limit?limit-offset:0;if(take>bytes)take=bytes;
        for(uint64_t at=0;at<take;){size_t n=take-at;if(n>sizeof px->compare)n=sizeof px->compare;
            if(!e4_bytes(fs,(uint64_t)x.physical*4096+at,px->compare,n)||memcmp(px->compare,expected+offset+at,n))return PX_REJECT(-VFS_EACCES);
            at+=n;}
        logical+=x.len;compared+=take;
    }
    if(!(px->state.trash&(1u<<role))&&(compared!=limit||logical!=(limit+4095)/4096))return PX_REJECT(-VFS_EACCES);
    return 0;
}
static int px_expected_absent(ext4_mount_t *fs,const char *path){e4_inode_t in;int r=px_lookup(fs,path,&in);return r==-VFS_ENOENT?0:r?r:-VFS_EACCES;}
static const char *px_child_name(const char *parent,const char *child){
    size_t n=strlen(parent);if(strncmp(parent,child,n)||child[n]!='/')return NULL;
    const char *name=child+n+1;if(!*name)return NULL;
    for(const char *p=name;*p;p++)if(*p=='/')return NULL;
    return name;
}
static int px_directory_members(e4_inode_t *dir,unsigned role){
    for(unsigned at=0;at<84;at++){
        uint32_t found=0;vfs_dirent_t entry;int r=e4_scan(dir,NULL,at,&entry,&found);if(r<0)return r;if(!found)return 0;
        if(!strcmp(entry.name,".")||!strcmp(entry.name,".."))continue;
        bool known=false;
        for(unsigned i=0;i<px->directory_count;i++)if(px->state.directories[i].inode&&!px_dir_gone(i)){
            const char *name=px_child_name(px->directories[role],px->directories[i]);
            if(name&&!strcmp(name,entry.name)&&found==px->state.directories[i].inode&&entry.type==VFS_DIRECTORY)known=true;
        }
        for(unsigned i=0;i<px->shape.file_count;i++)if(px->state.published&(1u<<i)){
            const char *name=px_child_name(px->directories[role],px->files[i]);
            if(name&&!strcmp(name,entry.name)&&found==px->state.files[i].inode&&entry.type==VFS_FILE)known=true;
        }
        if(!known)return PX_REJECT(-VFS_EACCES);
    }return PX_REJECT(-VFS_EACCES);
}
static int px_validate(ext4_mount_t *fs){
    if(fs->bs!=4096||fs->blocks!=px->policy.resources.capacity[0]||fs->inodes!=px->policy.resources.capacity[1])return PX_REJECT(-VFS_EACCES);
    int r=px_record_read(fs);if(r)return r;px_record *s=&px->state;unsigned mask=px_mask(),all=mask|(1u<<16);
    if((s->made|s->trash|s->gone)&~all||s->published&~mask||(s->trash&~s->made)||(s->gone&~s->made)||(s->published&~s->made)||(s->published&s->trash))return PX_REJECT(-VFS_EACCES);
    unsigned primary=s->made&mask;if((primary&(primary+1))||((s->made&(1u<<16))&&primary!=mask)||(s->gone&s->published)||(s->gone&~s->trash))return PX_REJECT(-VFS_EACCES);
    unsigned executable=1u<<px_member(16),docs=mask&~executable;
    if((s->phase==PX_BUILD||s->phase==PX_ABORT)&&(s->published&executable))return PX_REJECT(-VFS_EACCES);
    if(s->phase==PX_COMMIT&&(s->made!=all||(s->published&docs)!=docs||s->trash||s->gone))return PX_REJECT(-VFS_EACCES);
    if(s->phase==PX_RETIRE&&(s->made!=all||s->published!=mask||((s->trash|s->gone)&mask)))return PX_REJECT(-VFS_EACCES);
    if(s->phase==PX_DELETE&&(s->made!=mask||((~s->published)&mask&~s->trash)))return PX_REJECT(-VFS_EACCES);
    if(s->floor>px->shape.build||(s->floor==px->shape.build&&memcmp(s->floor_hash,px->shape.artifact,32))||
       (s->floor?px_zero(s->floor_hash,32):!px_zero(s->floor_hash,32))||s->root>px->shape.root||s->catalog>px->shape.catalog||
       (s->root&&s->root==px->shape.root&&memcmp(s->root_hash,px->root_hash,32))||
       (s->catalog&&s->catalog==px->shape.catalog&&memcmp(s->catalog_hash,px->shape.catalog_hash,32)))return PX_REJECT(-VFS_EACCES);
    if(!s->phase){
        if(s->operation||s->start_sequence||s->start_generation||!px_zero(s->start_hash,32)||!px_zero(s->plan_hash,32)||s->trash||s->gone||
           (s->installed?(s->made!=mask||s->published!=mask||s->floor!=px->shape.build):(s->made||s->published)))return PX_REJECT(-VFS_EACCES);
        for(unsigned d=0;d<2;d++)if(s->total[d]||s->remaining[d]||s->charged[d])return PX_REJECT(-VFS_EACCES);
    }else{
        if(!s->start_sequence||s->start_sequence>=s->sequence||!s->start_generation||px_zero(s->start_hash,32)||px_zero(s->plan_hash,32)||s->floor!=px->shape.build||
           (s->phase==PX_DELETE?s->operation!=PI_REMOVE:s->operation!=PI_INSTALL)||
           (s->phase==PX_RETIRE?s->generation!=s->start_generation+1:s->generation!=s->start_generation)||
           ((s->phase==PX_RETIRE||s->phase==PX_DELETE)?!s->installed:s->installed))return PX_REJECT(-VFS_EACCES);
        for(unsigned d=0;d<2;d++)if(s->remaining[d]>UINT64_MAX-s->charged[d]||s->total[d]!=s->remaining[d]+s->charged[d]||s->remaining[d]>px_free(fs->engine,d!=0,false))return PX_REJECT(-VFS_EACCES);
    }
    static const char *anchors[]={"/pkg/install.v2","/pkg/work-v2","/usr/bin","/usr/share/pkg","/","/pkg","/usr","/usr/share"};
    uint8_t uuid[16];if(!e4_bytes(fs,1128,uuid,16)||memcmp(uuid,px->policy.filesystem,16))return PX_REJECT(-VFS_EACCES);
    for(unsigned i=1;i<8;i++){
        e4_inode_t in;r=px_lookup(fs,anchors[i],&in);if(r)return r;
        if(in.ino!=px->policy.anchors[i].inode||in.generation!=px->policy.anchors[i].generation||in.mode!=VFS_S_IFDIR||(in.full_mode&07777)!=((i==1||i==5)?0700:0755)||in.uid||in.gid){
#ifdef PX_ANCHOR_DIAGNOSTIC
            PX_ANCHOR_DIAGNOSTIC(i,in);
#endif
            return PX_REJECT(-VFS_EACCES);
        }
    }
    for(unsigned i=0;i<17;i++){
        char path[192];px_stage_path(i,path);bool made=(s->made&(1u<<i))!=0,gone=(s->gone&(1u<<i))!=0,published=i<16&&(s->published&(1u<<i));
        bool used=i<px->shape.file_count||i==16;
        if(!made){if(s->files[i].inode||s->files[i].generation||s->files[i].size)return PX_REJECT(-VFS_EACCES);r=px_expected_absent(fs,path);if(r)return r;if(s->phase&&i<px->shape.file_count){r=px_expected_absent(fs,px->files[i]);if(r)return r;}continue;}
        if(!used||s->files[i].inode<11||!s->files[i].generation||s->files[i].size>px_member_size(i))return PX_REJECT(-VFS_EACCES);
        for(unsigned j=0;j<i;j++)if((s->made&(1u<<j))&&s->files[j].inode==s->files[i].inode)return PX_REJECT(-VFS_EACCES);
        if(gone){if(i<px->shape.file_count){r=px_expected_absent(fs,px->files[i]);if(r)return r;}r=px_expected_absent(fs,path);if(r)return r;continue;}
        e4_inode_t in;uint32_t inode=s->files[i].inode;fs->orphan_access=inode;r=e4_inode(fs,inode,&in);fs->orphan_access=0;if(r)return r;
        uint8_t raw[256];r=px_inode_bytes(fs->engine,inode,raw,false);if(r)return r;
        if(in.generation!=s->files[i].generation||in.mode!=VFS_S_IFREG||in.uid||in.gid||(in.full_mode&07777)!=(i==16?0600:px->shape.files[i].mode)||in.size!=s->files[i].size||e4_u16(in.extent+6)||e4_u16(in.extent+2)>4)return PX_REJECT(-VFS_EACCES);
        uint32_t links=e4_u16(raw+26);bool orphan=e4_orphan_index(fs->engine,inode)>=0;
        if(links!=1&&!(links==0&&orphan&&(s->trash&(1u<<i))))return PX_REJECT(-VFS_EACCES);
        const char *present=published?px->files[i]:path;
        if(links){e4_inode_t actual;r=px_lookup(fs,present,&actual);if(r||actual.ino!=inode||actual.generation!=in.generation)return PX_REJECT(-VFS_EACCES);}
        else{r=px_expected_absent(fs,present);if(r)return r;}
        if(i<px->shape.file_count){r=px_expected_absent(fs,published?path:px->files[i]);if(r)return r;}
        fs->orphan_access=inode;r=px_contents(fs,i,&in);fs->orphan_access=0;if(r)return r;
        if((published||s->phase==PX_COMMIT||s->phase==PX_RETIRE)&&!(s->trash&(1u<<i))&&in.size!=px_member_size(i))return PX_REJECT(-VFS_EACCES);
    }
    e4_inode_t work;r=px_lookup(fs,"/pkg/work-v2",&work);if(r)return r;
    for(unsigned i=0;i<20;i++){
        uint32_t found=0;vfs_dirent_t entry;r=e4_scan(&work,NULL,i,&entry,&found);if(r<0)return r;if(!found)break;
        if(!strcmp(entry.name,".")||!strcmp(entry.name,".."))continue;
        bool known=false;for(unsigned j=0;j<17;j++){char path[192];px_stage_path(j,path);const char *name=path+strlen(path)-3;if(!strcmp(entry.name,name)&&(s->made&(1u<<j))&&!(s->gone&(1u<<j))&&!(s->published&(1u<<j))&&found==s->files[j].inode)known=true;}
        if(!known||i==19)return PX_REJECT(-VFS_EACCES);
    }
    for(unsigned i=0;i<64;i++){
        if(i>=px->directory_count){if(s->directories[i].inode||s->directories[i].generation||px_dir_gone(i))return PX_REJECT(-VFS_EACCES);continue;}
        if(!s->directories[i].inode){if(s->directories[i].generation||px_dir_gone(i)||s->installed)return PX_REJECT(-VFS_EACCES);if(s->phase){r=px_expected_absent(fs,px->directories[i]);if(r)return r;}continue;}
        if(px_dir_gone(i)){r=px_expected_absent(fs,px->directories[i]);if(r)return r;continue;}
        e4_inode_t in;uint32_t ino=s->directories[i].inode;fs->orphan_access=ino;r=e4_inode(fs,ino,&in);fs->orphan_access=0;if(r)return r;
        if(in.ino!=s->directories[i].inode||in.generation!=s->directories[i].generation||in.mode!=VFS_S_IFDIR||(in.full_mode&07777)!=0755||in.uid||in.gid||in.size!=4096)return PX_REJECT(-VFS_EACCES);
        uint8_t raw[256];r=px_inode_bytes(fs->engine,ino,raw,false);if(r)return r;
        if(!e4_u16(raw+26)){if(e4_orphan_index(fs->engine,ino)<0||(s->phase!=PX_DELETE&&s->phase!=PX_ABORT))return PX_REJECT(-VFS_EACCES);r=px_expected_absent(fs,px->directories[i]);}
        else{e4_inode_t named;r=px_lookup(fs,px->directories[i],&named);if(!r&&named.ino!=ino)r=-VFS_EACCES;if(!r)r=px_directory_members(&in,i);}
        if(r)return r;
    }
    if(s->phase&&s->operation==PI_INSTALL){uint64_t owned[2];r=px_owned_cost(fs->engine,false,owned);if(r)return r;if(owned[0]!=s->charged[0]||owned[1]!=s->charged[1])return PX_REJECT(-VFS_EACCES);}
    return 0;
}
static int px_record_transaction(void){
    uint64_t flags=spin_lock_irqsave(&e4_lock);ext4_engine_t *e=px->fs->engine;int r=e4_namespace_begin(e);
    if(!r)r=e4_namespace_end(e,0);
    spin_unlock_irqrestore(&e4_lock,flags);return r;
}
static int px_scope(unsigned action,unsigned role){
    spin_debug_assert_unheld();if(!px||!px->fs||!px->policy.authorized())return PX_REJECT(-VFS_EACCES);
    if(px->fs->engine->tainted)return -VFS_EIO;
    uintptr_t actor=px->policy.current_actor();if(!actor)return PX_REJECT(-VFS_EACCES);
    px->action=action;px->role=role;px->transactions=0;__atomic_store_n(&px->actor,actor,__ATOMIC_RELEASE);return 0;
}
static int px_unscope(int r){__atomic_store_n(&px->actor,0,__ATOMIC_RELEASE);px->action=px->role=0;return r;}
static vfs_node_t *px_node(const char *path,int *error){
    creds_t root;creds_init_root(&root);return vfs_lookup_at_creds(&px->fs->cached[0]->node,path,&root,error);
}
static int px_parent(const char *path,vfs_node_t **node,char name[64]){
    char parent[192];size_t n=strlen(path),at=n;while(at&&path[at-1]!='/')at--;
    if(!at||n-at>=64||at>=sizeof parent)return -VFS_EINVAL;
    memcpy(name,path+at,n-at+1);memcpy(parent,path,at);parent[at]=0;
    int error=0;*node=px_node(parent,&error);return *node?0:error;
}
static int px_create(unsigned role,bool directory){
    char path[192],name[64];if(directory)px_copy(path,px->directories[role]);else px_stage_path(role,path);
    int r=px_scope(directory?PX_CREATE_DIR:PX_CREATE_FILE,role);if(r)return r;
    vfs_node_t *parent=NULL,*created=NULL;r=px_parent(path,&parent,name);
    if(!r){vfs_create_attrs_t attrs={directory?0755:role==16?0600:px->shape.files[role].mode,0,0};int error=0;
        created=parent->create_attrs_ref?parent->create_attrs_ref(parent,name,directory?VFS_DIRECTORY:VFS_FILE,&attrs,&error):NULL;
        if(!created)r=error?error:-VFS_EOPNOTSUPP;
    }vfs_node_put(created);vfs_node_put(parent);return px_unscope(r);
}
static int px_write_member(unsigned role){
    char path[192];px_stage_path(role,path);int r=px_scope(PX_WRITE,role);if(r)return r;
    int error=0;vfs_node_t *node=px_node(path,&error);if(!node)return px_unscope(error);
    uint64_t at=px->state.files[role].size,remaining=px_member_size(role)-at;size_t n=remaining;if(n>16384)n=16384;
    int64_t got=node->write?node->write(node,&at,false,px->payload+px->offsets[px_member(role)]+at,n):-VFS_EOPNOTSUPP;
    if(got<=0)r=got<0?(int)got:-VFS_EIO;
    vfs_node_put(node);return px_unscope(r);
}
static int px_move(unsigned role,bool publish){
    char private[192],old_name[64],new_name[64];px_stage_path(role,private);
    int r=px_scope(publish?PX_PUBLISH:PX_HIDE,role);if(r)return r;
    vfs_node_t *old=NULL,*next=NULL;r=px_parent(publish?private:px->files[role],&old,old_name);
    if(!r)r=px_parent(publish?px->files[role]:private,&next,new_name);
    if(!r)r=old->rename?old->rename(old,old_name,next,new_name):-VFS_EOPNOTSUPP;
    vfs_node_put(next);vfs_node_put(old);return px_unscope(r);
}
static int px_remove_file(unsigned role){
    char path[192],name[64];px_stage_path(role,path);
    if(px->state.published&(1u<<role))return px_move(role,false);
    ext4_engine_t *e=px->fs->engine;uint32_t ino=px->state.files[role].inode;
    int r=px_scope(PX_ORPHAN,role);if(r)return r;
    uint64_t flags=spin_lock_irqsave(&e4_lock);bool orphan=e4_orphan_index(e,ino)>=0;
    if(orphan)r=e4_orphan_cleanup(e,ino,false);
    spin_unlock_irqrestore(&e4_lock,flags);if(orphan)return px_unscope(r);
    __atomic_store_n(&px->actor,0,__ATOMIC_RELEASE);
    if(px->state.files[role].size){
        r=px_scope(PX_TRUNCATE,role);if(r)return r;int error=0;vfs_node_t *node=px_node(path,&error);
        if(!node)return px_unscope(error);
        r=node->truncate?node->truncate(node,0):-VFS_EOPNOTSUPP;vfs_node_put(node);return px_unscope(r);
    }
    r=px_scope(PX_UNLINK,role);if(r)return r;vfs_node_t *parent=NULL;r=px_parent(path,&parent,name);
    if(!r)r=parent->unlink?parent->unlink(parent,name):-VFS_EOPNOTSUPP;
    vfs_node_put(parent);return px_unscope(r);
}
static int px_remove_directory(unsigned role){
    int r=px_scope(PX_RMDIR,role);if(r)return r;ext4_engine_t *e=px->fs->engine;
    uint64_t flags=spin_lock_irqsave(&e4_lock);bool orphan=e4_orphan_index(e,px->state.directories[role].inode)>=0;
    if(orphan)r=e4_orphan_cleanup(e,px->state.directories[role].inode,false);
    spin_unlock_irqrestore(&e4_lock,flags);if(orphan)return px_unscope(r);
    char name[64];vfs_node_t *parent=NULL;r=px_parent(px->directories[role],&parent,name);
    if(!r)r=parent->unlink?parent->unlink(parent,name):-VFS_EOPNOTSUPP;
    vfs_node_put(parent);return px_unscope(r);
}
static int px_meta(void){int r=px_scope(PX_META,0);if(r)return r;r=px_record_transaction();return px_unscope(r);}
static int px_full_check(void){uint64_t flags=spin_lock_irqsave(&e4_lock);int r=px_validate(px->fs);spin_unlock_irqrestore(&e4_lock,flags);return r;}
static void px_view(bool previous){
    pi_inventory *v=&px->inventory;memset(v,0,sizeof *v);memset(px->observed,0,sizeof px->observed);
    px_record *s=&px->state;v->sequence=previous?s->start_sequence:s->sequence;v->generation=previous?s->start_generation:s->generation;v->inventory_bytes=8192;
    v->build_floor=s->floor;memcpy(v->floor_artifact,s->floor_hash,32);v->package_length=px->shape.package_length;v->command_length=px->shape.command_length;
    memcpy(v->package,px->shape.package,32);memcpy(v->command,px->shape.command,32);
    if(previous)memcpy(v->hash,s->start_hash,32);else{px_encode(s,px->raw);(void)crypto_sha256(px->raw,8192,v->hash);}
    v->installed=previous?s->operation==PI_REMOVE:s->installed;
    if(v->installed){v->build=px->shape.build;v->file_count=px->shape.file_count;memcpy(v->artifact,px->shape.artifact,32);
        for(unsigned i=0;i<v->file_count;i++){
            pi_file *f=px->shape.files+i;pi_object *o=px->observed+i;
            *o=(pi_object){.inode=s->files[i].inode,.generation=s->files[i].generation,.mode=f->mode,.links=1,.type=1,.size=f->size,.blocks=f->blocks};
            memcpy(o->hash,f->hash,32);v->owned[i]=*o;
        }
    }
}
static int px_rederive(unsigned operation,bool previous){
    px_view(previous);pi_resources res=px->policy.resources;
    res.free[0]=res.capacity[0];res.free[1]=res.capacity[1];res.remaining[0]=res.remaining[1]=0;
    if(!previous){uint64_t flags=spin_lock_irqsave(&e4_lock);res.free[0]=px_free(px->fs->engine,false,false);res.free[1]=px_free(px->fs->engine,true,false);spin_unlock_irqrestore(&e4_lock,flags);}
    px_input input={px->artifact,px->artifact_n,0};
    int r=pi_prepare(operation,px->catalog,px->catalog_n,&px->policy.catalog,px->shape.package,px->shape.package_length,px_source_read,&input,&px->inventory,px->observed,&res,&px->verify,&px->plan);
    if(r)return PX_REJECT(-VFS_EACCES);
    if(previous){
        if(memcmp(px->state.plan_hash,px->plan.hash,32))return PX_REJECT(-VFS_EACCES);
        for(unsigned d=0;d<2;d++)if(px->state.total[d]>px->plan.total[d])return PX_REJECT(-VFS_EACCES);
    }return 0;
}
static int px_begin(unsigned operation){
    int r=px_full_check();if(r)return r;if(px->state.phase)return -VFS_EAGAIN;
    r=px_rederive(operation,false);if(r)return r;if(px->plan.operation==PI_NOOP)return 0;
    if(operation==PI_INSTALL){
        uint64_t flags=spin_lock_irqsave(&e4_lock);r=px_expected_absent(px->fs,px->files[0]);
        if(!r&&px->directory_count)r=px_expected_absent(px->fs,px->directories[0]);
        if(!r&&(px->fs->nodes+px->directory_count+px->shape.file_count+9>E4_NODES||px->fs->engine->orphan_count>48))r=-VFS_ENOMEM;
        spin_unlock_irqrestore(&e4_lock,flags);if(r)return r;
    }
    px->pending=px->state;px_record *n=&px->pending;n->phase=operation==PI_INSTALL?PX_BUILD:PX_DELETE;n->operation=operation;
    n->start_sequence=n->sequence;n->start_generation=n->generation;
    px_encode(&px->state,px->raw);(void)crypto_sha256(px->raw,8192,n->start_hash);memcpy(n->plan_hash,px->plan.hash,32);
    n->floor=px->shape.build;memcpy(n->floor_hash,px->shape.artifact,32);n->root=px->shape.root;n->catalog=px->shape.catalog;
    memcpy(n->catalog_hash,px->shape.catalog_hash,32);memcpy(n->root_hash,px->root_hash,32);
    for(unsigned d=0;d<2;d++){n->total[d]=n->remaining[d]=px->plan.total[d];n->charged[d]=0;}
    return px_meta();
}
static int px_finish(bool installed){
    px->pending=px->state;px_record *n=&px->pending;n->phase=PX_CLOSED;n->installed=installed;n->operation=0;
    if(px->state.phase!=PX_ABORT)n->generation=n->start_generation+1;
    n->start_sequence=n->start_generation=0;memset(n->start_hash,0,32);memset(n->plan_hash,0,32);memset(n->total,0,sizeof n->total);memset(n->remaining,0,sizeof n->remaining);memset(n->charged,0,sizeof n->charged);
    n->trash=n->gone=0;memset(n->deleted,0,sizeof n->deleted);
    if(installed){n->made=n->published=px_mask();memset(n->files+16,0,sizeof n->files[16]);}
    else{n->made=n->published=0;memset(n->files,0,sizeof n->files);memset(n->directories,0,sizeof n->directories);}
    return px_meta();
}
/* Exactly one bounded filesystem call; orphan loops yield through EAGAIN after
 * one semantic commit, so the embedding rechecks credentials outside G. */
static int px_step(bool installing){
    ext4_engine_t *e=px->fs->engine;
    if(e->orphan_count){
        uint32_t ino=e->orphans[0];
        for(unsigned i=0;i<17;i++)if((px->state.made&(1u<<i))&&!(px->state.gone&(1u<<i))&&px->state.files[i].inode==ino)return px_remove_file(i);
        for(unsigned i=0;i<px->directory_count;i++)if(px->state.directories[i].inode==ino&&!px_dir_gone(i))return px_remove_directory(i);
        int r=px_scope(PX_OTHER_ORPHAN,0);if(r)return r;
        uint64_t flags=spin_lock_irqsave(&e4_lock);r=e4_orphan_cleanup(e,ino,false);spin_unlock_irqrestore(&e4_lock,flags);return px_unscope(r);
    }
    e->orphan_recovery_required=false;
    px_record *s=&px->state;
    if(s->phase==PX_BUILD){
        if(!installing){px->pending=*s;px->pending.phase=PX_ABORT;return px_meta();}
        for(unsigned i=0;i<px->directory_count;i++)if(!s->directories[i].inode)return px_create(i,true);
        for(unsigned i=0;i<px->shape.file_count;i++)if(!(s->made&(1u<<i)))return px_create(i,false);
        if(!(s->made&(1u<<16)))return px_create(16,false);
        for(unsigned i=0;i<px->shape.file_count;i++)if(s->files[i].size<px_member_size(i))return px_write_member(i);
        if(s->files[16].size<px_member_size(16))return px_write_member(16);
        for(unsigned i=0;i<px->shape.file_count;i++)if(px->shape.files[i].role==2&&!(s->published&(1u<<i)))return px_move(i,true);
        int r=px_full_check();if(r)return r;px->pending=px->state;px->pending.phase=PX_COMMIT;return px_meta();
    }
    if(s->phase==PX_COMMIT){
        for(unsigned i=0;i<px->shape.file_count;i++)if(!(s->published&(1u<<i)))return px_move(i,true);
        px->pending=*s;px->pending.phase=PX_RETIRE;px->pending.installed=1;px->pending.generation=s->start_generation+1;return px_meta();
    }
    if(s->phase==PX_RETIRE){if(!(s->gone&(1u<<16)))return px_remove_file(16);return px_finish(true);}
    if(s->phase==PX_DELETE||s->phase==PX_ABORT){
        for(unsigned i=0;i<17;i++)if((s->made&(1u<<i))&&!(s->gone&(1u<<i)))return px_remove_file(i);
        for(unsigned i=px->directory_count;i;i--)if(s->directories[i-1].inode&&!px_dir_gone(i-1))return px_remove_directory(i-1);
        return px_finish(false);
    }return 0;
}
static int px_run(bool installing){
    for(unsigned step=0;step<8192;step++){
        if(!px->policy.authorized())return PX_REJECT(-VFS_EACCES);
        if(!px->state.phase&&!px->fs->engine->orphan_count)return px_full_check();
        uint64_t before=px->state.sequence;int r=px_step(installing);
        if(r&&!(r==-VFS_EAGAIN&&(px->state.sequence!=before||px->transactions)))return r;
    }return -VFS_EFBIG;
}
static int px_control(unsigned operation){
    if(!px||!px->fs)return -VFS_EROFS;
    unsigned zero=0;
    if(!__atomic_compare_exchange_n(&px->busy,&zero,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return -VFS_EAGAIN;
    int r=px_full_check();if(!r&&!px->policy.authorized())r=-VFS_EACCES;
    if(!r&&!px->state.phase&&px->fs->engine->orphan_count)r=px_run(false);
    if(!r){
        if(operation==PI_INSTALL||operation==PI_REMOVE){r=px_begin(operation);if(!r)r=px_run(operation==PI_INSTALL);}
        else if(operation==0){if(px->state.phase)r=px_rederive(px->state.operation,true);if(!r)r=px_run(false);}
        else r=-VFS_EINVAL;
    }
    __atomic_store_n(&px->busy,0,__ATOMIC_RELEASE);return r;
}
