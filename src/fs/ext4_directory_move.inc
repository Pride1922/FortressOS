/* Cache publication is reserved/preflighted before I/O, never recursive. */
typedef struct {
    vfs_node_t *root;
    size_t old_len,new_len;
    char old_path[VFS_MAX_PATH],new_path[VFS_MAX_PATH];
} e4_move_cache_t;
typedef struct { uint32_t ino,parent;unsigned length; } e4_move_item_t;
typedef struct {
    e4_move_item_t items[E4_MAP_MAX];
    unsigned count,entries,blocks;
    uint8_t block[4096];
} e4_move_tree_t;
static int e4_move_path(char *out,const vfs_node_t *parent,const char *name) {
    size_t a=!strcmp(parent->path,"/")?0:strlen(parent->path),b=strlen(name);
    if(a+1+b>=VFS_MAX_PATH)return -VFS_EFBIG;
    memcpy(out,parent->path,a);out[a++]='/';memcpy(out+a,name,b+1);return 0;
}
static bool e4_move_descendant(vfs_node_t *node,vfs_node_t *root) {
    for(unsigned i=0;node&&i<E4_NODES;i++,node=node->parent)if(node==root)return true;
    return false;
}
static int e4_move_tree_entry(ext4_mount_t *fs,e4_move_tree_t *tree,
                              const e4_move_item_t *parent,const uint8_t *d) {
    uint32_t ino=e4_u32(d);unsigned nl=d[6];
    if(!ino || (nl==1&&d[8]=='.') || (nl==2&&d[8]=='.'&&d[9]=='.'))return 0;
    if(++tree->entries>E4_MAP_MAX || parent->length+1+nl>=VFS_MAX_PATH)return -VFS_EFBIG;
    e4_inode_t in;int r=e4_inode(fs,ino,&in);if(r)return r;
    if(in.mode!=(d[7]==2?VFS_S_IFDIR:VFS_S_IFREG))return -VFS_EIO;
    if(in.mode==VFS_S_IFDIR) {
        for(unsigned i=0;i<tree->count;i++)if(tree->items[i].ino==ino)return -VFS_EIO;
        if(tree->count==E4_MAP_MAX)return -VFS_EFBIG;
        tree->items[tree->count++]=(e4_move_item_t){ino,parent->ino,parent->length+1+nl};
    }
    return 0;
}
static int e4_move_tree_check(ext4_mount_t *fs,uint32_t ino,uint32_t parent,unsigned length) {
    e4_move_tree_t *tree=kcalloc(1,sizeof(*tree));if(!tree)return -VFS_ENOMEM;
    tree->items[tree->count++]=(e4_move_item_t){ino,parent,length};int r=0;
    for(unsigned item=0;!r&&item<tree->count;item++) {
        e4_move_item_t current=tree->items[item];e4_inode_t in;uint32_t up=0,self=0;vfs_dirent_t dot;
        r=e4_inode(fs,current.ino,&in);if(r)break;
        if(in.mode!=VFS_S_IFDIR){r=-VFS_EIO;break;}
        if(in.size/fs->bs>E4_MAP_MAX-tree->blocks){r=-VFS_EFBIG;break;}
        tree->blocks+=(unsigned)(in.size/fs->bs);
        r=e4_scan(&in,".",0,&dot,&self);if(r<0)break;
        if(dot.type!=VFS_DIRECTORY){r=-VFS_EIO;break;}
        r=e4_scan(&in,"..",0,&dot,&up);
        if(r<0)break;
        if(self!=current.ino||up!=current.parent||dot.type!=VFS_DIRECTORY){r=-VFS_EIO;break;}
        r=0;
        for(uint32_t l=0;!r&&l<in.size/fs->bs;l++) {
            uint32_t physical;r=e4_dir_block(&in,l,&physical);if(r)break;
            if(!e4_bytes_kind(fs,(uint64_t)physical*fs->bs,tree->block,fs->bs,E4_PR_DIRECTORY)){r=-VFS_EIO;break;}
            uint8_t *tail=tree->block+fs->bs-12;
            if(e4_u32(tail)||e4_u16(tail+4)!=12||tail[6]||tail[7]!=0xde||
                e4_crc(e4_inode_seed(fs,in.ino,in.generation),tree->block,fs->bs-12)!=e4_u32(tail+8)){r=-VFS_EIO;break;}
            for(unsigned at=0;!r&&at<fs->bs-12;) {
                const uint8_t *d=tree->block+at;unsigned rec=e4_u16(d+4),nl=d[6];
                if(rec<8||rec%4||rec>fs->bs-12-at||nl>rec-8){r=-VFS_EIO;break;}
                if(e4_u32(d)&&(!nl||nl>=VFS_MAX_NAME||(d[7]!=1&&d[7]!=2))){r=-VFS_EOPNOTSUPP;break;}
                r=e4_move_tree_entry(fs,tree,&current,d);at+=rec;
            }
        }
    }
    kfree(tree);return r;
}
static int e4_move_cache_prepare(ext4_mount_t *fs,vfs_node_t *old,const char *name,
                                 vfs_node_t *dest,const char *next,uint32_t ino,
                                 bool directory,e4_move_cache_t **out) {
    *out=NULL;e4_move_cache_t *p=kcalloc(1,sizeof(*p));if(!p)return -VFS_ENOMEM;
    int r=e4_move_path(p->old_path,old,name);if(!r)r=e4_move_path(p->new_path,dest,next);
    if(r)goto fail;
    for(vfs_node_t *n=old->children;n;n=n->next)if(!strcmp(n->name,name)) {
        if(((e4_node_t *)n)->removed||((e4_node_t *)n)->inode.ino!=ino){r=-VFS_EIO;goto fail;}
        p->root=n;memcpy(p->old_path,n->path,strlen(n->path)+1);break;
    }
    p->old_len=strlen(p->old_path);p->new_len=strlen(p->new_path);
    if(directory) {
        if(p->root&&e4_move_descendant(dest,p->root)){r=-VFS_EINVAL;goto fail;}
        r=e4_move_tree_check(fs,ino,((e4_node_t *)old)->inode.ino,(unsigned)p->new_len);if(r)goto fail;
    }
    if(p->root)for(unsigned i=0;i<fs->nodes;i++) {
        vfs_node_t *n=&fs->cached[i]->node;if(!e4_move_descendant(n,p->root))continue;
        size_t length=strlen(n->path);
        if(length<p->old_len||memcmp(n->path,p->old_path,p->old_len)||
            (length>p->old_len&&n->path[p->old_len]!='/')){r=-VFS_EIO;goto fail;}
        if(p->new_len+length-p->old_len>=VFS_MAX_PATH){r=-VFS_EFBIG;goto fail;}
    }
    *out=p;return 0;
fail:
    kfree(p);return r;
}
static void e4_move_cache_publish(ext4_mount_t *fs,vfs_node_t *old,vfs_node_t *dest,
                                 const char *name,e4_move_cache_t *p) {
    if(!p->root)return;
    /* Rebase before changing ancestry; the saved old prefix is independent. */
    for(unsigned i=0;i<fs->nodes;i++) {
        vfs_node_t *n=&fs->cached[i]->node;if(!e4_move_descendant(n,p->root))continue;
        size_t suffix=strlen(n->path)-p->old_len;
        memmove(n->path+p->new_len,n->path+p->old_len,suffix+1);memcpy(n->path,p->new_path,p->new_len);
    }
    vfs_node_t **link=&old->children;while(*link&&*link!=p->root)link=&(*link)->next;
    if(*link)*link=p->root->next;
    p->root->parent=dest;p->root->next=dest->children;dest->children=p->root;
    memcpy(p->root->name,name,strlen(name)+1);
}
