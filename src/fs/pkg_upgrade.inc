/* Ordinary-root upgrade: all data is staged before one executable+inventory
 * activation in the existing EXT4 replacement transaction. JBD2 replay chooses
 * old or new together. No network or catalog is required for replay. */
typedef struct {
    px_context *old,*next;
    px_record record;
    uint8_t encoded[8192],old_hash[32],scratch[16384];
    uint32_t blocks[2],source,victim;
    uintptr_t actor;
} pu_context;
/* Accessed only under e4_lock; cleared under that lock before freeing. */
static pu_context *pu_activation;

static int pu_check(px_context *c) {
    if(px)return -VFS_EAGAIN;
    px=c;int r=px_validate(c->fs);px=NULL;return r;
}
static int pu_file_check(pu_context *u,unsigned i) {
    px_context *c=u->next;e4_inode_t in;
    int r=px_lookup(c->fs,c->files[i],&in);if(r)return r;
    if(in.ino!=u->record.files[i].inode||in.generation!=u->record.files[i].generation||
       in.mode!=VFS_S_IFREG||(in.full_mode&07777)!=c->shape.files[i].mode||in.uid||in.gid||
       in.size!=c->shape.files[i].size)return -VFS_EACCES;
    uint8_t raw[256];r=px_inode_bytes(c->fs->engine,in.ino,raw,false);
    if(r)return r;
    if(e4_u16(raw+26)!=1)return -VFS_EACCES;
    r=e4_mapping(&in);if(r)return r;
    uint64_t at=0;
    for(unsigned j=0;j<in.map->count;j++) {
        e4_extent_t x=in.map->entries[j];
        if(x.unwritten||(uint64_t)x.logical*4096!=at)return -VFS_EACCES;
        uint64_t bytes=(uint64_t)x.len*4096;if(bytes>in.size-at)bytes=in.size-at;
        for(uint64_t k=0;k<bytes;) {
            size_t n=bytes-k;if(n>sizeof u->scratch)n=sizeof u->scratch;
            if(!e4_bytes(c->fs,(uint64_t)x.physical*4096+k,u->scratch,n))return -VFS_EIO;
            if(memcmp(u->scratch,c->payload+c->offsets[i]+at+k,n))return -VFS_EACCES;
            k+=n;
        }
        at+=bytes;
    }
    return at==in.size?0:-VFS_EACCES;
}
static int pu_replace_plan(ext4_engine_t *e,uint32_t source,uint32_t victim) {
    pu_context *u=pu_activation;
    if(!u||e->fs!=u->old->fs||u->actor!=px_default_actor())return 0;
    if(source!=u->source||victim!=u->victim||!px_default_authorized())return -VFS_EACCES;
    int r=pu_check(u->old);if(r)return r;
    uint8_t hash[32];(void)crypto_sha256(u->old->raw,8192,hash);
    if(memcmp(hash,u->old_hash,32))return -VFS_EAGAIN;
    /* Recheck all staged bytes and inode identities under the same exclusion
     * as activation. Public docs are immutable by package policy, not trusted
     * just because an earlier callback verified them. */
    for(unsigned i=0;i<u->next->shape.file_count;i++){r=pu_file_check(u,i);if(r)return r;}
    for(unsigned i=0;i<u->next->directory_count;i++) {
        e4_inode_t in;r=px_lookup(e->fs,u->next->directories[i],&in);if(r)return r;
        if(in.ino!=u->record.directories[i].inode||in.generation!=u->record.directories[i].generation||
           in.mode!=VFS_S_IFDIR||in.uid||in.gid||(in.full_mode&07777)!=0755)return -VFS_EACCES;
    }
    for(unsigned i=0;i<2;i++) {
        uint8_t *b=e4_stage(e,u->blocks[i],E4_NEW);if(!b)return e->error;
        memcpy(b,u->encoded+4096*i,4096);
    }
    return 0;
}
static void pu_drop(px_context *c) {
    if(!c)return;
    kfree(c->artifact);kfree(c->catalog);kfree(c->payload);kfree(c->nodes);kfree(c);
}
static int pu_shape(px_context *c) {
    const fpk_manifest *m=&c->verify.verify.manifest;
    c->shape.file_count=m->file_count;c->shape.build=m->build;
    c->shape.package_length=m->package.length;c->shape.command_length=m->command.length;
    memcpy(c->shape.package,m->package.bytes,m->package.length);
    memcpy(c->shape.command,m->command.bytes,m->command.length);memcpy(c->shape.platform,m->platform_id,32);
    for(unsigned i=0;i<m->file_count;i++) {
        const fpk_file *f=m->files+i;pi_file *d=c->shape.files+i;
        d->role=f->role;d->mode=f->mode;d->size=f->size;d->blocks=(f->size+4095)/4096;
        d->path_length=f->path.length;memcpy(d->path,f->path.bytes,f->path.length);memcpy(d->hash,f->sha256,32);
    }
    c->payload_n=c->verify.verify.envelope.expanded_bytes;
    c->payload=px_allocate(c->payload_n);if(!c->payload)return -VFS_ENOMEM;
    c->reader_at=128+c->verify.verify.envelope.manifest_bytes;
    if(gzip_decode_callbacks(px_decode_read,px_decode_write,c)||c->decode_at!=c->payload_n||c->reader_at!=c->artifact_n)return -VFS_EACCES;
    size_t at=0;for(unsigned i=0;i<m->file_count;i++){c->offsets[i]=at+512;at+=512+(size_t)((m->files[i].size+511)/512)*512;}
    if(at+1024!=c->payload_n)return -VFS_EACCES;
    int r=px_paths_for(c);c->shape.parent_count=c->directory_count;return r;
}
static int pu_directory(ext4_mount_t *fs,const char *path) {
    int r=px_ensure_dir(fs,path,0755);if(r)return r;
    uint64_t flags=spin_lock_irqsave(&e4_lock);e4_inode_t in;r=px_lookup(fs,path,&in);
    if(!r&&(in.mode!=VFS_S_IFDIR||in.uid||in.gid||(in.full_mode&07777)!=0755))r=-VFS_EACCES;
    spin_unlock_irqrestore(&e4_lock,flags);return r;
}
static int pu_stage(pu_context *u,unsigned i) {
    px_context *c=u->next;
    char parent_path[192],leaf[64];const char *path=c->files[i];size_t n=strlen(path),at=n;
    while(at&&path[at-1]!='/')at--;
    if(!at||n-at>=sizeof leaf)return -VFS_EFBIG;
    memcpy(parent_path,path,at);parent_path[at]=0;memcpy(leaf,path+at,n-at+1);
    creds_t actor;creds_init_root(&actor);int error=0;
    vfs_node_t *parent=vfs_lookup_at_creds(&c->fs->cached[0]->node,parent_path,&actor,&error);
    if(!parent)return error?error:-VFS_ENOENT;
    vfs_node_t *node=vfs_lookup_at_creds(&c->fs->cached[0]->node,path,&actor,&error);
    if(!node&&error==-VFS_ENOENT) {
        vfs_create_attrs_t attrs={.mode=c->shape.files[i].mode,.uid=0,.gid=0};
        node=parent->create_attrs_ref?parent->create_attrs_ref(parent,leaf,VFS_FILE,&attrs,&error):NULL;
    }
    vfs_node_put(parent);if(!node)return error?error:-VFS_EIO;
    vfs_metadata_t meta;int r=vfs_metadata(node,&meta);
    uint64_t size=c->shape.files[i].size;
    if(!r&&(meta.type!=VFS_FILE||meta.uid||meta.gid||(meta.mode&07777)!=c->shape.files[i].mode||meta.size>size))r=-VFS_EACCES;
    uint64_t offset=0;
    /* A pre-activation interruption can leave a prefix. Reuse only byte-exact
     * expected prefixes; foreign content is an error, never truncated/deleted. */
    while(!r&&offset<meta.size) {
        size_t take=meta.size-offset;if(take>sizeof u->scratch)take=sizeof u->scratch;
        int64_t got=node->read?node->read(node,offset,u->scratch,take):-VFS_EOPNOTSUPP;
        if(got!=(int64_t)take)r=got<0?(int)got:-VFS_EIO;
        else if(memcmp(u->scratch,c->payload+c->offsets[i]+offset,take))r=-VFS_EACCES;
        offset+=take;
    }
    while(!r&&offset<size) {
        if(!px_default_authorized()){r=-VFS_EACCES;break;}
        size_t take=size-offset;if(take>16384)take=16384;
        uint64_t position=offset;
        int64_t got=node->write?node->write(node,&position,false,c->payload+c->offsets[i]+offset,take):-VFS_EOPNOTSUPP;
        if(got<=0||(uint64_t)got>take)r=got<0?(int)got:-VFS_EIO;
        else offset+=(uint64_t)got;
    }
    vfs_node_put(node);
    if(!r) {
        uint64_t flags=spin_lock_irqsave(&e4_lock);e4_inode_t in;r=px_lookup(c->fs,path,&in);
        if(!r){u->record.files[i].inode=in.ino;u->record.files[i].generation=in.generation;u->record.files[i].size=in.size;}
        spin_unlock_irqrestore(&e4_lock,flags);
    }
    return r;
}
static int ext4_pkg_upgrade_source(void *context,const uint8_t package[32],
    const uint8_t *catalog,size_t catalog_n,const uint8_t *artifact,size_t artifact_n,
    const uint8_t *previous,size_t previous_n,pkg_transaction_status *status) {
    ext4_mount_t *fs=context?context:e4_active;
    if(!fs||!fs->engine||px||!px_default_authorized())return -VFS_EACCES;
    struct ext4_pkg_staging *st=kcalloc(1,sizeof *st);
    pu_context *u=kcalloc(1,sizeof *u);
    if(!st||!u){kfree(st);kfree(u);return -VFS_ENOMEM;}
    int r=-VFS_ENOMEM;u->old=px_allocate(sizeof *u->old);u->next=px_allocate(sizeof *u->next);
    if(!u->old||!u->next)goto done;
    px_context *old=u->old,*next=u->next;
    old->fs=next->fs=fs;
    size_t trust_n=0;r=px_read_vfs_file(&fs->cached[0]->node,"/etc/pkg/trust.conf",st->trust_wire,sizeof st->trust_wire,&trust_n);
    if(r)goto done;
    timespec_t now={0};
#ifndef HOST_TEST
    time_get_timespec(CLOCK_REALTIME,&now);
#else
    now.tv_sec=1792929600;
#endif
    if(pkg_repository_trust_decode(st->trust_wire,trust_n,(uint64_t)now.tv_sec,&st->trust)){r=-VFS_EACCES;goto done;}
    old->policy.resources.capacity[0]=fs->blocks;old->policy.resources.capacity[1]=fs->inodes;
    uint64_t flags=spin_lock_irqsave(&e4_lock);
    if(!e4_bytes(fs,1128,old->policy.filesystem,16))r=-VFS_EIO;
    else {px=old;r=px_record_read(fs);px=NULL;}
    if(!r){u->blocks[0]=old->record_blocks[0];u->blocks[1]=old->record_blocks[1];}
    spin_unlock_irqrestore(&e4_lock,flags);if(r)goto done;
    if(!old->state.installed||old->state.phase||old->state.sequence==UINT64_MAX-1||old->state.generation==UINT64_MAX-1||memcmp(package,old->state.package,32)){r=-VFS_EACCES;goto done;}
    (void)crypto_sha256(old->raw,8192,u->old_hash);
    old->artifact=px_allocate(previous_n);next->artifact=px_allocate(artifact_n);
    if(!old->artifact||!next->artifact){r=-VFS_ENOMEM;goto done;}
    memcpy(old->artifact,previous,previous_n);old->artifact_n=previous_n;
    memcpy(next->artifact,artifact,artifact_n);next->artifact_n=artifact_n;
    px_input input={old->artifact,previous_n,0};
    if(fpk_verify(&old->verify.verify,px_source_read,&input,previous_n,old->state.floor_hash)){r=-VFS_EACCES;goto done;}
    input=(px_input){next->artifact,artifact_n,0};size_t name_n=0;while(name_n<32&&package[name_n])name_n++;
    if(fpk_verify_authorized(&next->verify.verify,catalog,catalog_n,&st->trust.catalog,package,name_n,px_source_read,&input)){r=-VFS_EACCES;goto done;}
    const fpk_manifest *m=&next->verify.verify.manifest;
    uint8_t hash[32];(void)crypto_sha256(artifact,artifact_n,hash);
    size_t command_n=0;while(command_n<32&&old->state.command[command_n])command_n++;
    if(m->build<old->state.floor||(m->build==old->state.floor&&memcmp(hash,old->state.floor_hash,32))||
       m->command.length!=command_n||memcmp(m->command.bytes,old->state.command,m->command.length)||
       memcmp(m->platform_id,st->trust.platform,32)||m->base_min>st->trust.base_version||m->base_max<st->trust.base_version||
       st->trust.catalog.root_version<old->state.root){r=-VFS_EACCES;goto done;}
    fpk_catalog verified_catalog;
    if(fpk_catalog_verify(catalog,catalog_n,&st->trust.catalog,&verified_catalog)||verified_catalog.revision<old->state.catalog){r=-VFS_EACCES;goto done;}
    if(verified_catalog.revision==old->state.catalog){(void)crypto_sha256(catalog,catalog_n,hash);if(memcmp(hash,old->state.catalog_hash,32)){r=-VFS_EACCES;goto done;}}
    uint64_t memory=2*sizeof(px_context)+sizeof *u+previous_n+artifact_n+
        old->verify.verify.envelope.expanded_bytes+next->verify.verify.envelope.expanded_bytes;
    if(memory>32u*1024*1024){r=-VFS_ENOMEM;goto done;}
    r=pu_shape(old);if(r)goto done;
    old->shape.root=old->state.root;old->shape.catalog=old->state.catalog;
    memcpy(old->shape.artifact,old->state.floor_hash,32);memcpy(old->shape.catalog_hash,old->state.catalog_hash,32);memcpy(old->root_hash,old->state.root_hash,32);
    if(old->shape.build!=old->state.floor||memcmp(old->shape.package,package,32)){r=-VFS_EACCES;goto done;}
    flags=spin_lock_irqsave(&e4_lock);r=pu_check(old);spin_unlock_irqrestore(&e4_lock,flags);if(r)goto done;
    if(m->build==old->state.floor){*status=(pkg_transaction_status){.phase=PKG_TX_IDLE,.sequence=old->state.sequence,.account_sequence=1,.credential_generation=1};r=0;goto done;}
    r=pu_shape(next);if(r)goto done;
    u->record=old->state;u->record.sequence++;u->record.generation++;
    u->record.floor=m->build;u->record.root=st->trust.catalog.root_version;u->record.catalog=verified_catalog.revision;
    (void)crypto_sha256(artifact,artifact_n,u->record.floor_hash);(void)crypto_sha256(catalog,catalog_n,u->record.catalog_hash);
    uint8_t *root=u->scratch;memcpy(root,st->trust.catalog.repository,16);memcpy(root+16,st->trust.catalog.keys,256);
    px_put(root+272,4,st->trust.catalog.key_count);px_put(root+276,4,st->trust.catalog.threshold);px_put(root+280,8,st->trust.catalog.root_version);
    (void)crypto_sha256(root,288,u->record.root_hash);
    if(u->record.root==old->state.root&&memcmp(u->record.root_hash,old->state.root_hash,32)){r=-VFS_EACCES;goto done;}
    u->record.made=u->record.published=(1u<<m->file_count)-1;
    memset(u->record.files,0,sizeof u->record.files);memset(u->record.directories,0,sizeof u->record.directories);
    unsigned executable=16;uint64_t required=64+next->directory_count;
    for(unsigned i=0;i<m->file_count;i++){required+=(m->files[i].size+4095)/4096+8;if(m->files[i].role==1)executable=i;}
    flags=spin_lock_irqsave(&e4_lock);
    bool room=px_free(fs->engine,false,false)>required&&px_free(fs->engine,true,false)>m->file_count+next->directory_count+16;
    spin_unlock_irqrestore(&e4_lock,flags);
    if(!room){r=-VFS_ENOSPC;goto done;}
    if(executable==16){r=-VFS_EACCES;goto done;}
    for(unsigned i=0;i<next->directory_count;i++) {
        r=pu_directory(fs,next->directories[i]);if(r)goto done;
        flags=spin_lock_irqsave(&e4_lock);e4_inode_t in;r=px_lookup(fs,next->directories[i],&in);
        if(!r){u->record.directories[i].inode=in.ino;u->record.directories[i].generation=in.generation;}
        spin_unlock_irqrestore(&e4_lock,flags);if(r)goto done;
    }
    r=px_ensure_dir(fs,"/var/lib/pkg/upgrade-v3",0700);if(r)goto done;
    flags=spin_lock_irqsave(&e4_lock);e4_inode_t staging;r=px_lookup(fs,"/var/lib/pkg/upgrade-v3",&staging);
    if(!r&&(staging.mode!=VFS_S_IFDIR||staging.uid||staging.gid||(staging.full_mode&07777)!=0700))r=-VFS_EACCES;
    spin_unlock_irqrestore(&e4_lock,flags);if(r)goto done;
    px_copy(next->files[executable],"/var/lib/pkg/upgrade-v3/upgrade-");
    if(!px_decimal(next->files[executable],m->build)){r=-VFS_EFBIG;goto done;}
    for(unsigned i=0;i<m->file_count;i++){r=pu_stage(u,i);if(r)goto done;}
    u->source=u->record.files[executable].inode;
    for(unsigned i=0;i<old->shape.file_count;i++)if(old->shape.files[i].role==1)u->victim=old->state.files[i].inode;
    flags=spin_lock_irqsave(&e4_lock);px=old;px_encode(&u->record,u->encoded);px=NULL;spin_unlock_irqrestore(&e4_lock,flags);
    char command[33];memcpy(command,old->state.command,32);command[32]=0;
    int error=0;creds_t actor;creds_init_root(&actor);
    vfs_node_t *work=vfs_lookup_at_creds(&fs->cached[0]->node,"/var/lib/pkg/upgrade-v3",&actor,&error);
    vfs_node_t *bin=vfs_lookup_at_creds(&fs->cached[0]->node,"/usr/bin",&actor,&error);
    if(!work||!bin)r=error?error:-VFS_ENOENT;
    else {
        u->actor=px_default_actor();flags=spin_lock_irqsave(&e4_lock);pu_activation=u;spin_unlock_irqrestore(&e4_lock,flags);
        r=work->replace?work->replace(work,next->files[executable]+strlen("/var/lib/pkg/upgrade-v3/"),bin,command,&actor):-VFS_EOPNOTSUPP;
        flags=spin_lock_irqsave(&e4_lock);pu_activation=NULL;spin_unlock_irqrestore(&e4_lock,flags);
    }
    vfs_node_put(bin);vfs_node_put(work);
    if(!r)*status=(pkg_transaction_status){.phase=PKG_TX_IDLE,.sequence=u->record.sequence,.account_sequence=1,.credential_generation=1};
done:
    if(u){pu_drop(u->old);pu_drop(u->next);}kfree(u);kfree(st);return r;
}
