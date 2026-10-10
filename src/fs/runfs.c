#include "runfs.h"
#include "spinlock.h"
#include "string.h"

#define RUNFS_NODES 64u
#define RUNFS_BYTES 4096u
typedef struct { vfs_node_t node; uint32_t refs; bool used,removed,legacy; uint8_t data[RUNFS_BYTES]; } run_node_t;
static run_node_t run_pool[RUNFS_NODES];
static spinlock_t run_lock=SPINLOCK_RANKED(1,"runfs");
static run_node_t *run_entry(vfs_node_t *node) { return (run_node_t *)node; }
static void run_collect(run_node_t *n) {
    if (n->removed && !n->refs && !n->legacy) { memset(n,0,sizeof(*n)); }
}
static vfs_node_t *run_find_locked(vfs_node_t *dir,const char *name) {
    for (vfs_node_t *n=dir->children;n;n=n->next) if (!strcmp(n->name,name)) return n;
    return NULL;
}
static int run_get(vfs_node_t *node) {
    uint64_t irq=spin_lock_irqsave(&run_lock);run_node_t *n=run_entry(node);
    int r=!n->used || n->refs==UINT32_MAX ? -VFS_EIO : 0;
    if (!r) n->refs++;
    spin_unlock_irqrestore(&run_lock,irq);return r;
}
static void run_put(vfs_node_t *node) {
    uint64_t irq=spin_lock_irqsave(&run_lock);run_node_t *n=run_entry(node);
    if (!n->refs) __builtin_trap();
    n->refs--;run_collect(n);spin_unlock_irqrestore(&run_lock,irq);
}
static vfs_node_t *run_lookup_common(vfs_node_t *dir,const char *name,int *error,bool owned) {
    uint64_t irq=spin_lock_irqsave(&run_lock);
    vfs_node_t *n=run_entry(dir)->removed ? NULL : !strcmp(name,".") ? dir :
        !strcmp(name,"..") ? dir->parent : run_find_locked(dir,name);
    int r=-VFS_ENOENT;
    if (n) {
        if (n->get==run_get && owned && run_entry(n)->refs==UINT32_MAX) { n=NULL;r=-VFS_EIO; }
        else {
            if (n->get==run_get) { if (owned) run_entry(n)->refs++;else run_entry(n)->legacy=true; }
            r=0;
        }
    }
    spin_unlock_irqrestore(&run_lock,irq);if (error) *error=r;return n;
}
static vfs_node_t *run_lookup(vfs_node_t *d,const char *n) { return run_lookup_common(d,n,NULL,false); }
static vfs_node_t *run_lookup_ref(vfs_node_t *d,const char *n,int *e) { return run_lookup_common(d,n,e,true); }
static int run_metadata(vfs_node_t *node,vfs_metadata_t *out) {
    uint64_t irq=spin_lock_irqsave(&run_lock);
    *out=(vfs_metadata_t){.size=node->size,.type=node->type,.mode=node->mode,.uid=node->uid,.gid=node->gid};
    spin_unlock_irqrestore(&run_lock,irq);return 0;
}
static int64_t run_read_file(vfs_node_t *node,uint64_t off,void *buf,size_t len) {
    uint64_t irq=spin_lock_irqsave(&run_lock);
    if (off>=node->size) len=0;else if (len>node->size-off) len=node->size-off;
    if (len) memcpy(buf,run_entry(node)->data+off,len);
    spin_unlock_irqrestore(&run_lock,irq);return len;
}
static int64_t run_write_file(vfs_node_t *node,uint64_t *offset,bool append,const void *buf,size_t len) {
    uint64_t irq=spin_lock_irqsave(&run_lock);uint64_t off=append ? node->size : *offset;
    if (off>=RUNFS_BYTES) { spin_unlock_irqrestore(&run_lock,irq);return -VFS_ENOSPC; }
    if (len>RUNFS_BYTES-off) len=RUNFS_BYTES-off;
    memcpy(run_entry(node)->data+off,buf,len);*offset=off+len;
    if (off+len>node->size) node->size=off+len;
    spin_unlock_irqrestore(&run_lock,irq);return len;
}
static int run_truncate_file(vfs_node_t *node,uint64_t size) {
    if (size>RUNFS_BYTES) return -VFS_EFBIG;
    uint64_t irq=spin_lock_irqsave(&run_lock);
    uint64_t start=size<node->size ? size : node->size;
    uint64_t end=size>node->size ? size : node->size;
    memset(run_entry(node)->data+start,0,end-start);node->size=size;
    spin_unlock_irqrestore(&run_lock,irq);return 0;
}
static int run_readdir_node(vfs_node_t *dir,uint64_t index,void *output) {
    uint64_t irq=spin_lock_irqsave(&run_lock);vfs_node_t *n=dir->children;
    while (n && index--) n=n->next;
    if (n) {
        vfs_dirent_t *out=output;memset(out,0,sizeof(*out));
        memcpy(out->name,n->name,strlen(n->name)+1);out->type=n->type;out->size=n->size;
    }
    spin_unlock_irqrestore(&run_lock,irq);return n ? 1 : 0;
}
static int run_unlink_node(vfs_node_t *dir,const char *name) {
    uint64_t irq=spin_lock_irqsave(&run_lock);vfs_node_t **link=&dir->children;
    while (*link && strcmp((*link)->name,name)) link=&(*link)->next;
    int r=-VFS_ENOENT;
    if (*link) {
        vfs_node_t *n=*link;
        if (n->children) r=-VFS_ENOTEMPTY;
        else { *link=n->next;n->next=NULL;run_entry(n)->removed=true;run_collect(run_entry(n));r=0; }
    }
    spin_unlock_irqrestore(&run_lock,irq);return r;
}
static vfs_node_t *run_create_common(vfs_node_t *,const char *,vfs_node_type_t,const vfs_create_attrs_t *,int *,bool);
static vfs_node_t *run_create(vfs_node_t *d,const char *n,vfs_node_type_t t) { return run_create_common(d,n,t,NULL,NULL,false); }
static vfs_node_t *run_create_ref(vfs_node_t *d,const char *n,vfs_node_type_t t,int *e) { return run_create_common(d,n,t,NULL,e,true); }
static vfs_node_t *run_create_attrs(vfs_node_t *d,const char *n,vfs_node_type_t t,const vfs_create_attrs_t *a,int *e) { return run_create_common(d,n,t,a,e,true); }
#if defined(FORTRESS_DAC_ENFORCED) || defined(TEST_PERMISSIONS_ENFORCEMENT)
static int run_permission_actor(vfs_node_t *,unsigned,const creds_t *);
static vfs_node_t *run_lookup_actor(vfs_node_t *,const char *,const creds_t *,int *);
static vfs_node_t *run_create_actor(vfs_node_t *,const char *,vfs_node_type_t,uint32_t,const creds_t *,int *);
static int run_open_actor(vfs_node_t *,unsigned,bool,const creds_t *);
static int run_unlink_actor(vfs_node_t *,const char *,bool,const creds_t *);
static int run_rename_actor(vfs_node_t *,const char *,vfs_node_t *,const char *,bool,const creds_t *);
static int run_setattr_actor(vfs_node_t *,bool,uint32_t,creds_id_change_t,creds_id_change_t,const creds_t *);
static int64_t run_write_actor(vfs_node_t *,uint64_t *,bool,const void *,size_t,const creds_t *);
#endif
static void run_setup(run_node_t *n,vfs_node_t *parent,const char *name,vfs_node_type_t type,const vfs_create_attrs_t *a) {
    memset(n,0,sizeof(*n));n->used=true;n->node.type=type;n->node.parent=parent;
    n->node.mode=(type==VFS_DIRECTORY ? VFS_S_IFDIR : VFS_S_IFREG)|(a ? a->mode : type==VFS_DIRECTORY ? 0755 : 0644);
    n->node.uid=a ? a->uid : 0;n->node.gid=a ? a->gid : 0;
    memcpy(n->node.name,name,strlen(name)+1);
    size_t p=parent ? strlen(parent->path) : 0;
    if (parent) memcpy(n->node.path,parent->path,p);
    if (p!=1) n->node.path[p++]='/';
    memcpy(n->node.path+p,name,strlen(name)+1);
    n->node.get=run_get;n->node.put=run_put;n->node.metadata=run_metadata;
    n->node.owns_nodes=true;n->node.serializes_write_offset=true;
    if (type==VFS_DIRECTORY) {
        n->node.lookup=run_lookup;n->node.lookup_ref=run_lookup_ref;n->node.create=run_create;
        n->node.create_ref=run_create_ref;n->node.create_attrs_ref=run_create_attrs;
        n->node.unlink=run_unlink_node;n->node.readdir=run_readdir_node;
    } else { n->node.read=run_read_file;n->node.write=run_write_file;n->node.truncate=run_truncate_file; }
#if defined(FORTRESS_DAC_ENFORCED) || defined(TEST_PERMISSIONS_ENFORCEMENT)
    n->node.permission_actor=run_permission_actor;n->node.open_actor=run_open_actor;
    n->node.setattr_actor=run_setattr_actor;
    if (type==VFS_DIRECTORY) {
        n->node.lookup_actor=run_lookup_actor;n->node.create_actor=run_create_actor;
        n->node.unlink_actor=run_unlink_actor;
        n->node.rename_actor=run_rename_actor;
    } else n->node.write_actor=run_write_actor;
#endif
}
static vfs_node_t *run_create_common(vfs_node_t *dir,const char *name,vfs_node_type_t type,const vfs_create_attrs_t *a,int *error,bool owned) {
    int r=-VFS_EINVAL;vfs_node_t *out=NULL;
    if (!name || !*name || strlen(name)>=VFS_MAX_NAME || !strcmp(name,".") || !strcmp(name,"..") ||
        (type!=VFS_FILE && type!=VFS_DIRECTORY) || (a && (a->mode & ~07777u))) goto done;
    for (const char *p=name;*p;p++) if (*p=='/') goto done;
    if (strlen(dir->path)+strlen(name)+2>VFS_MAX_PATH) goto done;
    uint64_t irq=spin_lock_irqsave(&run_lock);
    if (run_entry(dir)->removed) r=-VFS_ENOENT;
    else if (run_find_locked(dir,name)) r=-VFS_EEXIST;
    else {
        r=-VFS_ENOSPC;
        for (unsigned i=3;i<RUNFS_NODES;i++) if (!run_pool[i].used) {
            run_setup(&run_pool[i],dir,name,type,a);run_pool[i].refs=owned ? 1 : 0;run_pool[i].legacy=!owned;
            out=&run_pool[i].node;out->next=dir->children;dir->children=out;r=0;break;
        }
    }
    spin_unlock_irqrestore(&run_lock,irq);
done:
    if (error) *error=r;
    if (!out) vfs_set_last_create_error(r);
    return out;
}
#if defined(FORTRESS_DAC_ENFORCED) || defined(TEST_PERMISSIONS_ENFORCEMENT)
#include "runfs_permissions.inc"
#endif
void runfs_init(vfs_node_t *root) {
    memset(run_pool,0,sizeof(run_pool));
    vfs_create_attrs_t tmp={.mode=01777};
    run_setup(&run_pool[0],root,"run",VFS_DIRECTORY,NULL);run_pool[0].legacy=true;
    run_setup(&run_pool[1],root,"tmp",VFS_DIRECTORY,&tmp);run_pool[1].legacy=true;
    run_setup(&run_pool[2],&run_pool[0].node,"user",VFS_DIRECTORY,NULL);run_pool[2].legacy=true;
    run_pool[0].node.children=&run_pool[2].node;
    run_pool[1].node.next=root->children;root->children=&run_pool[1].node;
    run_pool[0].node.next=root->children;root->children=&run_pool[0].node;
}
