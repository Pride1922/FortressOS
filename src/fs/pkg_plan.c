#include "pkg_engine_config.h"
#if FORTRESS_PKG_ENGINE
#include "pkg_plan.h"
#include "crypto.h"
#include "string.h"
static bool span(const void *p,size_t n){return p&&n<=UINTPTR_MAX-(uintptr_t)p;}
static bool apart(const void *p,size_t n,const void *q,size_t m){return span(p,n)&&span(q,m)&&((uintptr_t)p+n<=(uintptr_t)q||(uintptr_t)q+m<=(uintptr_t)p);}
static bool zero(const void *p,size_t n){const uint8_t *b=p;for(size_t i=0;i<n;i++)if(b[i])return false;return true;}
static void put(uint8_t *p,unsigned n,uint64_t v){for(unsigned i=0;i<n;i++)p[i]=(uint8_t)(v>>(8*i));}
static uint64_t blocks(uint64_t n,uint64_t bs){return n/bs+(n%bs!=0);}
/* Worst-case one extent per data block, with old and replacement depth-2 map
 * nodes coexisting until checkpoint. Inline <=4-extent maps need no blocks. */
static uint64_t map_peak(uint64_t data,uint64_t bs){
    if(data<=4)return 0;
    uint64_t capacity=(bs-12)/12,leaves=blocks(data,capacity);
    return 2*(leaves+(leaves>4?blocks(leaves,capacity):0));
}
static bool equal_object(const pi_object *o,const pi_object *owned,const fpk_file *f){
    return o->inode>=11&&o->generation&&o->inode==owned->inode&&o->generation==owned->generation&&
        o->mode==f->mode&&o->mode==owned->mode&&!o->uid&&!o->gid&&!owned->uid&&!owned->gid&&
        o->links==1&&owned->links==1&&o->type==1&&owned->type==1&&o->size==f->size&&o->size==owned->size&&
        o->blocks==owned->blocks&&!memcmp(o->hash,f->sha256,32)&&!memcmp(o->hash,owned->hash,32);
}
static unsigned parents(const fpk_manifest *m){
    unsigned result=0;
    for(unsigned i=0;i<m->file_count;i++){
        const fpk_file *f=m->files+i;if(f->role!=2)continue;
        for(unsigned at=3;at<f->path.length;at++)if(f->path.bytes[at]=='/'){
            bool seen=false;
            for(unsigned j=0;j<i;j++){const fpk_file *p=m->files+j;if(p->role==2&&p->path.length>at&&p->path.bytes[at]=='/'&&!memcmp(p->path.bytes,f->path.bytes,at))seen=true;}
            if(!seen)result++;
        }
    }
    return result?result+2:0; /* package and immutable build parents, then doc/... */
}
static bool valid_plan(const pi_plan *p){
    if(!p||p->operation<PI_INSTALL||p->operation>PI_NOOP||!p->sequence||p->sequence==UINT64_MAX||
       !p->generation||p->generation==UINT64_MAX||!p->build||!p->root||!p->catalog||!p->artifact_bytes||
       !p->file_count||p->file_count>16||p->parent_count>800||p->reserved||
       !fpk_identity_valid(p->package,p->package_length)||!fpk_identity_valid(p->command,p->command_length)||
       !zero(p->package+p->package_length,32-p->package_length)||!zero(p->command+p->command_length,32-p->command_length)||
       zero(p->platform,32)||zero(p->artifact,32)||zero(p->catalog_hash,32)||zero(p->previous,32)||zero(p->base_hash,32))return false;
    for(unsigned i=0;i<16;i++){
        const pi_file *f=p->files+i;
        if(i>=p->file_count){if(!zero(f,sizeof *f))return false;continue;}
        if(f->reserved||!f->size||!f->blocks||!f->path_length||f->path_length>100||!zero(f->path+f->path_length,100-f->path_length)||zero(f->hash,32)||
           (f->role!=1&&f->role!=2)||f->mode!=(f->role==1?0755:0644))return false;
    }
    return true;
}
int pi_encode(const pi_plan *p,uint8_t out[PI_WIRE]){
    if(!apart(p,sizeof *p,out,PI_WIRE)||!valid_plan(p))return -1;
    memset(out,0,PI_WIRE);memcpy(out,"FORT-PKG-PLAN2",14);put(out+16,4,PI_WIRE);put(out+20,2,2);put(out+22,2,p->operation);
    put(out+24,8,p->sequence);put(out+32,8,p->generation);put(out+40,8,p->build);put(out+48,8,p->root);put(out+56,8,p->catalog);put(out+64,8,p->artifact_bytes);
    put(out+72,8,p->total[0]);put(out+80,8,p->total[1]);put(out+88,8,p->memory_bytes);put(out+96,8,p->journal_images);put(out+104,8,p->orphan_slots);put(out+112,4,p->parent_count);
    put(out+116,2,p->package_length);put(out+118,2,p->command_length);put(out+120,2,p->file_count);
    memcpy(out+128,p->package,32);memcpy(out+160,p->command,32);memcpy(out+192,p->platform,32);memcpy(out+224,p->artifact,32);memcpy(out+256,p->catalog_hash,32);memcpy(out+288,p->previous,32);memcpy(out+320,p->base_hash,32);
    for(unsigned i=0;i<p->file_count;i++){
        const pi_file *f=p->files+i;uint8_t *r=out+512+256*i;put(r,2,f->role);put(r+2,2,f->mode);put(r+4,2,f->path_length);put(r+8,8,f->size);put(r+16,8,f->blocks);memcpy(r+24,f->hash,32);put(r+56,4,f->inode);put(r+60,4,f->generation);memcpy(r+64,f->path,100);
    }
    return crypto_sha256(out,PI_WIRE-32,out+PI_WIRE-32);
}
int pi_prepare(unsigned operation,const uint8_t *catalog,size_t catalog_n,const fpk_catalog_policy *policy,
               const uint8_t *package,size_t package_n,fpk_read_fn read,void *reader,const pi_inventory *inventory,
               const pi_object observed[16],const pi_resources *resources,pi_workspace *w,pi_plan *out){
    if(!read||!reader||!span(policy,sizeof *policy)||!span(inventory,sizeof *inventory)||!span(observed,16*sizeof *observed)||!span(resources,sizeof *resources)||
       !apart(w,sizeof *w,out,sizeof *out))return -1;
    const void *inputs[]={catalog,policy,package,inventory,observed,resources};
    const size_t sizes[]={catalog_n,sizeof *policy,package_n,sizeof *inventory,16*sizeof *observed,sizeof *resources};
    for(unsigned i=0;i<6;i++)if(!apart(w,sizeof *w,inputs[i],sizes[i])||!apart(out,sizeof *out,inputs[i],sizes[i]))return -1;
    if(!resources->base_count||resources->base_count>256||!apart(w,sizeof *w,resources->base_names,resources->base_count*sizeof(fpk_span))||!apart(out,sizeof *out,resources->base_names,resources->base_count*sizeof(fpk_span)))return -1;
    for(size_t i=0;i<resources->base_count;i++){
        fpk_span s=resources->base_names[i];if(!apart(w,sizeof *w,s.bytes,s.length)||!apart(out,sizeof *out,s.bytes,s.length))return -1;
    }
    w->verify.verified=0;
    for(size_t i=0;i<resources->base_count;i++){
        fpk_span s=resources->base_names[i];if(!fpk_identity_valid(s.bytes,s.length))goto fail;
        for(size_t j=0;j<i;j++)if(s.length==resources->base_names[j].length&&!memcmp(s.bytes,resources->base_names[j].bytes,s.length))goto fail;
    }
    if((operation!=PI_INSTALL&&operation!=PI_REMOVE)||!fpk_identity_valid(package,package_n)||
       !inventory->sequence||inventory->sequence==UINT64_MAX||!inventory->generation||inventory->generation==UINT64_MAX||
       inventory->installed>1||inventory->file_count>16||inventory->inventory_bytes<512||inventory->inventory_bytes>PI_INVENTORY_MAX||
       zero(inventory->hash,32)||(inventory->build_floor?zero(inventory->floor_artifact,32):!zero(inventory->floor_artifact,32))||resources->block_size<1024||resources->block_size>4096||(resources->block_size&(resources->block_size-1)))goto fail;
    if(!resources->capacity[0]||resources->capacity[0]>(UINT64_C(1)<<40)/resources->block_size||!resources->capacity[1]||resources->capacity[1]>1048576||
       resources->free[0]>resources->capacity[0]||resources->free[1]>resources->capacity[1]||resources->remaining[0]>resources->free[0]||resources->remaining[1]>resources->free[1]||
       fpk_verify_authorized(&w->verify,catalog,catalog_n,policy,package,package_n,read,reader))goto fail;
    const fpk_manifest *m=&w->verify.manifest;
    if(inventory->package_length){
        if(!fpk_identity_valid(inventory->package,inventory->package_length)||inventory->package_length!=package_n||memcmp(inventory->package,package,package_n)||
           !zero(inventory->package+inventory->package_length,32-inventory->package_length)||!fpk_identity_valid(inventory->command,inventory->command_length)||
           !zero(inventory->command+inventory->command_length,32-inventory->command_length)||inventory->command_length!=m->command.length||memcmp(inventory->command,m->command.bytes,m->command.length))goto fail;
    }else if(inventory->installed||inventory->build_floor||inventory->command_length||!zero(inventory->package,32)||!zero(inventory->command,32))goto fail;
    if(memcmp(m->platform_id,resources->platform,32)||m->base_min>resources->base||m->base_max<resources->base||m->build<inventory->build_floor)goto fail;
    for(size_t i=0;i<resources->base_count;i++)if(resources->base_names[i].length==m->command.length&&!memcmp(resources->base_names[i].bytes,m->command.bytes,m->command.length))goto fail;
    /* Resolve the independently verified catalog target again; authority never
     * comes from a caller-supplied verified flag or a borrowed scratch hash. */
    fpk_catalog verified;fpk_catalog_target target;
    if(fpk_catalog_verify(catalog,catalog_n,policy,&verified)||fpk_catalog_find(&verified,package,package_n,&target))goto fail;
    if(m->build==inventory->build_floor&&memcmp(target.sha256,inventory->floor_artifact,32))goto fail;
    if(inventory->installed){
        if(inventory->file_count!=m->file_count||inventory->build!=m->build||inventory->build_floor<inventory->build||memcmp(inventory->artifact,target.sha256,32))goto fail; /* upgrades separate */
        for(unsigned i=0;i<m->file_count;i++)if(!equal_object(observed+i,inventory->owned+i,m->files+i)||observed[i].blocks!=blocks(m->files[i].size,resources->block_size))goto fail;
        for(unsigned i=0;i<m->file_count;i++)for(unsigned j=0;j<i;j++)if(observed[i].inode==observed[j].inode)goto fail;
        for(unsigned i=m->file_count;i<16;i++)if(!zero(observed+i,sizeof *observed)||!zero(inventory->owned+i,sizeof *observed))goto fail;
    }else{
        if(operation==PI_REMOVE||inventory->file_count||inventory->build||!zero(inventory->artifact,32)||!zero(inventory->owned,sizeof inventory->owned))goto fail;
        for(unsigned i=0;i<16;i++)if(!zero(observed+i,sizeof *observed))goto fail;
    }
    memset(&w->candidate,0,sizeof w->candidate);pi_plan *p=&w->candidate;
    p->operation=operation==PI_INSTALL&&inventory->installed?PI_NOOP:operation;p->file_count=m->file_count;p->parent_count=parents(m);
    p->sequence=inventory->sequence+1;p->generation=inventory->generation+1;p->build=m->build;p->root=policy->root_version;p->catalog=verified.revision;p->artifact_bytes=target.length;
    if(p->operation==PI_NOOP){p->sequence=inventory->sequence;p->generation=inventory->generation;}
    p->package_length=m->package.length;p->command_length=m->command.length;memcpy(p->package,m->package.bytes,m->package.length);memcpy(p->command,m->command.bytes,m->command.length);
    memcpy(p->platform,resources->platform,32);memcpy(p->artifact,target.sha256,32);memcpy(p->catalog_hash,verified.sha256,32);memcpy(p->previous,inventory->hash,32);
    crypto_sha256_ctx ns;uint8_t length[2];crypto_sha256_init(&ns);
    for(size_t i=0;i<resources->base_count;i++){put(length,2,resources->base_names[i].length);crypto_sha256_update(&ns,length,2);crypto_sha256_update(&ns,resources->base_names[i].bytes,resources->base_names[i].length);}
    crypto_sha256_final(&ns,p->base_hash);
    uint64_t data=0,executable=0,map_blocks=0;
    for(unsigned i=0;i<m->file_count;i++){
        const fpk_file *f=m->files+i;pi_file *t=p->files+i;t->role=f->role;t->mode=f->mode;t->size=f->size;t->blocks=blocks(f->size,resources->block_size);t->path_length=f->path.length;
        memcpy(t->path,f->path.bytes,f->path.length);memcpy(t->hash,f->sha256,32);if(operation==PI_REMOVE){t->inode=observed[i].inode;t->generation=observed[i].generation;}
        data+=t->blocks;map_blocks+=map_peak(t->blocks,resources->block_size);if(f->role==1)executable=t->blocks;
    }
    if(p->operation!=PI_NOOP){
        uint64_t bs=resources->block_size,inventory_cost=blocks(PI_INVENTORY_MAX,bs);
        /* Full archive, authoritative members, separate activation, complete next
         * inventory + recovery recreation, intent, directory growth and bounded
         * metadata slack. Reservation includes all blocks, not payload bytes. */
        p->total[0]=8+2*(inventory_cost+map_peak(inventory_cost,bs))+blocks(PI_WIRE,bs);
        p->total[1]=4;
        if(operation==PI_INSTALL){uint64_t archive=blocks(target.length,bs);p->total[0]+=archive+map_peak(archive,bs)+data+map_blocks+executable+map_peak(executable,bs)+2*(p->parent_count+m->file_count);p->total[1]+=m->file_count+p->parent_count+2;}
        p->memory_bytes=2*PI_INVENTORY_MAX+sizeof *w+128*1024+64*bs;
        p->journal_images=64;p->orphan_slots=16;
        if(p->total[0]>(32u*1024u*1024u)/bs||resources->memory_bytes<p->memory_bytes||resources->journal_images<p->journal_images||resources->orphan_slots<p->orphan_slots)goto fail;
        const uint64_t reserve[2]={blocks(256*1024,bs),16};
        for(unsigned i=0;i<2;i++)if(resources->remaining[i]>resources->free[i]||reserve[i]>resources->free[i]-resources->remaining[i]||p->total[i]>resources->free[i]-resources->remaining[i]-reserve[i])goto fail;
    }
    if(pi_encode(p,w->wire))goto fail;
    memcpy(p->hash,w->wire+PI_WIRE-32,32);*out=*p;return 0;
fail:w->verify.verified=0;memset(&w->candidate,0,sizeof w->candidate);memset(w->wire,0,sizeof w->wire);return -1;
}

#endif
