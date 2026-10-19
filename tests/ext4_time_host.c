#define main ext4_existing_mount_suite_main
#include "ext4_mount_host.c"
#undef main
#include "../src/fs/ext4_time.h"
extern uint64_t ext4_time_test_seconds;

static int64_t decode_time(const uint8_t *raw,unsigned base,unsigned extra) {
    uint32_t lo=e4_u32(raw+base);
    int64_t signed_low=(lo&0x80000000u)?(int64_t)lo-4294967296LL:lo;
    return signed_low+(int64_t)(e4_u32(raw+extra)&3u)*4294967296LL;
}
static void inode_bytes(file_t *file,uint8_t raw[256]) {
    e4_inode_t *in=file->node->fs_private;ext4_mount_t *fs=in->fs;
    uint32_t g=(in->ino-1)/fs->ipg,index=(in->ino-1)%fs->ipg;
    uint64_t flags=spin_lock_irqsave(&e4_lock);
    assert(e4_bytes(fs,(uint64_t)e4_u32(fs->gd[g].raw+8)*fs->bs+(uint64_t)index*256,raw,256));
    spin_unlock_irqrestore(&e4_lock,flags);
}
static void codec(void) {
    const int64_t values[]={-2147483648LL,-1,0,1792368000,2147483647,2147483648LL,4102444800LL,15032385535LL};
    for(unsigned i=0;i<sizeof(values)/sizeof(values[0]);i++) {
        uint32_t low,extra;assert(ext4_time_encode(values[i],999999999,&low,&extra));
        uint8_t raw[256]={0};e4_p16(raw+128,32);
        assert(ext4_inode_time_set(raw,EXT4_MTIME,values[i],999999999));
        assert(decode_time(raw,16,136)==values[i] && (e4_u32(raw+136)>>2)==999999999);
    }
    uint32_t low=123,extra=456;
    assert(!ext4_time_encode(INT64_MAX,0,&low,&extra)&&low==123&&extra==456);
    assert(!ext4_time_encode(INT64_MIN,0,&low,&extra)&&low==123&&extra==456);
    assert(!ext4_time_encode(0,1000000000,&low,&extra)&&low==123&&extra==456);
    uint8_t raw[256]={0},copy[256];e4_p16(raw+128,4);memcpy(copy,raw,256);
    assert(!ext4_inode_time_set(raw,EXT4_MTIME,2147483648LL,0)&&!memcmp(copy,raw,256));
    assert(!ext4_inode_time_set(raw,EXT4_CRTIME,1,0)&&!memcmp(copy,raw,256));
    assert(ext4_inode_time_set(raw,EXT4_MTIME,2147483647,0)&&!memcmp(raw+132,copy+132,124));
    puts("PASS pure ext4 timestamp codec and declared-extra-area bounds");
}
static void short_extra(ext4_fault_disk_t *disk,block_dev_t *dev,const uint8_t *initial,
                        const char *prefix) {
    const unsigned sizes[]={4,8,12,16,20};
    for(unsigned s=0;s<sizeof(sizes)/sizeof(sizes[0]);s++) {
        reset(disk,initial);ext4_mount_t *fs=NULL;
        assert(!ext4_mount_journal_fixture(dev,"/mnt",admitted,&fs));
        ext4_time_test_seconds=1792368000;
        file_t *f=vfs_open("/mnt/short",VFS_O_CREAT|VFS_O_RDWR);assert(f);
        assert(vfs_write(f,"before",6)==6);
        file_t *append=vfs_open("/mnt/short",VFS_O_WRONLY|VFS_O_APPEND);assert(append);
        uint64_t flags=spin_lock_irqsave(&e4_lock);ext4_engine_t *engine=fs->engine;
        assert(!e4_namespace_begin(engine));e4_inode_t *in=f->node->fs_private;
        uint8_t *raw=e4_inode_image(engine,in->ino);assert(raw);
        e4_p16(raw+128,sizes[s]);memset(raw+132,0,124);
        e4_inode_sum(fs,in->ino,raw);assert(!e4_namespace_end(engine,0));
        spin_unlock_irqrestore(&e4_lock,flags);
        uint8_t before[256],after[256];inode_bytes(f,before);
        /* Representable writes work, including append, without touching bytes
         * beyond the declared extension area. */
        f->offset=0;assert(vfs_write(f,"AFTER!",6)==6);
        assert(vfs_write(append,"+",1)==1);inode_bytes(f,after);
        assert(!memcmp(before+128+sizes[s],after+128+sizes[s],128-sizes[s]));
        if(sizes[s]>=12) {
            ext4_time_test_seconds=4102444800ULL;f->offset=0;
            assert(vfs_write(f,"AFTER!",6)==6);inode_bytes(f,after);
            assert(decode_time(after,12,132)==4102444800LL);
            assert(decode_time(after,16,136)==4102444800LL);
            assert(!memcmp(before+128+sizes[s],after+128+sizes[s],128-sizes[s]));
        }
        assert(f->node->size==7);
        uint8_t *snapshot=malloc(disk->bytes);assert(snapshot);
        memcpy(snapshot,disk->volatile_bytes,disk->bytes);
        assert(!memcmp(snapshot,disk->stable,disk->bytes));
        size_t writes=disk->writes,flushes=disk->flushes;
        const uint64_t rejected[]={2147483648ULL,UINT64_MAX};
        for(unsigned t=0;t<2;t++) {
            /* Sizes >=12 contain both ctime and mtime extensions and support
             * 2038. All variants must reject the unrepresentable UINT64_MAX. */
            if(!t && sizes[s]>=12)continue;
            ext4_time_test_seconds=rejected[t];inode_bytes(f,before);
            uint64_t position=f->offset,append_position=append->offset;
            assert(vfs_write(f,"BAD",3)==-VFS_EOPNOTSUPP);
            assert(vfs_write(append,"BAD",3)==-VFS_EOPNOTSUPP);
            assert(f->offset==position && append->offset==append_position && f->node->size==7);
            inode_bytes(f,after);assert(!memcmp(before,after,256));
            assert(disk->writes==writes && disk->flushes==flushes);
            assert(!memcmp(snapshot,disk->stable,disk->bytes));
            assert(!memcmp(snapshot,disk->volatile_bytes,disk->bytes));
            f->offset=0;char bytes[8]={0};assert(vfs_read(f,bytes,7)==7);
            assert(!memcmp(bytes,"AFTER!+",7));f->offset=position;
        }
        ext4_time_test_seconds=UINT64_MAX;
        assert(!vfs_open("/mnt/refused",VFS_O_CREAT|VFS_O_RDWR));
        assert(vfs_mkdir("/mnt/refused-dir",0755)==-VFS_EOPNOTSUPP);
        assert(!vfs_lookup("/mnt/refused") && !vfs_lookup("/mnt/refused-dir"));
        assert(disk->writes==writes && disk->flushes==flushes);
        assert(!memcmp(snapshot,disk->stable,disk->bytes));
        assert(!memcmp(snapshot,disk->volatile_bytes,disk->bytes));
        free(snapshot);assert(!vfs_close(append));assert(!vfs_close(f));
        ext4_time_test_seconds=1792368000;assert(!ext4_freeze_and_sync(fs));
        char suffix[32];snprintf(suffix,sizeof(suffix),"short-%u",sizes[s]);
        save(prefix,suffix,disk->stable,disk->bytes);
    }
    puts("PASS mounted short-extra overwrite/append and rejected write/create/mkdir preservation (bytes, inode, offsets, device writes/flushes)");
}
static void ownership_rejection(ext4_fault_disk_t *disk,block_dev_t *dev,const uint8_t *initial) {
    reset(disk,initial);ext4_mount_t *fs=NULL;
    assert(!ext4_mount_journal_fixture(dev,"/mnt",admitted,&fs));
    /* Deliberately reserve a block without assigning an inode owner. Bitmap,
     * free counters and checksums remain internally consistent, so only the
     * complete ownership comparison should reject the next mount. */
    uint64_t flags=spin_lock_irqsave(&e4_lock);ext4_engine_t *engine=fs->engine;
    assert(!e4_namespace_begin(engine));assert(e4_allocate(engine));
    assert(!e4_namespace_end(engine,0));spin_unlock_irqrestore(&e4_lock,flags);
    assert(!ext4_freeze_and_sync(fs));
    uint8_t *invalid=malloc(disk->bytes);assert(invalid);
    memcpy(invalid,disk->stable,disk->bytes);reset(disk,invalid);
    fs=NULL;const char *stage=NULL;
    assert(ext4_mount_journal_fixture_diagnose(dev,"/mnt",admitted,&fs,&stage)==-VFS_EIO);
    assert(!fs && !e4_active && stage && !strcmp(stage,"recovery-preview"));
    assert(!disk->writes && !disk->flushes);
    assert(!memcmp(invalid,disk->stable,disk->bytes));
    assert(!memcmp(invalid,disk->volatile_bytes,disk->bytes));free(invalid);
    puts("PASS checksum-valid unowned allocation rejects before writes/publication");
}
int main(int argc,char **argv) {
    assert(argc==4);codec();size_t bytes;uint8_t *initial=load(argv[1],&bytes);
    unsigned sector=(unsigned)strtoul(argv[2],NULL,10);
    ext4_fault_disk_t disk={.stable=malloc(bytes),.volatile_bytes=malloc(bytes),.bytes=bytes,.cut=-1};
    assert(disk.stable&&disk.volatile_bytes);
    block_dev_t dev={.sector_size=sector,.sector_count=bytes/sector,.read_sector=ext4_fault_read,
        .write_sector=ext4_fault_write,.flush=ext4_fault_flush,.priv=&disk};
    const uint64_t epochs[]={1792368000,2147483647,2147483648ULL,4102444800ULL};
    for(unsigned i=0;i<4;i++) {
        reset(&disk,initial);ext4_mount_t *fs=NULL;
        const char *stage=NULL;
        int mounted=ext4_mount_journal_fixture_diagnose(&dev,"/mnt",admitted,&fs,&stage);
        if(mounted)fprintf(stderr,"mount error %d at %s\n",mounted,stage?stage:"unknown");
        assert(!mounted);
        ext4_time_test_seconds=epochs[i];
        file_t *f=vfs_open("/mnt/stamp",VFS_O_CREAT|VFS_O_RDWR);assert(f);
        uint8_t raw[256];inode_bytes(f,raw);
        if(decode_time(raw,144,148)!=(int64_t)epochs[i] || e4_u32(raw+148)>>2) {
            fprintf(stderr,"CREATION_TIMESTAMP_REGRESSION: expected %llu, seconds=%u extra=%u\n",
                (unsigned long long)epochs[i],e4_u32(raw+144),e4_u32(raw+148));
            assert(!vfs_close(f));teardown();free(initial);free(disk.stable);free(disk.volatile_bytes);return 2;
        }
        assert(!vfs_mkdir("/mnt/directory",0755));
        /* Inject valid inherited fractional/epoch bits, then overwrite them via
         * the actual mounted write path. Creation time must remain unchanged. */
        uint64_t flags=spin_lock_irqsave(&e4_lock);ext4_engine_t *engine=fs->engine;
        assert(!e4_namespace_begin(engine));e4_inode_t *in=f->node->fs_private;
        uint8_t *staged=e4_inode_image(engine,in->ino);assert(staged);
        e4_p32(staged+132,0x77777777);e4_p32(staged+136,0x66666666);
        e4_inode_sum(fs,in->ino,staged);assert(!e4_namespace_end(engine,0));
        spin_unlock_irqrestore(&e4_lock,flags);
        ext4_time_test_seconds=epochs[i]+1;
        assert(vfs_write(f,"bytes",5)==5);inode_bytes(f,raw);
        assert(decode_time(raw,12,132)==(int64_t)epochs[i]+1 && !(e4_u32(raw+132)>>2));
        assert(decode_time(raw,16,136)==(int64_t)epochs[i]+1 && !(e4_u32(raw+136)>>2));
        assert(decode_time(raw,144,148)==(int64_t)epochs[i]);
        assert(!vfs_close(f));assert(!ext4_freeze_and_sync(fs));
        char suffix[32];snprintf(suffix,sizeof(suffix),"epoch-%u",i);save(argv[3],suffix,disk.stable,bytes);
    }
    short_extra(&disk,&dev,initial,argv[3]);
    ownership_rejection(&disk,&dev,initial);
    teardown();free(initial);free(disk.stable);free(disk.volatile_bytes);
    puts("PASS actual mounted timestamp create/directory/write paths and stale extension replacement");return 0;
}
