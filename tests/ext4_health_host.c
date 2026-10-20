#define TEST_PERMISSIONS_ENFORCEMENT
#define TEST_EXT4_ATOMIC_COUNTERS
#define main previous_mount_main
#include "ext4_mount_host.c"
#undef main
static mount_info_t info(ext4_mount_t *fs) {
    mount_info_t m={0};assert(ext4_get_mount_info(&fs->cached[0]->node,&m));return m;
}
static file_t *create(const char *path) {
    file_t *f=vfs_open(path,VFS_O_CREAT|VFS_O_RDWR);assert(f);return f;
}
typedef struct {file_t *f;unsigned id;} logger_t;
static void *logger(void *arg) {
    logger_t *l=arg;char bytes[32];memset(bytes,'A'+l->id,sizeof(bytes));
    for(unsigned i=0;i<32;i++)assert(vfs_write(l->f,bytes,sizeof(bytes))==sizeof(bytes));
    assert(!vfs_close(l->f));return NULL;
}
int main(int argc,char **argv) {
    assert(argc==4);size_t n;uint8_t *initial=load(argv[1],&n);unsigned ss=(unsigned)strtoul(argv[2],NULL,10);
    ext4_fault_disk_t d={.stable=malloc(n),.volatile_bytes=malloc(n),.bytes=n,.cut=-1};assert(d.stable&&d.volatile_bytes);
    block_dev_t dev={.sector_size=ss,.sector_count=n/ss,.read_sector=ext4_fault_read,.write_sector=ext4_fault_write,.flush=ext4_fault_flush,.priv=&d};
    ext4_mount_t *fs;reset(&d,initial);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
    assert(info(fs).flags==MOUNT_FLAGS_RW);
    file_t *f=create("/mnt/log");assert(!vfs_close(f));pthread_t threads[4];logger_t logs[4];
    for(unsigned i=0;i<4;i++) {
        logs[i]=(logger_t){vfs_open("/mnt/log",VFS_O_WRONLY|VFS_O_APPEND),i};assert(logs[i].f);
        assert(!pthread_create(&threads[i],NULL,logger,&logs[i]));
    }
    for(unsigned i=0;i<4;i++)assert(!pthread_join(threads[i],NULL));
    f=vfs_open("/mnt/log",VFS_O_RDONLY);assert(f);char record[32];unsigned counts[4]={0};
    for(unsigned i=0;i<128;i++) {
        assert(vfs_read(f,record,32)==32 && record[0]>='A' && record[0]<='D');
        for(unsigned j=1;j<32;j++)assert(record[j]==record[0]);
        counts[record[0]-'A']++;
    }
    for(unsigned i=0;i<4;i++)assert(counts[i]==32);
    assert(!vfs_read(f,record,1)&&!vfs_close(f));
    f=create("/mnt/short");uint8_t *payload=malloc(65536);assert(payload);memset(payload,'S',65536);
    assert(vfs_write(f,payload,65536)==32768 && f->offset==32768);
    assert(vfs_write(f,payload+32768,32768)==32768 && f->offset==65536);assert(!vfs_close(f));
    assert(!ext4_sync(fs)&&!ext4_freeze_and_sync(fs));save(argv[3],"logs-short",d.stable,n);
    puts("PASS concurrent append records and positive short-write prefix");fflush(stdout);

    reset(&d,initial);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
    f=vfs_open("/mnt/target.bin",VFS_O_RDONLY);assert(f && !vfs_unlink("/mnt/target.bin"));
    assert(info(fs).flags & MOUNT_FLAGS_CLEANUP_PENDING);
    assert(!(info(fs).flags & MOUNT_FLAGS_RECOVERY_REQUIRED));assert(!ext4_sync(fs));
    assert(ext4_freeze_and_sync(fs)==-VFS_EAGAIN);assert(!vfs_close(f));
    assert(!ext4_freeze_and_sync(fs));assert(!(info(fs).flags & MOUNT_FLAGS_CLEANUP_PENDING));
    save(argv[3],"pinned",d.stable,n);
    reset(&d,initial);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
    mount_info_t baseline=info(fs);
    f=vfs_open("/mnt/target.bin",VFS_O_RDONLY);assert(f && !vfs_unlink("/mnt/target.bin"));
    uint32_t pending_ino=((e4_inode_t *)f->node->fs_private)->ino;
    e4_cache_drop(fs,false);fail_read=0;assert(!vfs_close(f));fail_read=-1;
    assert(!fs->engine->tainted && fs->cleanup_error==-VFS_EIO);
    for(unsigned i=0;i<fs->nodes;i++)assert(fs->cached[i]->inode.ino!=pending_ino);
    assert(e4_orphan_index(fs->engine,pending_ino)>=0);
    assert((info(fs).flags & (MOUNT_FLAGS_CLEANUP_PENDING|MOUNT_FLAGS_CLEANUP_ERROR))==(MOUNT_FLAGS_CLEANUP_PENDING|MOUNT_FLAGS_CLEANUP_ERROR));
    size_t stopped_cleanup=d.events;int error;
    file_t *denied=vfs_open_ext("/mnt/blocked",VFS_O_CREAT|VFS_O_RDWR,&error);
    assert(!denied && error==-VFS_EAGAIN && d.events==stopped_cleanup);
    size_t query_reads=reads;
    assert(info(fs).flags & MOUNT_FLAGS_RECOVERY_REQUIRED);assert(reads==query_reads);
    e4_cache_drop(fs,false);fail_read=0;assert(ext4_sync(fs)==-VFS_EIO);fail_read=-1;
    assert((info(fs).flags & MOUNT_FLAGS_RECOVERY_REQUIRED) && fs->cleanup_error==-VFS_EIO);
    assert(ext4_sync(fs)==-VFS_EIO && !fs->engine->orphan_count && !fs->cleanup_error && !fs->engine->orphan_recovery_required);
    assert(!(info(fs).flags & MOUNT_FLAGS_RECOVERY_REQUIRED));
    file_t *resumed=create("/mnt/blocked");assert(!vfs_close(resumed) && !vfs_unlink("/mnt/blocked"));
    mount_info_t reclaimed=info(fs);assert(reclaimed.free_inodes==baseline.free_inodes+1);
    assert(reclaimed.free_blocks==baseline.free_blocks+3);
    assert(!ext4_sync(fs)&&!ext4_sync(fs));assert(info(fs).free_blocks==reclaimed.free_blocks);
    assert(!ext4_freeze_and_sync(fs));save(argv[3],"close-retry",d.stable,n);
    puts("PASS pinned health, lost close error acknowledgement and exact reclamation");fflush(stdout);

    reset(&d,initial);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
    d.cut=(long)d.events;assert(ext4_sync(fs)==-VFS_EIO);size_t stopped=d.events;
    assert(info(fs).flags & MOUNT_FLAGS_TAINTED);assert(info(fs).flags & MOUNT_FLAGS_RW);
    assert(info(fs).flags & MOUNT_FLAGS_RECOVERY_REQUIRED);
    int taint_error;assert(!vfs_open_ext("/mnt/blocked",VFS_O_CREAT|VFS_O_RDWR,&taint_error) && taint_error==-VFS_EIO);
    assert(ext4_sync(fs)==-VFS_EIO && ext4_freeze_and_sync(fs)==-VFS_EIO && d.events==stopped);
    teardown();ext4_fault_restart(&d);vfs_init();assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
    assert(!(info(fs).flags & (MOUNT_FLAGS_TAINTED|MOUNT_FLAGS_RECOVERY_REQUIRED)));assert(!ext4_freeze_and_sync(fs));save(argv[3],"flush-recovery",d.stable,n);
    puts("PASS sticky failed barrier health, zero later I/O and reboot recovery");fflush(stdout);

    reset(&d,initial);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
    f=create("/mnt/source");assert(vfs_write(f,"NEW",3)==3 && !vfs_close(f));
    f=create("/mnt/dest");assert(vfs_write(f,"OLD",3)==3 && !vfs_close(f));
    creds_t root;creds_init_root(&root);
    file_t *busy_source=vfs_open("/mnt/source",VFS_O_RDONLY);assert(busy_source);
    assert(vfs_replace_creds("/mnt/source","/mnt/dest",&root)==-VFS_EAGAIN);
    assert(!(info(fs).flags & MOUNT_FLAGS_RECOVERY_REQUIRED));assert(!vfs_close(busy_source));
    d.cut=(long)d.events;
    assert(vfs_replace_creds("/mnt/source","/mnt/dest",&root)==-VFS_EIO);
    size_t failed_reads=reads;mount_info_t failed=info(fs);
    unsigned fatal=MOUNT_FLAGS_TAINTED|MOUNT_FLAGS_NAMESPACE_UNCERTAIN|MOUNT_FLAGS_RECOVERY_REQUIRED;
    assert((failed.flags & fatal)==fatal && reads==failed_reads);
    teardown();ext4_fault_restart(&d);vfs_init();assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
    assert(!(info(fs).flags & fatal));assert(!ext4_freeze_and_sync(fs));save(argv[3],"replace-health",d.stable,n);
    puts("PASS recovery-required distinguishes close retry, pins and permanent namespace/I/O failure");fflush(stdout);

    /* Installed-root allocation policy on the actual mounted allocator. The
     * fixture remains /mnt; selecting trusted_root here is a host-only adapter. */
    reset(&d,initial);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
    assert(!vfs_mkdir("/mnt/etc",0755)&&!vfs_mkdir("/mnt/var",0755));
    fs->trusted_root=true;account_io_t account;assert(!ext4_root_account_io(fs,&account));
    mount_info_t start=info(fs);f=create("/mnt/fill");
    while(vfs_write(f,payload,32768)==32768) {}
    while(vfs_write(f,payload,fs->bs)==(int64_t)fs->bs) {}
    assert(vfs_write(f,payload,fs->bs)==-VFS_ENOSPC);assert(!vfs_close(f));
    file_t *tail=create("/mnt/fill-tail");
    while(vfs_write(tail,payload,fs->bs)==(int64_t)fs->bs) {}
    assert(!vfs_close(tail));
    mount_info_t floor=info(fs);assert(floor.free_blocks*fs->bs>=256u*1024 && floor.free_blocks*fs->bs<=256u*1024+4*fs->bs);
    assert(!ext4_root_account_headroom(fs));
    assert(!account.write(account.context,ACCOUNT_TEMP_PASSWD,payload,ACCOUNT_FILE_MAX));
    assert(!fs->account_allocation && info(fs).free_blocks<floor.free_blocks);
    assert(ext4_root_account_headroom(fs)==-VFS_ENOSPC);
    /* Fill /etc's directory records while below the block floor. Empty
     * ordinary files need only inodes until directory growth is necessary. */
    unsigned padding=0;
    for(;;padding++) {
        char path[96];snprintf(path,sizeof(path),"/mnt/etc/padding-%048u",padding);int error;
        file_t *entry=vfs_open_ext(path,VFS_O_CREAT|VFS_O_RDWR,&error);
        if(!entry) {assert(error==-VFS_ENOSPC);break;}assert(!vfs_close(entry));
    }
    assert(padding && info(fs).free_inodes>EXT4_ACCOUNT_RESERVE_INODES);
    assert(!account.rename(account.context,ACCOUNT_TEMP_PASSWD,ACCOUNT_PASSWD));
    assert(!fs->account_allocation && !account.remove(account.context,ACCOUNT_PASSWD));
    for(unsigned i=0;i<padding;i++) {
        char path[96];snprintf(path,sizeof(path),"/mnt/etc/padding-%048u",i);assert(!vfs_unlink(path));
    }
    assert(!vfs_unlink("/mnt/fill")&&!vfs_unlink("/mnt/fill-tail")&&!ext4_sync(fs));
    /* One newly grown empty directory block is intentionally retained. */
    assert(info(fs).free_blocks==start.free_blocks-1 && info(fs).free_inodes==start.free_inodes);
    assert(!ext4_freeze_and_sync(fs));save(argv[3],"block-floor",d.stable,n);
    puts("PASS installed block floor and fixed-role account bypass/reclamation");fflush(stdout);

    reset(&d,initial);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
    assert(!vfs_mkdir("/mnt/etc",0755)&&!vfs_mkdir("/mnt/var",0755));
    fs->trusted_root=true;assert(!ext4_root_account_io(fs,&account));start=info(fs);unsigned created=0;
    for(;;created++) {
        char path[64];snprintf(path,sizeof(path),"/mnt/inode-%u",created);int error;
        f=vfs_open_ext(path,VFS_O_CREAT|VFS_O_RDWR,&error);
        if(!f) {assert(error==-VFS_ENOSPC);break;}assert(!vfs_close(f));
    }
    assert(info(fs).free_inodes==16 && !ext4_root_account_headroom(fs));
    assert(!account.write(account.context,ACCOUNT_TEMP_PASSWD,payload,ACCOUNT_FILE_MAX));
    assert(info(fs).free_inodes==15 && ext4_root_account_headroom(fs)==-VFS_ENOSPC);
    assert(!fs->account_allocation && !account.remove(account.context,ACCOUNT_TEMP_PASSWD));
    assert(info(fs).free_inodes==16);
    for(unsigned i=0;i<created;i++) {char path[64];snprintf(path,sizeof(path),"/mnt/inode-%u",i);assert(!vfs_unlink(path));}
    assert(!ext4_sync(fs)&&info(fs).free_inodes==start.free_inodes);
    assert(!ext4_freeze_and_sync(fs));save(argv[3],"inode-floor",d.stable,n);
    printf("PASS installed inode floor and exact %u-inode reclamation\n",created);fflush(stdout);
    teardown();free(payload);free(initial);free(d.stable);free(d.volatile_bytes);return 0;
}
