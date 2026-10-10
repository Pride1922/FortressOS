/* Actual journaled EXT4/VFS, owned references and pthread exclusion. */
#define main mount_fixture_main
#include "ext4_mount_host.c"
#undef main

static void churn(ext4_mount_t *fs) {
    size_t baseline=live;uint8_t sb[1024];assert(e4_bytes(fs,1024,sb,sizeof(sb)));
    uint32_t free_inodes=e4_u32(sb+16),free_blocks=e4_u32(sb+12);
    for (unsigned round=0;round<1152;round++) {
        file_t *f=vfs_open("/mnt/memory-churn.bin",VFS_O_CREAT|VFS_O_RDWR);assert(f);
        assert(!vfs_close(f) && !vfs_unlink("/mnt/memory-churn.bin"));
        assert(!vfs_lookup_ref("/mnt/memory-churn.bin",NULL));
        assert(fs->nodes==1 && live==baseline && !fs->engine->orphan_count);
    }
    assert(e4_bytes(fs,1024,sb,sizeof(sb)));
    assert(e4_u32(sb+16)==free_inodes && e4_u32(sb+12)==free_blocks);
    assert(!fs->engine->tainted && !ext4_sync(fs));
    printf("churn PASS: 1152 journaled create/close/unlink cycles, exact live allocation and disk counters; cached=%u\n",fs->nodes);
}
typedef struct { pthread_barrier_t barrier;vfs_node_t *node; } pin_race_t;
static void *lookup_worker(void *opaque) {
    pin_race_t *race=opaque;
    race->node=vfs_lookup_ref("/mnt/race.bin",NULL);assert(race->node);
    int r=pthread_barrier_wait(&race->barrier);assert(!r || r==PTHREAD_BARRIER_SERIAL_THREAD);
    r=pthread_barrier_wait(&race->barrier);assert(!r || r==PTHREAD_BARRIER_SERIAL_THREAD);
    vfs_stat_t st;assert(!vfs_stat(race->node,&st) && st.type==VFS_FILE);
    assert(((e4_node_t *)race->node)->removed);
    vfs_node_put(race->node);return NULL;
}
static void lifetimes(ext4_mount_t *fs) {
    assert(!vfs_mkdir("/mnt/parent",0));
    file_t *f=vfs_open("/mnt/parent/child",VFS_O_CREAT|VFS_O_RDWR);assert(f);
    vfs_node_t *child=vfs_lookup_ref("/mnt/parent/child",NULL);assert(child);
    assert(!vfs_close(f) && !vfs_unlink("/mnt/parent/child") && !vfs_unlink("/mnt/parent"));
    assert(fs->nodes==3 && ((e4_node_t *)child->parent)->removed);
    vfs_node_put(child);assert(fs->nodes==1);
    f=vfs_open("/mnt/race.bin",VFS_O_CREAT|VFS_O_RDWR);assert(f && !vfs_close(f));
    pin_race_t race={0};assert(!pthread_barrier_init(&race.barrier,NULL,2));pthread_t worker;
    assert(!pthread_create(&worker,NULL,lookup_worker,&race));
    int r=pthread_barrier_wait(&race.barrier);assert(!r || r==PTHREAD_BARRIER_SERIAL_THREAD);
    assert(!vfs_unlink("/mnt/race.bin"));
    f=vfs_open("/mnt/race.bin",VFS_O_CREAT|VFS_O_RDWR);assert(f && f->node!=race.node);
    assert(!vfs_close(f) && !vfs_unlink("/mnt/race.bin"));
    r=pthread_barrier_wait(&race.barrier);assert(!r || r==PTHREAD_BARRIER_SERIAL_THREAD);
    assert(!pthread_join(worker,NULL) && !pthread_barrier_destroy(&race.barrier) && fs->nodes==1);
    f=vfs_open("/mnt/open.bin",VFS_O_CREAT|VFS_O_RDWR);assert(f);
    file_t *other=vfs_open("/mnt/open.bin",VFS_O_RDWR);assert(other);
    uint8_t bytes[17];memset(bytes,0x6a,sizeof(bytes));assert(vfs_write(f,bytes,sizeof(bytes))==sizeof(bytes));
    f->ref_count++;assert(!vfs_unlink("/mnt/open.bin"));bytes_check(other,0x6a,sizeof(bytes));
    assert(!vfs_close(f) && !vfs_close(other));
    assert(fs->nodes==2 && fs->engine->orphan_count==1);
    bytes_check(f,0x6a,sizeof(bytes));assert(!vfs_close(f));
    assert(fs->nodes==1 && !fs->engine->orphan_count);
    puts("lifetime PASS: transient lookup/unlink pthread ordering, inode reuse, detached parent, independent/dup open-unlink pins");
}
static void saturation(ext4_mount_t *fs) {
    vfs_node_t *pins[E4_NODES-1];size_t baseline=live;
    for (unsigned i=0;i<E4_NODES-1;i++) {
        file_t *f=vfs_open("/mnt/memory-churn.bin",VFS_O_CREAT|VFS_O_RDWR);assert(f);
        pins[i]=vfs_lookup_ref("/mnt/memory-churn.bin",NULL);assert(pins[i]);
        assert(!vfs_close(f) && !vfs_unlink("/mnt/memory-churn.bin"));
    }
    assert(fs->nodes==E4_NODES && live==baseline+E4_NODES-1);
    ext4_fault_disk_t *d=fs->dev->priv;size_t events=d->events;int error=0;
    assert(!vfs_open_ext("/mnt/README.txt",VFS_O_RDONLY,&error) && error==-VFS_EFBIG);
    assert(!vfs_open_ext("/mnt/memory-churn.bin",VFS_O_CREAT|VFS_O_RDWR,&error) && error==-VFS_EFBIG);
    assert(d->events==events && live==baseline+E4_NODES-1);
    vfs_node_put(pins[0]);
    file_t *existing=vfs_open("/mnt/README.txt",VFS_O_RDONLY);assert(existing && !vfs_close(existing));
    assert(fs->nodes==E4_NODES);
    file_t *f=vfs_open("/mnt/memory-churn.bin",VFS_O_CREAT|VFS_O_RDWR);assert(f);
    assert(!vfs_close(f) && !vfs_unlink("/mnt/memory-churn.bin"));
    for (unsigned i=1;i<E4_NODES-1;i++) {
        assert(((e4_node_t *)pins[i])->removed && ((e4_node_t *)pins[i])->refs==1);
        vfs_node_put(pins[i]);
    }
    assert(fs->nodes==1 && live==baseline);
    fail_alloc=0;
    assert(!vfs_lookup_ref("/mnt/README.txt",&error) && error==-VFS_ENOMEM);
    fail_alloc=-1;assert(fs->cached[0]->refs==0);
    existing=vfs_open("/mnt/README.txt",VFS_O_RDONLY);assert(existing && !vfs_close(existing));
    vfs_node_t *legacy=vfs_create("/mnt/legacy.bin",VFS_FILE);assert(legacy);
    assert(!vfs_unlink("/mnt/legacy.bin") && ((e4_node_t *)legacy)->removed);
    assert(!strcmp(legacy->name,"legacy.bin") && ((e4_node_t *)legacy)->legacy);
    puts("capacity PASS: 1023 owned pins, EFBIG not false ENOENT, zero-write rejection, release/leaf eviction recovery, OOM balance and legacy-pointer compatibility");
}
static void api_errors(ext4_mount_t *fs) {
    file_t *f=vfs_open("/mnt/source.bin",VFS_O_CREAT|VFS_O_RDWR);assert(f);
    vfs_node_t *held=vfs_lookup_ref("/mnt/source.bin",NULL);assert(held);
    assert(vfs_rename("/mnt/source.bin/child","/tmp/moved.bin")==-8);
    assert(!vfs_mkdir("/mnt/dest",0));
    assert(vfs_rename("/mnt/source.bin/","/mnt/dest/new.bin")==-8);
    assert(!vfs_rename("/mnt/source.bin","/mnt/dest/new.bin"));
    assert(!strcmp(held->path,"/mnt/dest/new.bin") && held==f->node);
    assert(!vfs_unlink("/mnt/dest/new.bin") && !vfs_close(f) && !vfs_unlink("/mnt/dest"));
    assert(((e4_node_t *)held->parent)->removed);vfs_node_put(held);assert(fs->nodes==1);
    int error=0;ext4_fault_disk_t *d=fs->dev->priv;size_t events=d->events;
    fail_alloc=1;assert(!vfs_open_ext("/mnt/oom.bin",VFS_O_CREAT|VFS_O_RDWR,&error) && error==-VFS_ENOMEM);
    fail_alloc=-1;assert(!fs->cached[0]->refs && d->events==events && fs->nodes==1);
    e4_cache_drop(fs,true);fail_read=0;
    assert(!vfs_lookup_ref("/mnt/README.txt",&error) && error==-VFS_EIO);
    fail_read=-1;assert(!fs->cached[0]->refs && d->events==events);
    fs->cached[0]->refs=UINT32_MAX;
    assert(!vfs_lookup_ref("/mnt",&error) && error==-VFS_EFBIG);
    fs->cached[0]->refs=0;
    assert(!vfs_lookup_ref("/mnt/missing/child",&error) && error==-VFS_ENOENT && !fs->cached[0]->refs);
    puts("owned API PASS: rename pin/parent transfer, OOM/IO/type/reference-overflow errors and zero-write cleanup");
}
int main(int argc,char **argv) {
    assert(argc==3 || (argc==4 && !strcmp(argv[3],"--api")));setvbuf(stdout,NULL,_IONBF,0);
    size_t n;uint8_t *initial=load(argv[1],&n);
    ext4_fault_disk_t d={.stable=malloc(n),.volatile_bytes=malloc(n),.bytes=n,.cut=-1};assert(d.stable && d.volatile_bytes);
    block_dev_t dev={.sector_size=512,.sector_count=n/512,.read_sector=ext4_fault_read,
        .write_sector=ext4_fault_write,.flush=ext4_fault_flush,.priv=&d};
    reset(&d,initial);ext4_mount_t *fs=NULL;assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
    if (argc==3) { churn(fs);lifetimes(fs);saturation(fs); }
    else { lifetimes(fs);api_errors(fs); }
    assert(!ext4_freeze_and_sync(fs));clean_check(fs);save(argv[2],"exhausted-clean",d.stable,n);
    teardown();ext4_fault_restart(&d);vfs_init();assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
    file_t *f=vfs_open("/mnt/README.txt",VFS_O_RDONLY);assert(f && !vfs_close(f));
    f=vfs_open("/mnt/memory-churn.bin",VFS_O_CREAT|VFS_O_RDWR);assert(f);
    assert(!vfs_close(f) && !vfs_unlink("/mnt/memory-churn.bin"));
    assert(!ext4_freeze_and_sync(fs));clean_check(fs);save(argv[2],"remount-clean",d.stable,n);
    teardown();free(initial);free(d.stable);free(d.volatile_bytes);
    puts("EXT4 memory reclamation PASS: bounded cache, durability, remount and zero host allocation leaks");return 0;
}
