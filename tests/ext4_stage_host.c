#define FORTRESS_EXT4_STAGE_TEST
#define TEST_PERMISSIONS_ENFORCEMENT
#define TEST_EXT4_ATOMIC_COUNTERS
#define main previous_mount_main
#include "ext4_mount_host.c"
#undef main
#include <time.h>
static creds_t actor;
static uint8_t payload[EXT4_STAGE_TEST_MAX];
static uint64_t now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;}
static file_t *fresh(const char *path){file_t *f=vfs_open(path,VFS_O_CREAT|VFS_O_RDWR);assert(f);return f;}
static void counters(ext4_fault_disk_t *d){d->events=d->writes=d->flushes=0;}
static void wrap(ext4_mount_t *fs){uint64_t f=spin_lock_irqsave(&e4_lock);fs->engine->writer->head=fs->engine->writer->io->length-2;spin_unlock_irqrestore(&e4_lock,f);}
typedef struct{file_t *file;pthread_barrier_t *barrier;int64_t result;} racer_t;
static void *race(void *p){racer_t *r=p;pthread_barrier_wait(r->barrier);r->result=ext4_stage_test_write(r->file,payload,1,0,&actor);return NULL;}
static void reopen(block_dev_t *dev,ext4_mount_t **fs) {
    teardown();ext4_fault_restart(dev->priv);vfs_init();assert(!ext4_mount_journal_fixture(dev,"/mnt",admitted,fs));
}
int main(int argc,char **argv){
    assert(argc==4);creds_init_root(&actor);memset(payload,'B',sizeof(payload));
    size_t n;uint8_t *initial=load(argv[1],&n);unsigned ss=(unsigned)strtoul(argv[2],NULL,10);
    ext4_fault_disk_t d={.stable=malloc(n),.volatile_bytes=malloc(n),.bytes=n,.cut=-1};assert(d.stable&&d.volatile_bytes);
    block_dev_t dev={.sector_size=ss,.sector_count=n/ss,.read_sector=ext4_fault_read,.write_sector=ext4_fault_write,.flush=ext4_fault_flush,.priv=&d};
    ext4_mount_t *fs;reset(&d,initial);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));unsigned bs=fs->bs;
    file_t *f=fresh("/mnt/stage");counters(&d);uint64_t start=now();unsigned calls=0;
    while(f->offset<10485760){size_t length=10485760-f->offset;if(length>sizeof(payload))length=sizeof(payload);
        int64_t got=ext4_stage_test_write(f,payload,length,f->offset,&actor);assert(got>0&&got<=(int64_t)length);calls++;}
    uint64_t elapsed=now()-start;assert(d.flushes==6u*calls && calls==10485760/(32u*bs));
    printf("{\"block\":%u,\"sector\":%u,\"bytes\":10485760,\"calls\":%u,\"flushes\":%zu,\"writes\":%zu,\"elapsed_ns\":%llu}\n",bs,ss,calls,d.flushes,d.writes,(unsigned long long)elapsed);fflush(stdout);
    size_t events=d.events;uint64_t off=f->offset;
    assert(ext4_stage_test_write(f,payload,1,0,&actor)==-VFS_EAGAIN&&d.events==events&&f->offset==off);
    assert(ext4_stage_test_write(f,payload,0,off,&actor)==-VFS_EINVAL&&d.events==events);
    assert(ext4_stage_test_write(f,payload,sizeof(payload)+1,off,&actor)==-VFS_EINVAL&&d.events==events);
    f->flags|=VFS_O_APPEND;assert(ext4_stage_test_write(f,payload,1,off,&actor)==-VFS_EOPNOTSUPP&&d.events==events);f->flags&=~VFS_O_APPEND;
    f->flags=VFS_O_RDONLY;assert(ext4_stage_test_write(f,payload,1,off,&actor)==-VFS_EBADF&&d.events==events);f->flags=VFS_O_RDWR;
    /* Unaligned EOF consumes a partial block; one call still touches <=32. */
    assert(ext4_stage_test_write(f,payload,1,off,&actor)==1);off++;
    assert(ext4_stage_test_write(f,payload,sizeof(payload),off,&actor)==32u*bs-1);
    assert(f->offset==10485760+32u*bs);assert(!ext4_stage_test_sync(f));
    assert(!vfs_close(f)&&!ext4_freeze_and_sync(fs));save(argv[3],"large",d.stable,n);
    /* Start from one durable byte, measure every callback in one large append. */
    reset(&d,initial);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));f=fresh("/mnt/cut");
    assert(vfs_write(f,"B",1)==1);uint8_t *prepared=malloc(n);assert(prepared);memcpy(prepared,d.stable,n);assert(!vfs_close(f));
    f=vfs_open("/mnt/cut",VFS_O_RDWR);assert(f);f->offset=1;counters(&d);
    wrap(fs);assert(ext4_stage_test_write(f,payload,sizeof(payload),1,&actor)==32u*bs-1);size_t total=d.events;assert(!vfs_close(f));
    unsigned cuts=0;
    /* Full callback cuts, before/after, wrapped journal, four persistence models. */
    for(unsigned persistence=0;persistence<4;persistence++)for(unsigned after=0;after<2;after++)for(size_t cut=0;cut<total;cut++){
        reset(&d,prepared);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
        f=vfs_open("/mnt/cut",VFS_O_RDWR);assert(f);f->offset=1;wrap(fs);counters(&d);d.cut=(long)cut;d.after=after!=0;d.persistence=persistence;
        assert(ext4_stage_test_write(f,payload,sizeof(payload),1,&actor)==-VFS_EIO);
        mount_info_t m={0};assert(ext4_get_mount_info(f->node,&m));assert(m.flags&MOUNT_FLAGS_RECOVERY_REQUIRED);
        assert(!vfs_close(f));reopen(&dev,&fs);
        for(unsigned recovery=0;recovery<2;recovery++){
            f=vfs_open("/mnt/cut",VFS_O_RDONLY);assert(f);uint64_t size=f->node->size;assert(size==1||size==32u*bs);
            bytes_check(f,'B',(size_t)size);assert(!vfs_close(f));assert(!ext4_freeze_and_sync(fs));
            if(!recovery)reopen(&dev,&fs);
        }cuts++;
    }
    save(argv[3],"recovered",d.stable,n);printf("PASS %u callback cuts, two recoveries each\n",cuts);fflush(stdout);
    /* Every measured kmalloc failure before I/O must preserve data and offset. */
    reset(&d,prepared);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));f=vfs_open("/mnt/cut",VFS_O_RDWR);assert(f);f->offset=1;
    allocations=0;assert(ext4_stage_test_write(f,payload,sizeof(payload),1,&actor)>0);size_t allocated=allocations;assert(!vfs_close(f));
    for(size_t at=0;at<allocated;at++){
        reset(&d,prepared);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));f=vfs_open("/mnt/cut",VFS_O_RDWR);assert(f);f->offset=1;
        counters(&d);fail_alloc=(long)at;int64_t got=ext4_stage_test_write(f,payload,sizeof(payload),1,&actor);fail_alloc=-1;
        assert(got<0 && !d.events && f->offset==1);assert(!vfs_close(f));
    }
    printf("PASS %zu allocation failures before I/O\n",allocated);
    reset(&d,initial);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));f=fresh("/mnt/race");
    pthread_barrier_t barrier;assert(!pthread_barrier_init(&barrier,NULL,2));racer_t racers[2]={{f,&barrier,0},{f,&barrier,0}};pthread_t threads[2];
    for(unsigned i=0;i<2;i++)assert(!pthread_create(&threads[i],NULL,race,&racers[i]));
    for(unsigned i=0;i<2;i++)assert(!pthread_join(threads[i],NULL));
    assert((racers[0].result==1&&racers[1].result==-VFS_EAGAIN)||(racers[1].result==1&&racers[0].result==-VFS_EAGAIN));
    assert(f->offset==1&&f->node->size==1);pthread_barrier_destroy(&barrier);assert(!vfs_close(f));
    f=fresh("/mnt/ordinary");assert(vfs_write(f,payload,sizeof(payload))==32768);assert(!vfs_close(f));
    /* Same staged accounting protects account headroom on the prototype path. */
    assert(!vfs_mkdir("/mnt/etc",0755)&&!vfs_mkdir("/mnt/var",0755));fs->trusted_root=true;account_io_t io;assert(!ext4_root_account_io(fs,&io));
    f=fresh("/mnt/floor");int64_t got;
    do{events=d.events;off=f->offset;got=ext4_stage_test_write(f,payload,sizeof(payload),off,&actor);}while(got>0);
    assert(got==-VFS_ENOSPC&&d.events==events&&f->offset==off);
    mount_info_t floor={0};assert(ext4_get_mount_info(f->node,&floor));assert(floor.free_blocks*bs>=EXT4_ACCOUNT_RESERVE_BYTES&&floor.free_inodes>=EXT4_ACCOUNT_RESERVE_INODES);
    assert(!vfs_close(f)&&!vfs_unlink("/mnt/floor"));assert(!ext4_freeze_and_sync(fs));save(argv[3],"floor",d.stable,n);
    puts("PASS shared-offset race, ordinary clamp and account allocation floor");
    teardown();free(prepared);free(initial);free(d.stable);free(d.volatile_bytes);return 0;
}
