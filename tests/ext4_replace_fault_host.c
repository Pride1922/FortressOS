#define TEST_PERMISSIONS_ENFORCEMENT
#define TEST_EXT4_ATOMIC_COUNTERS
#define main previous_mount_main
#include "ext4_mount_host.c"
#undef main
static creds_t root;
extern uint64_t ext4_time_test_seconds;
static void raw_inode(file_t *f,uint8_t raw[256]) {
    e4_inode_t *in=f->node->fs_private;ext4_mount_t *fs=in->fs;
    uint32_t g=(in->ino-1)/fs->ipg,index=(in->ino-1)%fs->ipg;
    uint64_t flags=spin_lock_irqsave(&e4_lock);
    assert(e4_bytes(fs,(uint64_t)e4_u32(fs->gd[g].raw+8)*fs->bs+(uint64_t)index*256,raw,256));
    spin_unlock_irqrestore(&e4_lock,flags);
}
static void seed(const char *name,char c) {
    file_t *f=vfs_open(name,VFS_O_CREAT|VFS_O_RDWR);assert(f);
    char b[16];memset(b,c,sizeof(b));assert(vfs_write(f,b,sizeof(b))==sizeof(b));assert(!vfs_close(f));
}
static void zero_events(ext4_fault_disk_t *d) {d->events=d->writes=d->flushes=0;reads=allocations=0;}
typedef struct {pthread_barrier_t *barrier;const char *source;int result;_Atomic unsigned *done;} worker_t;
static void *writer(void *p) {
    worker_t *w=p;pthread_barrier_wait(w->barrier);w->result=vfs_replace_creds(w->source,"/mnt/target",&root);
    atomic_fetch_add(w->done,1);return NULL;
}
static void *reader(void *p) {
    worker_t *w=p;pthread_barrier_wait(w->barrier);unsigned checks=0;
    do {
        int error;file_t *f=vfs_open_creds("/mnt/target",VFS_O_RDONLY,&root,&error);
        if(!f) {assert(error==-VFS_EAGAIN);continue;}
        char bytes[16];assert(vfs_read(f,bytes,sizeof(bytes))==sizeof(bytes));
        assert(bytes[0]=='O'||bytes[0]=='A'||bytes[0]=='B');
        for(unsigned i=1;i<sizeof(bytes);i++)assert(bytes[i]==bytes[0]);
        assert(!vfs_close(f));checks++;
    }while(atomic_load(w->done)<2 || !checks);
    return NULL;
}
int main(int argc,char **argv) {
    assert(argc==4);creds_init_root(&root);ext4_time_test_seconds=1792368000;
    size_t n;uint8_t *initial=load(argv[1],&n);
    unsigned ss=(unsigned)strtoul(argv[2],NULL,10);
    ext4_fault_disk_t d={.stable=malloc(n),.volatile_bytes=malloc(n),.bytes=n,.cut=-1};assert(d.stable&&d.volatile_bytes);
    block_dev_t dev={.sector_size=ss,.sector_count=n/ss,.read_sector=ext4_fault_read,.write_sector=ext4_fault_write,.flush=ext4_fault_flush,.priv=&d};
    ext4_mount_t *fs=NULL;reset(&d,initial);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
    seed("/mnt/source",'N');seed("/mnt/target",'O');
    uint8_t *prepared=malloc(n),*before=malloc(n);assert(prepared&&before);memcpy(prepared,d.stable,n);
    zero_events(&d);assert(!vfs_replace_creds("/mnt/source","/mnt/target",&root));size_t count=allocations,read_count=reads;
    assert(count);assert(!ext4_sync(fs));
    assert(!ext4_freeze_and_sync(fs));save(argv[3],"activated-new",d.stable,n);
    reset(&d,prepared);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
    assert(!ext4_freeze_and_sync(fs));save(argv[3],"original-old",d.stable,n);
    for(size_t i=0;i<count;i++) {
        reset(&d,prepared);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));memcpy(before,d.stable,n);zero_events(&d);fail_alloc=(long)i;
        int r=vfs_replace_creds("/mnt/source","/mnt/target",&root);fail_alloc=-1;
        assert(r==-VFS_ENOMEM && !fs->engine->tainted && !fs->namespace_uncertain && !d.events);
        assert(!memcmp(before,d.stable,n) && !memcmp(before,d.volatile_bytes,n));
        assert(vfs_lookup("/mnt/source") && vfs_lookup("/mnt/target"));
    }
    unsigned denied_reads=0;
    for(unsigned i=0;i<read_count;i++) {
        reset(&d,prepared);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));memcpy(before,d.stable,n);zero_events(&d);fail_read=i;
        int r=vfs_replace_creds("/mnt/source","/mnt/target",&root);fail_read=-1;
        if(r!=-VFS_EIO || fs->engine->tainted || d.events)
            fprintf(stderr,"read fault %u: result=%d taint=%u events=%zu reads=%zu\n",i,r,fs->engine->tainted,d.events,(size_t)reads);
        assert(r==-VFS_EIO && !fs->engine->tainted && !d.events);
        assert(!memcmp(before,d.stable,n)&&!memcmp(before,d.volatile_bytes,n));denied_reads++;
    }
    reset(&d,prepared);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));zero_events(&d);
    memcpy(before,d.stable,n);
    uint32_t length=fs->engine->writer->io->length;fs->engine->writer->io->length=fs->engine->writer->io->first+1;
    assert(vfs_replace_creds("/mnt/source","/mnt/target",&root)==-VFS_ENOSPC && !d.events);
    assert(!memcmp(before,d.stable,n) && !memcmp(before,d.volatile_bytes,n));
    fs->engine->writer->io->length=length;
    assert(!vfs_replace_creds("/mnt/source","/mnt/target",&root));assert(!ext4_sync(fs));
    printf("PASS allocation failures=%zu, early read failures=%u and exhausted journal credits preserve disk/state\n",count,denied_reads);fflush(stdout);
    reset(&d,prepared);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));assert(!vfs_chmod_creds("/mnt",0777,&root));
    creds_t user={.uid=1001,.euid=1001,.suid=1001,.gid=1001,.egid=1001,.sgid=1001};int error;
    file_t *owned=vfs_open_mode_creds("/mnt/user-source",VFS_O_CREAT|VFS_O_RDWR,0600,&user,&error);assert(owned);
    assert(vfs_write(owned,"private",7)==7 && !vfs_close(owned));
    assert(!vfs_replace_creds("/mnt/user-source","/mnt/target",&user));
    owned=vfs_open_creds("/mnt/target",VFS_O_RDONLY,&user,&error);assert(owned);vfs_metadata_t value;
    assert(!vfs_metadata(owned->node,&value) && value.uid==1001 && value.gid==1001 && value.mode==(VFS_S_IFREG|0600));
    assert(!vfs_close(owned) && !ext4_sync(fs) && !ext4_freeze_and_sync(fs));
    save(argv[3],"credentials",d.stable,n);puts("PASS non-root replacement in writable directory preserves source UID/GID/mode");
    reset(&d,prepared);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));ext4_time_test_seconds=2147483648ULL;
    assert(!vfs_replace_creds("/mnt/source","/mnt/target",&root));
    owned=vfs_open("/mnt/target",VFS_O_RDONLY);assert(owned);uint8_t raw[256];raw_inode(owned,raw);
    assert(e4_u32(raw+12)==0x80000000u && e4_u32(raw+132)==1);
    assert(e4_u32(raw+16)==1792368000 && !e4_u32(raw+136));
    assert(e4_u32(raw+144)==1792368000 && !e4_u32(raw+148));
    assert(!vfs_close(owned)&&!ext4_sync(fs)&&!ext4_freeze_and_sync(fs));save(argv[3],"timestamps",d.stable,n);
    ext4_time_test_seconds=1792368000;reset(&d,prepared);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
    owned=vfs_open("/mnt/source",VFS_O_RDONLY);assert(owned);
    uint64_t flags=spin_lock_irqsave(&e4_lock);assert(!e4_namespace_begin(fs->engine));
    e4_inode_t *in=owned->node->fs_private;uint8_t *staged=e4_inode_image(fs->engine,in->ino);assert(staged);
    e4_p16(staged+128,4);memset(staged+132,0,124);e4_inode_sum(fs,in->ino,staged);
    assert(!e4_namespace_end(fs->engine,0));spin_unlock_irqrestore(&e4_lock,flags);assert(!vfs_close(owned));
    memcpy(before,d.stable,n);zero_events(&d);ext4_time_test_seconds=2147483648ULL;
    assert(vfs_replace_creds("/mnt/source","/mnt/target",&root)==-VFS_EOPNOTSUPP && !d.events);
    assert(!memcmp(before,d.stable,n)&&!memcmp(before,d.volatile_bytes,n));
    ext4_time_test_seconds=1792368000;assert(!ext4_freeze_and_sync(fs));save(argv[3],"short-extra",d.stable,n);
    puts("PASS replacement ctime epoch encoding, preserved mtime/crtime and short-extra rejection before I/O");
    for(unsigned round=0;round<32;round++) {
        reset(&d,prepared);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
        seed("/mnt/one",'A');seed("/mnt/two",'B');
        pthread_barrier_t barrier;assert(!pthread_barrier_init(&barrier,NULL,3));_Atomic unsigned done=0;
        worker_t a={&barrier,"/mnt/one",-1,&done},b={&barrier,"/mnt/two",-1,&done};pthread_t x,y,z;
        assert(!pthread_create(&x,NULL,writer,&a) && !pthread_create(&y,NULL,writer,&b) && !pthread_create(&z,NULL,reader,&a));
        assert(!pthread_join(x,NULL)&&!pthread_join(y,NULL)&&!pthread_join(z,NULL));assert(!pthread_barrier_destroy(&barrier));
        assert(!a.result&&!b.result);assert(!ext4_sync(fs) && !fs->engine->orphan_count);
        assert(!ext4_freeze_and_sync(fs));
    }
    save(argv[3],"race",d.stable,n);puts("PASS 32 concurrent two-replacer/open/read races; no false ENOENT or mixed bytes");
    /* A separate cache-capacity fixture has enough inodes for 1024 retained
     * legacy nodes. All references must remain valid through failed admission. */
    reset(&d,prepared);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
    assert(vfs_lookup("/mnt/source")&&vfs_lookup("/mnt/target"));
    for(unsigned i=0;fs->nodes<E4_NODES;i++) {
        char path[64];snprintf(path,sizeof(path),"/mnt/pinned-%u",i);assert(vfs_create(path,VFS_FILE));
    }
    /* Pin the two replacement entries too; cache eviction cannot make room. */
    assert(vfs_lookup("/mnt/source")&&vfs_lookup("/mnt/target"));zero_events(&d);
    uint8_t *full=malloc(n);assert(full);memcpy(full,d.stable,n);
    assert(vfs_replace_creds("/mnt/source","/mnt/target",&root)==-VFS_EFBIG && !d.events);
    assert(!memcmp(full,d.stable,n)&&!memcmp(full,d.volatile_bytes,n));free(full);
    assert(!ext4_freeze_and_sync(fs));
    teardown();ext4_fault_restart(&d);vfs_init();assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
    assert(!vfs_replace_creds("/mnt/source","/mnt/target",&root));assert(!ext4_sync(fs));
    assert(!ext4_freeze_and_sync(fs));save(argv[3],"cache",d.stable,n);puts("PASS 1024 pinned cache entries; failure preservation and reboot retry");
    reset(&d,prepared);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
    file_t *bulk=vfs_open("/mnt/fill",VFS_O_CREAT|VFS_O_RDWR),*tail=vfs_open("/mnt/fill-tail",VFS_O_CREAT|VFS_O_RDWR);assert(bulk&&tail);
    uint8_t *payload=malloc(32768);assert(payload);memset(payload,'F',32768);
    while(vfs_write(bulk,payload,32768)==32768) {}
    while(vfs_write(tail,payload,fs->bs)==(int64_t)fs->bs) {}
    assert(vfs_write(tail,payload,fs->bs)==-VFS_ENOSPC && !fs->engine->tainted);
    assert(!vfs_close(bulk)&&!vfs_close(tail));free(payload);
    uint64_t free_blocks=0;for(unsigned g=0;g<fs->groups;g++)free_blocks+=e4_u16(fs->gd[g].raw+12);
    assert(!free_blocks);
    assert(!vfs_replace_creds("/mnt/source","/mnt/target",&root));assert(!ext4_sync(fs));
    assert(!ext4_freeze_and_sync(fs));save(argv[3],"disk-full",d.stable,n);puts("PASS zero free data blocks: allocation-free replacement and orphan cleanup");
    teardown();free(initial);free(prepared);free(before);free(d.stable);free(d.volatile_bytes);return 0;
}
