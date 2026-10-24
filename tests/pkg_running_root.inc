/* Private fixture boot handoff, before any Ring 3 publication. */
static bool jh_running;
static bool jh_lose_account_flush;
static pp_object jh_base[5];
static const char *jh_paths[]={"/bin/login","/bin/shell","/bin/sudo","/bin/pkg-boot","/bin"};
static uint8_t jh_buffer[4096];
static int jh_image(ext4_mount_t *fs,unsigned which){
    creds_t actor;creds_init_root(&actor);int r=0,error=0;
    vfs_node_t *disk=vfs_lookup_at_creds(ext4_prepared_root(fs),jh_paths[which],&actor,&error);
    vfs_node_t *source=which<4?vfs_bootstrap_lookup_creds(jh_paths[which],&actor,&error):NULL;
    vfs_metadata_t dm,sm;
    if(!disk || vfs_metadata(disk,&dm) || dm.uid || dm.gid || (dm.mode&07777)!=(which==2?04755:0755) ||
       dm.type!=(which==4?VFS_DIRECTORY:VFS_FILE))r=-VFS_EACCES;
    if(!r && which<4 && (!source || vfs_metadata(source,&sm) || dm.size!=sm.size || !dm.size || dm.size>VFS_MAX_EXEC_SIZE))r=-VFS_EINVAL;
    uint8_t dh[32],sh[32];digest_t hash;
    if(!r && which<4){
        vfs_node_t *nodes[]={disk,source};uint8_t *sums[]={dh,sh};
        for(unsigned n=0;n<2 && !r;n++){
            digest_init(&hash,DIGEST_SHA256);uint64_t at=0;
            while(at<dm.size){size_t take=dm.size-at;if(take>sizeof jh_buffer)take=sizeof jh_buffer;
                int64_t got=nodes[n]->read?nodes[n]->read(nodes[n],at,jh_buffer,take):0;
                if(!nodes[n]->read && nodes[n]->data){memcpy(jh_buffer,(uint8_t *)nodes[n]->data+at,take);got=take;}
                if(got!=(int64_t)take || !digest_update(&hash,jh_buffer,take)){r=-VFS_EIO;break;}at+=take;
            }
            if(!r)digest_final(&hash,sums[n]);
        }
        if(!r && memcmp(dh,sh,32))r=-VFS_EACCES;
    }
    vfs_node_put(disk);vfs_node_put(source);if(r)return r;
    uint64_t flags=spin_lock_irqsave(&e4_lock);e4_inode_t in;r=identity_path(fs,jh_paths[which],&in);if(!r)r=e4_mapping(&in);
    if(!r && (!in.map->count || in.map->count>4))r=-VFS_EFBIG;
    if(!r){pp_object *o=jh_base+which;memset(o,0,sizeof *o);o->inode=in.ino;o->count=in.map->count;
        for(unsigned i=0;i<o->count;i++){o->extent[i].physical=in.map->entries[i].physical;o->extent[i].length=in.map->entries[i].len;}}
    spin_unlock_irqrestore(&e4_lock,flags);return r;
}
static int jh_fence(ext4_engine_t *e){
    if(!jh_running || e!=jb_engine)return 0;
    for(unsigned i=0;i<5;i++){int r=pn_fence_object(e,jh_base+i);if(r)return r;}
    return 0;
}
int jh_account_gate(ext4_mount_t *fs){return jb_engine && fs==jb_engine->fs?jn_root_ready(fs):0;}
int jh_account_op(void *fs,unsigned op){return jb_engine && fs==jb_engine->fs?jn_account_operation(op):0;}
int jh_account_sync(void *fs){jn_account_barrier=true;int r=ext4_sync(fs);jn_account_barrier=false;return r;}
bool jh_is_running(void){return jh_running;}
void jh_boot_fault(bool lose_flush){jh_lose_account_flush=lose_flush;}
bool jh_finish_boot(void){
    if(!gk_root_admitted())return false;
    const char *stage="reserve namespace";int r=vfs_boot_begin_root_preparation();if(r)goto fail;
    stage="package recovery";
    for(unsigned step=0;step<9;step++){
        uint64_t flags=spin_lock_irqsave(&e4_lock);r=jb_validate(jb_engine);bool done=!r && pn_view.action==PP_DONE;
        if(!r && !done){jb_authorized=true;r=pn_step(jb_engine);jb_authorized=false;}
        spin_unlock_irqrestore(&e4_lock,flags);if(r)goto fail;if(done)break;if(step==8){r=-VFS_EIO;goto fail;}
    }
    stage="measured executables";
    for(unsigned i=0;i<5;i++){r=jh_image(jb_engine->fs,i);if(r)goto fail;}
    stage="journal activation";
    {uint64_t flags=spin_lock_irqsave(&e4_lock);r=jb_validate(jb_engine);
        if(!r && !jb_engine->dirty){jb_authorized=true;r=e4_journal_mark(jb_engine,false);jb_authorized=false;}
        spin_unlock_irqrestore(&e4_lock,flags);if(r)goto fail;}
    stage="real account service";jn_flush_ack_loss=jh_lose_account_flush;r=(int)jn_test(20);if(r)goto fail;
    stage="root publication";r=ext4_publish_root(jb_engine->fs);if(r)goto fail;
    jh_running=true;serial_puts("PKGROOT PUBLISHED real-account-service installed-login\n");return true;
fail:
    jb_authorized=false;accounts_abandon();vfs_boot_cancel_root_preparation();
    serial_puts("PKGROOT REFUSED stage=");serial_puts(stage);serial_puts(" error=");serial_print_dec((uint64_t)-r);serial_puts("\n");return false;
}
static int jh_commit(ext4_engine_t *e){
    if(e!=jb_engine || !jh_running)return pn_core_commit(e);
    int r=jh_fence(e);if(r)return r;
    jb_authorized=true;r=pn_core_commit(e);jb_authorized=false;return r;
}
static int e4_journal_drain(ext4_mount_t *fs,bool finish){
    spin_debug_assert_held(&e4_lock);
    bool own=jh_running && jb_engine && fs==jb_engine->fs;
    if(own)jb_authorized=true;
    int r=jh_core_drain(fs,finish);if(own)jb_authorized=false;return r;
}
