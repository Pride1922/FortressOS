#define TEST_PERMISSIONS_ENFORCEMENT
#define main ext4_previous_suite_main
#include "ext4_mount_host.c"
#undef main
static creds_t root,user;
static const char *source="/mnt/a/source",*target="/mnt/b/target";
static void counters(ext4_fault_disk_t *d) {d->events=d->writes=d->flushes=0;}
static void put_file(const char *path,char byte,size_t count) {
    int error;file_t *f=vfs_open_mode_creds(path,VFS_O_CREAT|VFS_O_RDWR,0755,&root,&error);assert(f);
    uint8_t bytes[512];memset(bytes,byte,sizeof(bytes));
    while(count) {size_t n=count>sizeof(bytes) ? sizeof(bytes) : count;assert(vfs_write(f,bytes,n)==(int64_t)n);count-=n;}
    assert(!vfs_close(f));
}
static void check_file(file_t *f,char byte,size_t count) {
    uint8_t bytes[512];f->offset=0;
    while(count) {size_t n=count>sizeof(bytes) ? sizeof(bytes) : count;assert(vfs_read(f,bytes,n)==(int64_t)n);for(size_t i=0;i<n;i++)assert(bytes[i]==byte);count-=n;}
    assert(!vfs_read(f,bytes,1));
}
static bool recovered_state(ext4_mount_t *fs) {
    int error;file_t *f=vfs_open_ext(target,VFS_O_RDONLY,&error);assert(f);
    uint8_t c;assert(vfs_read(f,&c,1)==1);assert(c=='O'||c=='N');
    bool changed=c=='N';check_file(f,changed ? 'N' : 'O',changed ? 2051 : 1031);assert(!vfs_close(f));
    f=vfs_open_ext(source,VFS_O_RDONLY,&error);
    if(changed)assert(!f && error==-VFS_ENOENT);
    else {assert(f);check_file(f,'N',2051);assert(!vfs_close(f));}
    assert(!fs->engine->orphan_count);return changed;
}
static void unchanged_denial(ext4_fault_disk_t *d,const char *a,const char *b,const creds_t *who,int expected) {
    uint8_t *copy=malloc(d->bytes);assert(copy);memcpy(copy,d->stable,d->bytes);counters(d);
    assert(vfs_replace_creds(a,b,who)==expected);
    assert(!d->events && !memcmp(copy,d->stable,d->bytes) && !memcmp(copy,d->volatile_bytes,d->bytes));free(copy);
}
int main(int argc,char **argv) {
    assert(argc>=4&&argc<=6);bool wrap=argc>=5&&!strcmp(argv[4],"wrap"),quick=argc==6;
    creds_init_root(&root);user=(creds_t){.uid=1001,.euid=1001,.suid=1001,.gid=1001,.egid=1001,.sgid=1001};
    size_t n;uint8_t *initial=load(argv[1],&n);unsigned ss=(unsigned)strtoul(argv[2],NULL,10);
    ext4_fault_disk_t d={.stable=malloc(n),.volatile_bytes=malloc(n),.bytes=n,.cut=-1};assert(d.stable&&d.volatile_bytes);
    block_dev_t dev={.sector_size=ss,.sector_count=n/ss,.read_sector=ext4_fault_read,.write_sector=ext4_fault_write,.flush=ext4_fault_flush,.priv=&d};
    reset(&d,initial);ext4_mount_t *fs=NULL;assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
    assert(!vfs_mkdir("/mnt/a",0755) && !vfs_mkdir("/mnt/b",0755));
    put_file(source,'N',2051);if(wrap)put_file("/mnt/pad",'P',1);put_file(target,'O',1031);
    uint8_t *prepared=malloc(n);assert(prepared);memcpy(prepared,d.stable,n);
    if(wrap) {
        jbd2_plan_t *io=fs->engine->writer->io;uint8_t *js=prepared+(uint64_t)io->super_block*fs->bs;
        j_put(js+24,UINT32_MAX-1);j_put(js+88,io->length-6);jw_seal(js,1024,252,UINT32_MAX);
    }
    unchanged_denial(&d,source,target,&user,-VFS_EACCES);
    unchanged_denial(&d,"/mnt/missing",target,&root,-VFS_ENOENT);
    unchanged_denial(&d,source,"/mnt/missing",&root,-VFS_ENOENT);
    unchanged_denial(&d,source,"/mnt/a",&root,-VFS_EOPNOTSUPP);
    unchanged_denial(&d,source,"/mnt/b/target/",&root,-VFS_ENOTDIR);
    unchanged_denial(&d,source,"/tmp/target",&root,-VFS_EOPNOTSUPP);
    fs->engine->orphan_count=64;unchanged_denial(&d,source,target,&root,-VFS_EFBIG);fs->engine->orphan_count=0;
    assert(!vfs_chmod_creds("/mnt/a",01777,&root) && !vfs_chmod_creds("/mnt/b",01777,&root));
    unchanged_denial(&d,source,target,&user,-VFS_EPERM);
    assert(!vfs_chmod_creds("/mnt/a",0755,&root) && !vfs_chmod_creds("/mnt/b",0755,&root));
    int error;file_t *f=vfs_open(source,VFS_O_RDONLY);assert(f);
    unchanged_denial(&d,source,target,&root,-VFS_EAGAIN);assert(!vfs_close(f));
    counters(&d);assert(!vfs_replace_creds(source,source,&root) && !d.events);
    file_t *old=vfs_open(target,VFS_O_RDWR),*independent=vfs_open(target,VFS_O_RDONLY);assert(old&&independent);
    assert(vfs_file_get(old));file_t *alias=old;
    uint32_t victim=((e4_inode_t *)old->node->fs_private)->ino;
    f=vfs_open(source,VFS_O_RDONLY);assert(f);uint32_t replacement=((e4_inode_t *)f->node->fs_private)->ino;assert(!vfs_close(f));
    fs->account_inodes[1]=replacement;unchanged_denial(&d,source,target,&root,-VFS_EROFS);fs->account_inodes[1]=0;
    vfs_node_t *delayed=vfs_lookup_creds(source,&root,&error);assert(delayed);
    vfs_node_t *delayed_target=vfs_lookup_creds(target,&root,&error);assert(delayed_target);
    vfs_metadata_t meta;void *snapshot=NULL;
    assert(!e4_exec_snapshot(old->node,&root,&meta,&snapshot) && meta.size==1031);
    assert(!vfs_replace_creds(source,target,&root));
    assert(e4_open_actor(delayed,VFS_MAY_WRITE,false,&root)==-VFS_ENOENT);
    vfs_node_put(delayed);
    assert(e4_open_actor(delayed_target,VFS_MAY_READ,false,&root)==-VFS_EAGAIN);
    vfs_node_put(delayed_target);
    for(unsigned i=0;i<1031;i++)assert(((uint8_t *)snapshot)[i]=='O');
    kfree(snapshot);snapshot=NULL;
    assert(!vfs_lookup(source));f=vfs_open(target,VFS_O_RDONLY);assert(f);
    assert(((e4_inode_t *)f->node->fs_private)->ino==replacement && replacement!=victim);
    assert(!e4_exec_snapshot(f->node,&root,&meta,&snapshot) && meta.size==2051);
    for(unsigned i=0;i<2051;i++)assert(((uint8_t *)snapshot)[i]=='N');
    kfree(snapshot);
    check_file(f,'N',2051);check_file(old,'O',1031);check_file(independent,'O',1031);
    old->offset=0;assert(vfs_write(old,"X",1)==1);f->offset=0;char c;assert(vfs_read(f,&c,1)==1&&c=='N');
    assert(!vfs_close(f) && !vfs_close(alias) && !vfs_close(old));assert(fs->engine->orphan_count==1);
    assert(!vfs_close(independent));assert(!fs->engine->orphan_count);
    assert(!ext4_freeze_and_sync(fs));save(argv[3],"retained",d.stable,n);
    unchanged_denial(&d,source,target,&root,-VFS_EROFS);
    /* Same-directory, same directory-block and often same inode-table block. */
    reset(&d,prepared);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
    put_file("/mnt/a/peer",'O',1031);assert(!vfs_replace_creds(source,"/mnt/a/peer",&root));
    put_file("/mnt/a/empty",'N',0);put_file("/mnt/a/empty-peer",'O',0);
    assert(!vfs_replace_creds("/mnt/a/empty","/mnt/a/empty-peer",&root));
    put_file("/mnt/a/large",'O',65539);put_file("/mnt/a/newlarge",'N',2051);
    old=vfs_open("/mnt/a/large",VFS_O_RDONLY);assert(old);
    assert(!vfs_replace_creds("/mnt/a/newlarge","/mnt/a/large",&root));check_file(old,'O',65539);assert(!vfs_close(old));
    assert(!ext4_sync(fs) && !ext4_freeze_and_sync(fs));save(argv[3],"same-directory",d.stable,n);
    /* Event sequence spans activation and later orphan cleanup. */
    reset(&d,prepared);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));counters(&d);
    old=vfs_open(target,VFS_O_RDONLY);assert(old);
    uint32_t head=fs->engine->writer->head,sequence=fs->engine->writer->sequence;
    assert(!vfs_replace_creds(source,target,&root));size_t activation=d.events;
    if(wrap)assert(fs->engine->writer->head<head && !fs->engine->writer->sequence && sequence==UINT32_MAX);
    assert(!vfs_close(old));
    assert(!ext4_sync(fs));size_t events=d.events;assert(events>activation);
    printf("replace baseline activation=%zu total=%zu\n",activation,events);fflush(stdout);
    if(!quick)for(unsigned persistence=0;persistence<4;persistence++)for(unsigned after=0;after<2;after++)for(size_t cut=0;cut<events;cut++) {
        reset(&d,prepared);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
        old=vfs_open(target,VFS_O_RDONLY);assert(old);counters(&d);d.persistence=persistence;d.after=after;d.cut=(long)cut;
        int r=vfs_replace_creds(source,target,&root);
        if(!r) {assert(!vfs_close(old));old=NULL;r=ext4_sync(fs);}
        assert(r==-VFS_EIO && fs->engine->tainted);
        if(fs->namespace_uncertain) {
            assert(!vfs_lookup(source) && !vfs_lookup(target));
            assert(!vfs_open_ext(target,VFS_O_RDONLY,&error) && error==-VFS_EIO);
            /* A failed device may prevent data reads too. Restore reads only;
             * taint/uncertainty still prohibit all namespace admission. */
            d.offline=false;check_file(old,'O',1031);d.offline=true;
        }
        if(old)assert(!vfs_close(old));
        /* Heap teardown is reboot; each mounted recovery performs the complete
         * checksum/allocation/reference audit before publication. */
        teardown();ext4_fault_restart(&d);d.persistence=0;vfs_init();
        assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));recovered_state(fs);
        assert(!ext4_freeze_and_sync(fs));
        teardown();ext4_fault_restart(&d);vfs_init();
        assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));recovered_state(fs);
        assert(!ext4_freeze_and_sync(fs));
    }
    save(argv[3],"recovered",d.stable,n);
    printf("PASS replace mounted retention/admission/cleanup/recovery cuts=%zu\n",quick ? 0 : events*8);
    teardown();free(initial);free(prepared);free(d.stable);free(d.volatile_bytes);return 0;
}
