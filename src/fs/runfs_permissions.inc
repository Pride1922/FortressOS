/* Included by runfs.c. Every decision and associated publication is made
 * under run_lock; actor values are captured before entry, without G held. */
#include "permission_values.h"

static vfs_metadata_t run_value_locked(vfs_node_t *n) {
    return (vfs_metadata_t){.size=n->size,.type=n->type,.mode=n->mode,
        .uid=n->uid,.gid=n->gid,.mnt_flags=n->mnt_flags};
}
static int run_access_locked(vfs_node_t *n,unsigned mask,const creds_t *actor) {
    if (!run_entry(n)->used || run_entry(n)->removed) return -VFS_ENOENT;
    vfs_metadata_t value=run_value_locked(n);
    return permission_access_value(&value,mask,actor);
}
static int run_permission_actor(vfs_node_t *n,unsigned mask,const creds_t *actor) {
    uint64_t irq=spin_lock_irqsave(&run_lock);
    int r=run_access_locked(n,mask,actor);
    spin_unlock_irqrestore(&run_lock,irq);return r;
}
static vfs_node_t *run_lookup_actor(vfs_node_t *dir,const char *name,
                                    const creds_t *actor,int *error) {
    uint64_t irq=spin_lock_irqsave(&run_lock);
    int r=run_access_locked(dir,VFS_MAY_EXEC,actor);vfs_node_t *n=NULL;
    if (!r) {
        if (!strcmp(name,".")) n=dir;
        else if (!strcmp(name,"..")) n=dir->parent;
        else n=run_find_locked(dir,name);
        if (!n) r=-VFS_ENOENT;
        else if (n->get==run_get) {
            if (!run_entry(n)->used || run_entry(n)->removed) r=-VFS_ENOENT;
            else if (run_entry(n)->refs==UINT32_MAX) r=-VFS_EIO;
            else run_entry(n)->refs++;
        }
    }
    spin_unlock_irqrestore(&run_lock,irq);
    if (error) *error=r;
    return r ? NULL : n;
}
static int run_open_actor(vfs_node_t *n,unsigned mask,bool truncate,const creds_t *actor) {
    uint64_t irq=spin_lock_irqsave(&run_lock);
    int r=run_access_locked(n,mask,actor);
    if (!r && truncate) {
        if (n->type!=VFS_FILE) r=-VFS_EINVAL;
        else {
            vfs_metadata_t value=run_value_locked(n);
            uint16_t mode=permission_content_mode(&value,actor);
            memset(run_entry(n)->data,0,n->size);n->size=0;n->mode=mode;
        }
    }
    spin_unlock_irqrestore(&run_lock,irq);return r;
}
static int run_setattr_actor(vfs_node_t *n,bool chown,uint32_t mode,
                             creds_id_change_t uid,creds_id_change_t gid,const creds_t *actor) {
    uint64_t irq=spin_lock_irqsave(&run_lock);int r=-VFS_ENOENT;
    if (run_entry(n)->used) {
        vfs_metadata_t old=run_value_locked(n),next;
        r=chown ? permission_chown_value(&old,uid,gid,actor,&next) :
            permission_chmod_value(&old,mode,actor,&next);
        if (!r) { n->mode=next.mode;n->uid=next.uid;n->gid=next.gid; }
    }
    spin_unlock_irqrestore(&run_lock,irq);return r;
}
static int64_t run_write_actor(vfs_node_t *n,uint64_t *offset,bool append,
                               const void *buf,size_t len,const creds_t *actor) {
    uint64_t irq=spin_lock_irqsave(&run_lock);
    /* Existing descriptor rights survive chmod and unlink. */
    if (!run_entry(n)->used) { spin_unlock_irqrestore(&run_lock,irq);return -VFS_ENOENT; }
    if (!len) { spin_unlock_irqrestore(&run_lock,irq);return 0; }
    uint64_t off=append ? n->size : *offset;
    if (off>=RUNFS_BYTES) { spin_unlock_irqrestore(&run_lock,irq);return -VFS_ENOSPC; }
    if (len>RUNFS_BYTES-off) len=RUNFS_BYTES-off;
    vfs_metadata_t value=run_value_locked(n);
    uint16_t mode=permission_content_mode(&value,actor);
    memcpy(run_entry(n)->data+off,buf,len);*offset=off+len;
    if (off+len>n->size) n->size=off+len;
    n->mode=mode;
    spin_unlock_irqrestore(&run_lock,irq);return len;
}
static int run_unlink_actor(vfs_node_t *dir,const char *name,bool require_directory,const creds_t *actor) {
    uint64_t irq=spin_lock_irqsave(&run_lock);
    int r=run_access_locked(dir,VFS_MAY_WRITE|VFS_MAY_EXEC,actor);
    vfs_node_t **link=&dir->children;
    if (!r) {
        while (*link && strcmp((*link)->name,name)) link=&(*link)->next;
        if (!*link) r=-VFS_ENOENT;
        else {
            vfs_node_t *victim=*link;
            if (require_directory && victim->type!=VFS_DIRECTORY) {r=-VFS_ENOTDIR;goto done;}
            vfs_metadata_t d=run_value_locked(dir),v=run_value_locked(victim);
            r=permission_delete_value(&d,&v,actor);
            if (!r && victim->children) r=-VFS_ENOTEMPTY;
            if (!r) {
                *link=victim->next;victim->next=NULL;
                run_entry(victim)->removed=true;run_collect(run_entry(victim));
            }
        }
    }
done:
    spin_unlock_irqrestore(&run_lock,irq);return r;
}
static vfs_node_t *run_create_actor(vfs_node_t *dir,const char *name,vfs_node_type_t type,
                                    uint32_t requested,const creds_t *actor,int *error) {
    int r=-VFS_EINVAL;vfs_node_t *out=NULL;
    if (!name || !*name || strlen(name)>=VFS_MAX_NAME || !strcmp(name,".") ||
        !strcmp(name,"..") || (type!=VFS_FILE && type!=VFS_DIRECTORY)) goto done;
    for (const char *p=name;*p;p++) if (*p=='/') goto done;
    uint64_t irq=spin_lock_irqsave(&run_lock);
    r=run_access_locked(dir,VFS_MAY_WRITE|VFS_MAY_EXEC,actor);
    vfs_create_attrs_t attrs;
    if (!r) {
        vfs_metadata_t value=run_value_locked(dir);
        r=permission_create_value(&value,type,requested,actor,&attrs);
    }
    if (!r && strlen(dir->path)+strlen(name)+2>VFS_MAX_PATH) r=-VFS_EINVAL;
    if (!r && run_find_locked(dir,name)) r=-VFS_EEXIST;
    if (!r) {
        r=-VFS_ENOSPC;
        for (unsigned i=3;i<RUNFS_NODES;i++) if (!run_pool[i].used) {
            run_setup(&run_pool[i],dir,name,type,&attrs);run_pool[i].refs=1;
            out=&run_pool[i].node;out->next=dir->children;dir->children=out;r=0;break;
        }
    }
    spin_unlock_irqrestore(&run_lock,irq);
done:
    if (error) *error=r;
    return out;
}

static int run_rename_actor(vfs_node_t *old,const char *name,vfs_node_t *dest,
                            const char *next,bool require_directory,const creds_t *actor) {
    if (dest->permission_actor!=run_permission_actor) return -VFS_EOPNOTSUPP;
    if (!*name || !*next || !strcmp(name,".") || !strcmp(name,"..") ||
        !strcmp(next,".") || !strcmp(next,"..") || strlen(next)>=VFS_MAX_NAME) return -VFS_EINVAL;
    uint64_t irq=spin_lock_irqsave(&run_lock);
    int r=run_access_locked(old,VFS_MAY_WRITE|VFS_MAY_EXEC,actor);
    if (!r) r=run_access_locked(dest,VFS_MAY_WRITE|VFS_MAY_EXEC,actor);
    vfs_node_t *source=run_find_locked(old,name),*victim=run_find_locked(dest,next);
    if (!r && !source) r=-VFS_ENOENT;
    if (!r && require_directory && source->type!=VFS_DIRECTORY) r=-VFS_ENOTDIR;
    if (!r) {
        vfs_metadata_t d=run_value_locked(old),v=run_value_locked(source);
        r=permission_delete_value(&d,&v,actor);
    }
    if (!r && victim) {
        vfs_metadata_t d=run_value_locked(dest),v=run_value_locked(victim);
        r=permission_delete_value(&d,&v,actor);
    }
    if (!r && source==victim) goto done;
    if (!r && source->type==VFS_DIRECTORY && old!=dest)
        r=run_access_locked(source,VFS_MAY_WRITE,actor);
    if (!r && victim && source->type!=victim->type)
        r=victim->type==VFS_DIRECTORY ? -7 : -VFS_ENOTDIR;
    if (!r && victim && victim->children) r=-VFS_ENOTEMPTY;
    if (!r) for (vfs_node_t *p=dest;p && p->permission_actor==run_permission_actor;p=p->parent)
        if (p==source) { r=-VFS_EINVAL;break; }
    char prior[VFS_MAX_PATH],updated[VFS_MAX_PATH];
    size_t before=0,after=0;
    if (!r) {
        before=strlen(source->path);memcpy(prior,source->path,before+1);
        after=strlen(dest->path);
        if (after+strlen(next)+2>VFS_MAX_PATH) r=-VFS_EINVAL;
        else {
            memcpy(updated,dest->path,after);updated[after++]='/';
            memcpy(updated+after,next,strlen(next)+1);after+=strlen(next);
            for (unsigned i=0;i<RUNFS_NODES;i++) {
                vfs_node_t *n=&run_pool[i].node;
                if (run_pool[i].used && !run_pool[i].removed && !strncmp(n->path,prior,before) &&
                    (n->path[before]==0 || n->path[before]=='/') && after+strlen(n->path+before)>=VFS_MAX_PATH)
                    { r=-VFS_EINVAL;break; }
            }
        }
    }
    if (!r) {
        if (victim) {
            vfs_node_t **link=&dest->children;while (*link!=victim) link=&(*link)->next;
            *link=victim->next;victim->next=NULL;run_entry(victim)->removed=true;run_collect(run_entry(victim));
        }
        vfs_node_t **link=&old->children;while (*link!=source) link=&(*link)->next;
        *link=source->next;source->parent=dest;source->next=dest->children;dest->children=source;
        memcpy(source->name,next,strlen(next)+1);
        for (unsigned i=0;i<RUNFS_NODES;i++) {
            vfs_node_t *n=&run_pool[i].node;
            if (run_pool[i].used && !run_pool[i].removed && !strncmp(n->path,prior,before) &&
                (n->path[before]==0 || n->path[before]=='/')) {
                size_t tail=strlen(n->path+before)+1;
                memmove(n->path+after,n->path+before,tail);memcpy(n->path,updated,after);
            }
        }
    }
done:
    spin_unlock_irqrestore(&run_lock,irq);return r;
}

static int run_exec_snapshot(vfs_node_t *n,const creds_t *actor,vfs_metadata_t *out,void **bytes) {
    uint64_t irq=spin_lock_irqsave(&run_lock);
    int r=run_access_locked(n,VFS_MAY_EXEC,actor);
    void *copy=NULL;
    if (!r && (n->type!=VFS_FILE || !n->size)) r=-VFS_EINVAL;
    if (!r) {
        copy=kmalloc(n->size);
        if (!copy) r=-VFS_ENOMEM;
        else { memcpy(copy,run_entry(n)->data,n->size);*out=run_value_locked(n);*bytes=copy; }
    }
    spin_unlock_irqrestore(&run_lock,irq);return r;
}
