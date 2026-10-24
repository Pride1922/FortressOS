/* Private native EXT4 floor store. Caller owns exclusion or boot preview. */
#include "pkg_trust_floor.h"
static uint8_t ft_module[3][1024],ft_record[8192];
static tf_floor ft_min,ft_pre,ft_end,ft_current,ft_target;
static tf_observed ft_observed;
static pp_object ft_object;
static bool ft_active;
static uint64_t ft_u64(const uint8_t *p){return (uint64_t)e4_u32(p)|((uint64_t)e4_u32(p+4)<<32);}
static int ft_load(ext4_mount_t *fs){
    size_t length=0;int r=current_read_file(fs,"/pkg/trust-floor.v1",0600,ft_record,&length);if(r)return r;if(length!=1024)return -VFS_EINVAL;
    if(tf_decode(ft_module[0],&jb_policy,&ft_min)||tf_decode(ft_module[1],&jb_policy,&ft_pre)||tf_decode(ft_module[2],&jb_policy,&ft_end)||tf_decode(ft_record,&jb_policy,&ft_current)||
       tf_boot(&ft_current,&ft_min,&ft_pre,&ft_end,&jb_policy))return -VFS_EACCES;
    e4_inode_t in;r=identity_path(fs,"/pkg/trust-floor.v1",&in);if(r)return r;r=e4_mapping(&in);if(r)return r;
    const uint8_t *loc=ft_current.locator;
    if(in.ino!=e4_u32(loc+4)||in.generation!=e4_u32(loc+8)||in.map->count!=1||in.map->entries[0].logical||in.map->entries[0].unwritten||
       in.map->entries[0].len!=1||in.map->entries[0].physical!=ft_u64(loc+48))return -VFS_EACCES;
    memset(&ft_object,0,sizeof ft_object);ft_object.inode=in.ino;ft_object.count=1;ft_object.extent[0].physical=in.map->entries[0].physical;ft_object.extent[0].length=1;
    const uint8_t *g=pn_workspace.parts[2].p,*state=pn_workspace.parts[6].p;
    ft_observed=(tf_observed){.counter={jb_policy.root_version,jb_policy.catalog_revision_floor,ft_u64(state+112),ft_u64(g+24),ft_u64(state+24)}};
    memcpy(ft_observed.root,jb_policy.root_hash,32);memcpy(ft_observed.catalog,jb_policy.catalog_hash,32);
    if(ft_observed.counter[2])memcpy(ft_observed.artifact,state+120,32);
    return tf_admit(&ft_current,&ft_observed)?-VFS_EACCES:0;
}
static int ft_publish(ext4_engine_t *e,unsigned which){
    if(!ft_active || which<1 || which>2 || e->tainted)return -VFS_EIO;
    int r=ft_load(e->fs);if(r)return r;
    if(tf_decode(ft_module[which],&jb_policy,&ft_target))return -VFS_EACCES;
    if(ft_current.epoch>=ft_target.epoch)return 0;
    if(tf_transition(&ft_current,&ft_target,jb_policy.now)||tf_admit(&ft_target,&ft_observed))return -VFS_EACCES;
    if(!e->dirty){r=e4_journal_mark(e,false);if(r)return r;}
    r=e4_namespace_begin(e);if(r)return r;
    uint8_t *image=e4_stage(e,(uint32_t)ft_object.extent[0].physical,E4_NEW);
    if(!image)r=e->error;else memcpy(image,ft_module[which],1024);
    r=e4_namespace_end(e,r);if(r)return r;
    r=ft_load(e->fs);if(!r&&memcmp(ft_record,ft_module[which],1024))r=-VFS_EIO;
    return r;
}
