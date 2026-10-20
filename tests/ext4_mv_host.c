#define TEST_PERMISSIONS_ENFORCEMENT
#define TEST_EXT4_ATOMIC_COUNTERS
#define main previous_mount_main
#include "ext4_mount_host.c"
#undef main
static creds_t root;
static void append(char *p,const char *s){size_t a=strlen(p),b=strlen(s);assert(a+b<256);memcpy(p+a,s,b+1);}
static file_t *create_file(const char *path){file_t *f=vfs_open(path,VFS_O_CREAT|VFS_O_RDWR);assert(f);return f;}
static void seed(void){
    assert(!vfs_mkdir("/mnt/opt",0755)&&!vfs_mkdir("/mnt/var",0755));
    assert(!vfs_mkdir("/mnt/opt/tree",0755)&&!vfs_mkdir("/mnt/opt/tree/sub",0755));
    file_t *f=create_file("/mnt/opt/tree/sub/data");assert(vfs_write(f,"PAYLOAD",7)==7&&!vfs_close(f));
}
static unsigned links(vfs_node_t *node){e4_inode_t *in=node->fs_private;uint8_t raw[256];ext4_mount_t *fs=in->fs;
    uint32_t g=(in->ino-1)/fs->ipg,index=(in->ino-1)%fs->ipg;
    assert(e4_bytes(fs,(uint64_t)e4_u32(fs->gd[g].raw+8)*fs->bs+index*256,raw,256));return e4_u16(raw+26);
}
static void verify(const char *path,const char *parent,unsigned old_links,unsigned new_links){
    int error;vfs_node_t *n=vfs_lookup_ref(path,&error),*p=vfs_lookup_ref(parent,&error);assert(n&&p&&n->parent==p);
    uint64_t flags=spin_lock_irqsave(&e4_lock);uint32_t up=0;
    assert(e4_scan(n->fs_private,"..",0,NULL,&up)==1&&up==((e4_inode_t *)p->fs_private)->ino);
    spin_unlock_irqrestore(&e4_lock,flags);vfs_node_put(n);vfs_node_put(p);
    n=vfs_lookup_ref("/mnt/opt",&error);p=vfs_lookup_ref("/mnt/var",&error);assert(n&&p);
    assert(links(n)==old_links&&links(p)==new_links);vfs_node_put(n);vfs_node_put(p);
}
static void reboot(block_dev_t *dev,ext4_mount_t **fs){teardown();ext4_fault_restart(dev->priv);vfs_init();assert(!ext4_mount_journal_fixture(dev,"/mnt",admitted,fs));}
int main(int argc,char **argv){
    assert(argc==4||argc==5);creds_init_root(&root);size_t size;uint8_t *initial=load(argv[1],&size);unsigned ss=(unsigned)strtoul(argv[2],NULL,10);
    ext4_fault_disk_t d={.stable=malloc(size),.volatile_bytes=malloc(size),.bytes=size,.cut=-1};assert(d.stable&&d.volatile_bytes);
    block_dev_t dev={.sector_size=ss,.sector_count=size/ss,.read_sector=ext4_fault_read,.write_sector=ext4_fault_write,.flush=ext4_fault_flush,.priv=&d};
    ext4_mount_t *fs;reset(&d,initial);
    if(argc==5) {
        assert(!ext4_mount_rw(&dev,"/mnt",&fs));seed();file_t *f=vfs_open("/mnt/opt/tree/sub/data",VFS_O_RDONLY);assert(f);
        assert(!vfs_rename("/mnt/opt/tree","/mnt/opt/renamed"));
        assert(!vfs_rename("/mnt/opt/renamed","/mnt/var/moved"));
        assert(!strcmp(f->node->path,"/mnt/var/moved/sub/data"));verify("/mnt/var/moved","/mnt/var",2,3);
        assert(vfs_rename("/mnt/var/moved","/mnt/var/moved/sub/loop")==-VFS_EINVAL);
        assert(!vfs_close(f)&&!ext4_freeze_and_sync(fs));save(argv[3],"legacy",d.stable,size);
        teardown();free(initial);free(d.stable);free(d.volatile_bytes);puts("PASS nonjournaled trusted fallback");return 0;
    }
    assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));seed();
    file_t *held=vfs_open("/mnt/opt/tree/sub/data",VFS_O_RDWR),*directory=vfs_open("/mnt/opt/tree",VFS_O_RDONLY);assert(held&&directory);
    assert(!vfs_rename_creds("/mnt/opt/tree","/mnt/opt/renamed",&root));
    assert(!strcmp(held->node->path,"/mnt/opt/renamed/sub/data"));verify("/mnt/opt/renamed","/mnt/opt",3,2);
    assert(!vfs_rename_creds("/mnt/opt/renamed","/mnt/var/moved",&root));
    assert(!strcmp(held->node->path,"/mnt/var/moved/sub/data")&&!strcmp(directory->node->path,"/mnt/var/moved"));
    verify("/mnt/var/moved","/mnt/var",2,3);char bytes[8]={0};assert(vfs_read(held,bytes,7)==7&&!memcmp(bytes,"PAYLOAD",7));
    size_t events=d.events;assert(vfs_rename_creds("/mnt/var/moved","/mnt/var/moved/sub/loop",&root)==-VFS_EINVAL&&d.events==events);
    assert(vfs_rename_creds("/mnt/var/moved","/mnt/var/moved/sub",&root)==-VFS_EINVAL&&d.events==events);
    assert(!vfs_mkdir("/mnt/opt/existing",0755));events=d.events;
    assert(vfs_rename_creds("/mnt/var/moved","/mnt/opt/existing",&root)==-VFS_EEXIST&&d.events==events);
    assert(!vfs_rename_creds("/mnt/var/moved/","/mnt/var/moved/",&root)&&d.events==events);
    assert(!vfs_close(held)&&!vfs_close(directory));assert(!ext4_freeze_and_sync(fs));save(argv[3],"moved",d.stable,size);
    /* Cold subtree after mount: no path-based descendants are loaded. */
    reboot(&dev,&fs);assert(!vfs_rename_creds("/mnt/var/moved","/mnt/opt/back",&root));verify("/mnt/opt/back","/mnt/opt",4,2);
    assert(!ext4_freeze_and_sync(fs));save(argv[3],"back",d.stable,size);
    /* Rejection when a cold descendant would exceed the destination path cap. */
    reset(&d,initial);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));seed();
    char path[256]="/mnt/var";for(unsigned i=0;i<3;i++){append(path,"/abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefgh");assert(!vfs_mkdir(path,0755));}
    append(path,"/abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwx");assert(!vfs_mkdir(path,0755));append(path,"/move");
    events=d.events;assert(vfs_rename_creds("/mnt/opt/tree",path,&root)==-VFS_EFBIG&&d.events==events);
    /* One prepared cross-parent transaction, all callback cuts and persistence models. */
    reset(&d,initial);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));seed();assert(!ext4_sync(fs));
    uint8_t *prepared=malloc(size);assert(prepared);memcpy(prepared,d.stable,size);
    d.events=d.writes=d.flushes=0;allocations=0;
    assert(!vfs_rename_creds("/mnt/opt/tree","/mnt/var/moved",&root));size_t total=d.events,allocs=allocations;
    unsigned cuts=0;
    for(unsigned persistence=0;persistence<4;persistence++)for(unsigned after=0;after<2;after++)for(size_t cut=0;cut<total;cut++){
        reset(&d,prepared);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));d.events=d.writes=d.flushes=0;d.persistence=persistence;d.cut=(long)cut;d.after=after!=0;
        assert(vfs_rename_creds("/mnt/opt/tree","/mnt/var/moved",&root)==-VFS_EIO);
        assert(fs->namespace_uncertain&&fs->engine->tainted);
        int error;assert(!vfs_lookup_ref("/mnt/opt/tree",&error)&&error==-VFS_EIO);
        reboot(&dev,&fs);
        for(unsigned pass=0;pass<2;pass++){
            bool moved=vfs_lookup("/mnt/var/moved")!=NULL;
            assert((vfs_lookup("/mnt/opt/tree")!=NULL)!=moved);
            verify(moved?"/mnt/var/moved":"/mnt/opt/tree",moved?"/mnt/var":"/mnt/opt",moved?2:3,moved?3:2);
            file_t *f=vfs_open(moved?"/mnt/var/moved/sub/data":"/mnt/opt/tree/sub/data",VFS_O_RDONLY);assert(f);
            memset(bytes,0,sizeof(bytes));assert(vfs_read(f,bytes,7)==7&&!memcmp(bytes,"PAYLOAD",7)&&!vfs_close(f));
            assert(!ext4_freeze_and_sync(fs));if(!pass)reboot(&dev,&fs);
        }cuts++;
    }
    save(argv[3],"recovered",d.stable,size);
    for(size_t at=0;at<allocs;at++){
        reset(&d,prepared);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));int error;
        vfs_node_t *a=vfs_lookup_ref("/mnt/opt",&error),*b=vfs_lookup_ref("/mnt/var",&error);assert(a&&b);
        d.events=0;fail_alloc=(long)at;int r=a->rename_actor(a,"tree",b,"moved",false,&root);fail_alloc=-1;
        /* allocs also includes pathname-cache allocations; untriggered indices
         * may succeed. A triggered pre-I/O failure must be ENOMEM, never partial. */
        assert(!r||(r==-VFS_ENOMEM&&!d.events));vfs_node_put(a);vfs_node_put(b);
    }
    reset(&d,prepared);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
    assert(!vfs_chmod_creds("/mnt/opt",0777,&root)&&!vfs_chmod_creds("/mnt/var",0777,&root));
    creds_t user;creds_init_root(&user);user.uid=user.euid=user.suid=1000;user.cap_effective=0;
    events=d.events;assert(vfs_rename_creds("/mnt/opt/tree","/mnt/var/moved",&user)==-VFS_EACCES&&d.events==events);
    assert(!vfs_chmod_creds("/mnt/opt",01777,&root));events=d.events;
    assert(vfs_rename_creds("/mnt/opt/tree","/mnt/opt/other",&user)==-VFS_EPERM&&d.events==events);
    assert(!vfs_mkdir("/mnt/etc",0755)&&!vfs_mkdir("/mnt/var/account",0755));fs->trusted_root=true;account_io_t account;assert(!ext4_root_account_io(fs,&account));
    fs->account_inodes[0]=((e4_inode_t *)vfs_lookup("/mnt/etc")->fs_private)->ino;
    events=d.events;assert(vfs_rename_creds("/mnt/etc","/mnt/var/etc",&root)==-VFS_EROFS&&d.events==events);
    printf("PASS %u rename callback cuts, %zu allocation positions; retained subtree/links/dotdot/cold/path bounds/DAC/sticky/account\n",cuts,allocs);
    teardown();free(initial);free(prepared);free(d.stable);free(d.volatile_bytes);return 0;
}
