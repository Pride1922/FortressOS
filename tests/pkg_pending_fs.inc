/* Private copied EXT4 translation unit. The filesystem stays detached. */
#include "pkg_pending_candidate.h"
static pp_workspace pn_workspace;
static pp_input pn_input;
static pp_view pn_view;
static pp_change pn_change;
static pp_object pn_objects[4],pn_executable,pn_archive;
static uint8_t pn_baseline[PS_BUNDLE_MAX],pn_artifact[8192],pn_intent[1024],pn_scratch[8192];
static size_t pn_baseline_n,pn_artifact_n,pn_intent_n;
static uint64_t pn_grant_floor,pn_state_floor;
static ext4_engine_t *pn_bound;
static bool pn_typed;
static const char *pn_paths[]={"/pkg/transaction.v1/archive","/pkg/transaction.v1/activation","/pkg/transaction.v1/next.v1","/pkg/transaction.v1/pending.v1"};
static unsigned pn_roles[]={1,17,18,19};
static const char *pn_names_short[]={"archive","activation","next.v1","pending.v1"};
static int pn_observe(ext4_mount_t *fs,const char *path,unsigned role,pp_object *out){
    size_t length;int r=current_read_file(fs,path,role==17?0755:0600,pn_scratch,&length);if(r)return r;
    e4_inode_t in;r=identity_path(fs,path,&in);if(r)return r;r=e4_mapping(&in);if(r)return r;
    memset(out,0,sizeof *out);out->role=role;out->inode=in.ino;out->generation=in.generation;out->mode=in.full_mode&07777;
    out->uid=in.uid;out->gid=in.gid;out->links=1;out->size=length;out->count=in.map->count;
    if(crypto_sha256(pn_scratch,length,out->hash))return -VFS_EIO;
    for(unsigned i=0;i<out->count;i++){e4_extent_t x=in.map->entries[i];out->extent[i].logical=x.logical;out->extent[i].length=x.len;out->extent[i].physical=x.physical;out->blocks+=x.len;}
    return 0;
}
/* Exact bounded directories: missing charged names are refined by OWN below;
 * extra names, duplicate entries, aliases, wrong dot ownership fail closed. */
static int pn_names(ext4_mount_t *fs){
    static const char *paths[]={"/pkg","/pkg/transaction.v1","/usr/bin","/usr/share/pkg"};
    static const char *parents[]={"/","/pkg","/usr","/usr/share"};
    static const char *names[][10]={{"enrollment.v1","grant.v1","ownership.v1","transaction.v1","origin.v1","account-generation.v1","state.v1","trust.v1","descriptor.v2","artifact.fpk"},{"archive","activation","next.v1","pending.v1"},{"demo"},{NULL}};
    for(unsigned d=0;d<4;d++){
        e4_inode_t in,parent;int r=identity_path(fs,paths[d],&in);if(!r)r=identity_path(fs,parents[d],&parent);if(r)return r;
        if(in.mode!=VFS_S_IFDIR||in.uid||in.gid||(in.full_mode&07777)!=(d<2?0700:0755)||parent.mode!=VFS_S_IFDIR||parent.uid||parent.gid)return -VFS_EACCES;
        unsigned count=d==0?10:d==1?4:d==2?1:0,seen=0;
        for(unsigned i=0;i<count+3;i++){
            vfs_dirent_t entry;uint32_t found=0;r=e4_scan(&in,NULL,i,&entry,&found);if(r<0)return r;
            if(!found){if((seen&3)!=3 || (d==0 && seen!=4095))return -VFS_EINVAL;break;}
            unsigned bit;
            if(!strcmp(entry.name,".")){bit=0;if(found!=in.ino||entry.type!=VFS_DIRECTORY)return -VFS_EINVAL;}
            else if(!strcmp(entry.name,"..")){bit=1;if(found!=parent.ino||entry.type!=VFS_DIRECTORY)return -VFS_EINVAL;}
            else{unsigned child=0;while(child<count&&strcmp(entry.name,names[d][child]))child++;if(child==count||entry.type!=(d==0&&child==3?VFS_DIRECTORY:VFS_FILE))return -VFS_EINVAL;bit=child+2;}
            if((seen&(1u<<bit))||i==count+2)return -VFS_EINVAL;
            seen|=1u<<bit;
        }
    }
    return 0;
}
static int pn_load(ext4_mount_t *fs){
    int r=pn_names(fs);if(r)return r;
    pn_artifact_n=pn_intent_n=0;
    r=current_read_file(fs,"/pkg/artifact.fpk",0600,pn_artifact,&pn_artifact_n);if(r)return r;
    memset(&pn_input,0,sizeof pn_input);pn_input.authority=(pc_input){.selector={jb_selector,jb_selector_n},.baseline={pn_baseline,pn_baseline_n},.current={jb_bundle,jb_bundle_n},.artifact={pn_artifact,pn_artifact_n},.policy=&jb_policy,.grant_floor=pn_grant_floor,.state_floor=pn_state_floor};
    r=pn_observe(fs,"/pkg/artifact.fpk",1,&pn_archive);if(r)return r;
    r=pn_observe(fs,"/usr/bin/demo",17,&pn_executable);
    if(!r)pn_input.executable=&pn_executable;else if(r!=-VFS_ENOENT)return r;
    for(unsigned i=0;i<4;i++){
        pp_object *o=pn_objects+pn_input.object_count;
        r=pn_observe(fs,pn_paths[i],pn_roles[i],o);
        if(r==-VFS_ENOENT){
            if(i==1&&pn_input.executable)*o=pn_executable;else continue;
        }else if(r)return r;
        else if(i==1&&pn_input.executable)return -VFS_EINVAL;
        if(i==3){memcpy(pn_intent,pn_scratch,1024);pn_intent_n=1024;}
        pn_input.object_count++;
    }
    /* Published activation becomes baseline after FINISH; no longer in OWN. */
    if(!e4_u16(jb_bundle+96+2656+512+22)){
        if(pn_input.object_count && (pn_input.object_count!=1 || pn_objects[0].role!=17 || !pn_input.executable))return -VFS_EINVAL;
        pn_input.object_count=0;
    }
    pn_input.objects=pn_objects;pn_input.intent=(ps_blob){pn_intent,pn_intent_n};
    uint8_t *sb=fs->scratch;if(!e4_bytes(fs,1024,sb,1024))return -VFS_EIO;
    pn_input.free[0]=e4_u32(sb+12);pn_input.free[1]=e4_u32(sb+16);pn_input.capacity[0]=fs->blocks;pn_input.capacity[1]=e4_u32(sb);
    if(pp_validate(&pn_input,&pn_workspace,&pn_view))return -VFS_EACCES;
    return 0;
}
static uint64_t pn_planned_free(ext4_engine_t *e,bool inode){
    uint64_t total=0;ext4_mount_t *fs=e->fs;
    for(unsigned g=0;g<fs->groups;g++){
        const uint8_t *raw=fs->gd[g].raw;unsigned block=fs->first+1+g*32/fs->bs;
        for(unsigned i=0;i<e->images;i++)if(e->image[i].block==block)raw=e->image[i].bytes+(g*32)%fs->bs;
        total+=e4_u16(raw+(inode?14:12));
    }
    return total;
}
static int pn_stamp(ext4_engine_t *e,const char *path,const uint8_t *bytes,size_t n){
    e4_inode_t in;int r=identity_path(e->fs,path,&in);if(r)return r;r=e4_mapping(&in);if(r)return r;
    size_t at=0;
    for(unsigned i=0;i<in.map->count;i++)for(unsigned j=0;j<in.map->entries[i].len;j++){
        uint8_t *p=e4_stage(e,in.map->entries[i].physical+j,E4_NEW);if(!p)return e->error;
        size_t take=n-at;if(take>e->fs->bs)take=e->fs->bs;memcpy(p,bytes+at,take);at+=take;
    }
    return at==n?0:-VFS_EIO;
}
static bool pn_object_block(const pp_object *o,uint32_t block){
    for(unsigned i=0;i<o->count;i++)if(block>=o->extent[i].physical&&block-o->extent[i].physical<o->extent[i].length)return true;
    return false;
}
/* Ordinary operations cannot edit reserved contents, inode identities or free
 * their bitmap bits, and cannot spend durable package or account credits. */
static int pn_fence_object(ext4_engine_t *e,const pp_object *o){
    ext4_mount_t *fs=e->fs;unsigned group=(o->inode-1)/fs->ipg,index=(o->inode-1)%fs->ipg;
    uint64_t slot=(uint64_t)e4_u32(fs->gd[group].raw+8)*fs->bs+(uint64_t)index*256;
    for(unsigned i=0;i<e->images;i++){
        e4_image_t *im=e->image+i;if(pn_object_block(o,im->block))return -VFS_EACCES;
        if(im->block==slot/fs->bs){if(!e4_bytes(fs,slot,pn_scratch,256)||memcmp(pn_scratch,im->bytes+slot%fs->bs,256))return -VFS_EACCES;}
        if(im->block==e4_u32(fs->gd[group].raw+4)&&!(im->bytes[index/8]&(1u<<(index%8))))return -VFS_EACCES;
        for(unsigned j=0;j<o->count;j++)for(unsigned k=0;k<o->extent[j].length;k++){
            uint64_t block=o->extent[j].physical+k;unsigned bg=(block-fs->first)/fs->bpg,bit=(block-fs->first)%fs->bpg;
            if(im->block==e4_u32(fs->gd[bg].raw)&&!(im->bytes[bit/8]&(1u<<(bit%8))))return -VFS_EACCES;
        }
    }
    return 0;
}
static int e4_semantic_commit(ext4_engine_t *e){
    if(e!=pn_bound)return pn_core_commit(e);
    if(e->tainted)return -VFS_EIO;
    if(pn_typed){
        for(unsigned i=0;i<2;i++)if(pn_planned_free(e,i)!=pn_change.free_after[i])return -VFS_EIO;
        const pp_object *freed=NULL;for(unsigned i=0;i<pn_input.object_count;i++)if(pn_objects[i].role==pn_change.role)freed=pn_objects+i;
        bool deleting=pn_change.action==PP_DELETE||(pn_change.action==PP_FINISH&&pn_change.role);
        uint64_t blocks=0;
        for(unsigned i=0;i<e->frees;i++)for(unsigned j=0;j<e->released[i].len;j++){
            if(!deleting||!freed||!pn_object_block(freed,e->released[i].lo+j))return -VFS_EIO;
            blocks++;
        }
        if(deleting?(!freed||blocks!=freed->blocks||e->release_ino!=freed->inode):(blocks||e->release_ino))return -VFS_EIO;
        int r=pn_stamp(e,"/pkg/grant.v1",pn_change.grant,1024);
        if(!r)r=pn_stamp(e,"/pkg/ownership.v1",pn_change.owners,8192);
        if(!r)r=pn_stamp(e,"/pkg/account-generation.v1",pn_change.account,1024);
        if(!r)r=pn_stamp(e,"/pkg/state.v1",pn_change.state,1024);
        if(r)return r;
    }else{
        for(unsigned i=0;i<2;i++){uint64_t free=pn_planned_free(e,i);if(free<pn_view.budget.remaining[i]||free-pn_view.budget.remaining[i]<pn_view.budget.reserve[i])return -VFS_ENOSPC;}
        const uint8_t *en=pn_workspace.baseline.parts[0].p;
        for(unsigned a=0;a<20;a++){
            const uint8_t *row=a<16?en+416+128*a:jb_witness+32+128*a;pp_object o;memset(&o,0,sizeof o);o.inode=e4_u32(row+4);o.count=e4_u16(row+32);
            for(unsigned j=0;j<o.count;j++){o.extent[j].physical=e4_u32(row+48+16*j);o.extent[j].length=e4_u32(row+44+16*j);}
            int r=pn_fence_object(e,&o);if(r)return r;
        }
        for(unsigned i=0;i<pn_input.object_count;i++){int r=pn_fence_object(e,pn_objects+i);if(r)return r;}
        int r=pn_fence_object(e,&pn_archive);if(r)return r;
        if(pn_input.executable){r=pn_fence_object(e,&pn_executable);if(r)return r;}
    }
    return pn_core_commit(e);
}
/* One bounded non-orphan removal. No VFS handle or cached reference survives
 * into this path. All frees and record refunds share the semantic transaction. */
static int pn_remove_plan(ext4_engine_t *e,const pp_object *o,const char *name){
    ext4_mount_t *fs=e->fs;
    if(o->inode<11||o->inode==fs->journal_ino)return -VFS_EINVAL;
    for(unsigned i=0;i<fs->nodes;i++)if(fs->cached[i]->inode.ino==o->inode&&(fs->cached[i]->opens||fs->cached[i]->refs||fs->cached[i]->legacy))return -VFS_EAGAIN;
    for(unsigned i=0;i<16;i++)if(e->handles[i].ino==o->inode&&e->handles[i].refs)return -VFS_EAGAIN;
    e4_inode_t parent,in;int r=identity_path(fs,"/pkg/transaction.v1",&parent);if(r)return r;
    uint32_t found=0;r=e4_scan(&parent,name,0,NULL,&found);if(r<0)return r;if(found!=o->inode)return -VFS_EIO;
    uint8_t *raw;r=e4_load_plan(e,o->inode,&in,&raw);if(r)return r;
    if(in.mode!=VFS_S_IFREG||in.generation!=o->generation||e4_u16(raw+26)!=1||e->old_count)return -VFS_EINVAL;
    for(unsigned i=0;i<fs->pending_count;i++)if(!e4_release(e,fs->pending[i].physical,fs->pending[i].len))return e->error;
    unsigned g=(o->inode-1)/fs->ipg,bit=(o->inode-1)%fs->ipg;uint8_t *bitmap=e4_stage_bitmap(e,g,true);if(!bitmap)return e->error;
    if(!(bitmap[bit/8]&(1u<<(bit%8))))return -VFS_EIO;
    e4_set_bit(bitmap,bit,false);if(!e4_account(e,g,true,1))return e->error;e->release_ino=o->inode;
    memset(raw,0,256);e4_p32(raw+100,o->generation);e4_p16(raw+128,32);e4_inode_sum(fs,o->inode,raw);
    return e4_dir_edit(e,&parent,name,0,0);
}
static int pn_step(ext4_engine_t *e){
    if(e!=pn_bound||e->tainted||e->orphan_count)return -VFS_EIO;
    if(pn_view.action==PP_DONE)return 0;
    for(unsigned a=0;a<pn_input.object_count;a++){
        unsigned ino=pn_objects[a].inode;
        for(unsigned i=0;i<e->fs->nodes;i++)if(e->fs->cached[i]->inode.ino==ino&&(e->fs->cached[i]->opens||e->fs->cached[i]->refs||e->fs->cached[i]->legacy))return -VFS_EAGAIN;
        for(unsigned i=0;i<16;i++)if(e->handles[i].ino==ino&&e->handles[i].refs)return -VFS_EAGAIN;
    }
    if(pp_next(&pn_input,&pn_workspace,&pn_view,&pn_change))return -VFS_EACCES;
    int r=0;if(!e->dirty)r=e4_journal_mark(e,false);if(r)return r;
    r=e4_namespace_begin(e);if(r)return r;pn_typed=true;
    if(pn_change.action==PP_DELETE||(pn_change.action==PP_FINISH&&pn_change.role)){
        unsigned i=0;while(i<pn_input.object_count&&pn_objects[i].role!=pn_change.role)i++;
        if(i==pn_input.object_count)r=-VFS_EIO;else{
            unsigned path=0;while(path<4&&pn_roles[path]!=pn_change.role)path++;
            r=path==4?-VFS_EIO:pn_remove_plan(e,pn_objects+i,pn_names_short[path]);
        }
    }else if(pn_change.action==PP_ACTIVATE){
        e4_inode_t parent,dest;r=identity_path(e->fs,"/pkg/transaction.v1",&parent);if(!r)r=identity_path(e->fs,"/usr/bin",&dest);bool dir=false;
        if(!r)r=e4_rename_plan(e,parent.ino,"activation",dest.ino,"demo",&dir);
        if(!r&&dir)r=-VFS_EINVAL;
    }
    r=e4_namespace_end(e,r);pn_typed=false;return r;
}
