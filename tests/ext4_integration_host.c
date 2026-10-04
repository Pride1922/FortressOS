/* Phase 8.6: actual mounted operations, transaction coverage and restart oracle.
 * Reuse the Phase-8.5 allocator, ranked pthread lock and disposable disk shims. */
#define main phase85_main
#include "ext4_mount_host.c"
#undef main

static size_t metadata_writes,data_writes,journal_writes;
static size_t selected[128],selected_count;
static int previous_state=-1;
static bool tracing;
static void select_event(size_t event) {
    for (size_t k=0;k<selected_count;k++) if (selected[k]==event) return;
    assert(selected_count<128);selected[selected_count++]=event;
}
static bool audited_write(block_dev_t *dev,uint64_t lba,const void *opaque) {
    ext4_fault_disk_t *d=dev->priv;
    if (e4_active && e4_active->journal_mounted) {
        spin_debug_assert_held(&e4_lock);
        jbd2_writer_t *w=e4_active->engine->writer;
        int state=(int)w->state;
        if (tracing && state!=previous_state) select_event(d->events);
        previous_state=state;
        /* Inspect each changed filesystem-block slice, including sub-sector
         * RMW neighbors. Every home change must equal an owned staged image. */
        const uint8_t *bytes=opaque;uint64_t start=lba*dev->sector_size;
        for (unsigned at=0;at<dev->sector_size;) {
            uint32_t block=(uint32_t)((start+at)/w->io->bs);
            unsigned skip=(unsigned)((start+at)%w->io->bs),n=w->io->bs-skip;
            if (n>dev->sector_size-at) n=dev->sector_size-at;
            bool journal=false;
            for (unsigned k=0;k<w->io->length;k++) if (w->map[k]==block) { journal=true;break; }
            if (journal) {
                assert(state==JBD2_WRITE_COMMITTING || state==JBD2_WRITE_CHECKPOINTING);
                journal_writes++;
            } else if (memcmp(bytes+at,d->volatile_bytes+start+at,n)) {
                j_image_t *images=state==JBD2_WRITE_CHECKPOINTING ? w->metadata : w->data;
                unsigned count=state==JBD2_WRITE_CHECKPOINTING ? w->metadata_count : w->data_count;
                assert(state==JBD2_WRITE_COMMITTING || state==JBD2_WRITE_CHECKPOINTING);
                bool found=false;
                for (unsigned k=0;k<count;k++) if (images[k].block==block &&
                    !memcmp(bytes+at,images[k].bytes+skip,n)) { found=true;break; }
                assert(found); /* A direct metadata-home bypass fails here. */
                if (state==JBD2_WRITE_CHECKPOINTING) metadata_writes++;else data_writes++;
            }
            at+=n;
        }
    }
    return ext4_fault_write(dev,lba,opaque);
}
static bool audited_flush(block_dev_t *dev) {
    ext4_fault_disk_t *d=dev->priv;
    if (tracing) select_event(d->events);
    return ext4_fault_flush(dev);
}
enum { CREATE, MKDIR, WRITE, APPEND, RENAME, TRUNCATE, UNLINK, PIN_UNLINK,
       LAST_CLOSE, RMDIR, REUSE, SYNC, FREEZE, OPERATIONS };
static const char *names[]={"create","mkdir","write","append","rename","truncate",
    "unlink","open-unlink","last-close","rmdir","reuse","sync","freeze"};
static file_t *prepare(block_dev_t *dev,const uint8_t *initial,unsigned op) {
    reset(dev->priv,initial);ext4_mount_t *fs=NULL;
    assert(!ext4_mount_journal_fixture(dev,"/mnt",admitted,&fs));
    if (op==RENAME || op==RMDIR) assert(!vfs_mkdir("/mnt/sub",0));
    if (op==REUSE) {
        assert(!vfs_unlink("/mnt/target.bin"));
        return vfs_open("/mnt/reuse.bin",VFS_O_CREAT|VFS_O_RDWR);
    }
    if (op==WRITE || op==APPEND || op==TRUNCATE || op==PIN_UNLINK || op==LAST_CLOSE) {
        file_t *f=vfs_open("/mnt/target.bin",VFS_O_RDWR|(op==APPEND ? VFS_O_APPEND : 0));assert(f);
        if (op==WRITE) f->offset=3*fs->bs;
        if (op==LAST_CLOSE) assert(!vfs_unlink("/mnt/target.bin"));
        return f;
    }
    return NULL;
}
static int64_t operate(unsigned op,file_t *f) {
    uint8_t bytes[8192];memset(bytes,'I',sizeof(bytes));unsigned bs=e4_active->bs;
    switch (op) {
    case CREATE: { int error=0;return vfs_create_ext("/mnt/new.bin",VFS_FILE,&error) ? 0 : error; }
    case MKDIR:return vfs_mkdir("/mnt/newdir",0);
    case WRITE:case REUSE:return vfs_write(f,bytes,2*bs);
    case APPEND:return vfs_write(f,bytes,17);
    case RENAME:return vfs_rename("/mnt/target.bin","/mnt/sub/renamed.bin");
    case TRUNCATE:return vfs_truncate(f->node,bs+17);
    case UNLINK:case PIN_UNLINK:return vfs_unlink("/mnt/target.bin");
    case LAST_CLOSE:return vfs_close(f);
    case RMDIR:return vfs_unlink("/mnt/sub");
    case SYNC:return ext4_sync_journal_fixture();
    case FREEZE:return ext4_freeze_journal_fixture();
    default:abort();
    }
}
static void recovered_oracle(unsigned op,unsigned bs) {
    vfs_node_t *target=vfs_lookup("/mnt/target.bin"),*renamed=vfs_lookup("/mnt/sub/renamed.bin");
    if (op==RENAME) assert((target!=NULL)!=(renamed!=NULL));
    if (op==LAST_CLOSE || op==REUSE) assert(!target);
    vfs_node_t *node=op==REUSE ? vfs_lookup("/mnt/reuse.bin") : target ? target : renamed;
    if (node) {
        uint64_t old=op==REUSE ? 0 : 3u*bs,new_size=old;
        if (op==WRITE) new_size=5u*bs;
        if (op==APPEND) new_size=3u*bs+17;
        if (op==TRUNCATE) new_size=bs+17;
        if (op==REUSE) new_size=2u*bs;
        assert(node->size==old || node->size==new_size);
        file_t *f=vfs_open(node->path,VFS_O_RDONLY);assert(f);uint8_t bytes[4096];uint64_t at=0;
        while (at<node->size) {
            size_t n=node->size-at;if (n>sizeof(bytes)) n=sizeof(bytes);
            assert(vfs_read(f,bytes,n)==(int64_t)n);
            for (size_t k=0;k<n;k++) assert(bytes[k]==(op==REUSE || at+k>=3u*bs ? 'I' : 'O'));
            at+=n;
        }
        assert(!vfs_close(f));
    }
    assert(!e4_active->engine->orphan_count);
    assert(!ext4_freeze_and_sync(e4_active));clean_check(e4_active);
}
static pthread_barrier_t shared_barrier;
static int64_t (*real_write)(vfs_node_t *,uint64_t *,bool,const void *,size_t);
static int64_t paired_write(vfs_node_t *node,uint64_t *offset,bool append,const void *bytes,size_t n) {
    int r=pthread_barrier_wait(&shared_barrier);assert(!r || r==PTHREAD_BARRIER_SERIAL_THREAD);
    return real_write(node,offset,append,bytes,n);
}
typedef struct { file_t *file;unsigned id; } append_arg_t;
static void *record_writer(void *opaque) {
    append_arg_t *arg=opaque;uint8_t bytes[32];memset(bytes,(int)('A'+arg->id),sizeof(bytes));
    for (unsigned k=0;k<32;k++) {
        bytes[0]=(uint8_t)arg->id;bytes[1]=(uint8_t)k;
        assert(vfs_write(arg->file,bytes,sizeof(bytes))==(int64_t)sizeof(bytes));
    }
    return NULL;
}
static void shared_offsets(block_dev_t *dev,const uint8_t *initial,const char *prefix,bool independent) {
    (void)prepare(dev,initial,SYNC);
    file_t *a=vfs_open("/mnt/records.bin",VFS_O_CREAT|VFS_O_RDWR|(independent ? VFS_O_APPEND : 0));assert(a);
    file_t *b=independent ? vfs_open("/mnt/records.bin",VFS_O_RDWR|VFS_O_APPEND) : a;assert(b);
    if (!independent) a->ref_count++;
    real_write=a->node->write;a->node->write=paired_write;
    assert(!pthread_barrier_init(&shared_barrier,NULL,2));
    append_arg_t first={a,0},second={b,1};pthread_t t1,t2;
    assert(!pthread_create(&t1,NULL,record_writer,&first) && !pthread_create(&t2,NULL,record_writer,&second));
    assert(!pthread_join(t1,NULL) && !pthread_join(t2,NULL) && !pthread_barrier_destroy(&shared_barrier));
    a->node->write=real_write;assert(a->node->size==2048);
    if (!independent) assert(a->offset==2048);
    a->offset=0;uint8_t bytes[32];bool seen[2][32]={{false}};
    for (unsigned k=0;k<64;k++) {
        assert(vfs_read(a,bytes,sizeof(bytes))==(int64_t)sizeof(bytes));
        assert(bytes[0]<2 && bytes[1]<32 && !seen[bytes[0]][bytes[1]]);seen[bytes[0]][bytes[1]]=true;
        for (unsigned n=2;n<32;n++) assert(bytes[n]=='A'+bytes[0]);
    }
    assert(!vfs_close(a) && !vfs_close(b));
    assert(!ext4_freeze_and_sync(e4_active));clean_check(e4_active);
    save(prefix,independent ? "independent-append" : "shared-write",((ext4_fault_disk_t *)dev->priv)->stable,
        ((ext4_fault_disk_t *)dev->priv)->bytes);
    puts(independent ? "independent append PASS: 64 intact unique records" :
        "shared offset PASS: forced concurrent non-append writes, 64 intact unique records");fflush(stdout);
}
static size_t staging_failures(block_dev_t *dev,const uint8_t *initial,bool truncate_only) {
    ext4_fault_disk_t *d=dev->priv;size_t checked=0;
    for (unsigned op=0;op<OPERATIONS;op++) {
        if (truncate_only && op!=TRUNCATE) continue;
        file_t *f=prepare(dev,initial,op);e4_cache_drop(e4_active,false);reads=allocations=0;
        assert(operate(op,f)>=0);size_t rd=reads,al=allocations;
        if (f && op!=LAST_CLOSE) assert(!vfs_close(f));
        for (unsigned kind=0;kind<2;kind++) for (size_t at=0;at<(kind ? rd : al);at++) {
            f=prepare(dev,initial,op);e4_cache_drop(e4_active,false);
            d->events=d->writes=d->flushes=0;
            if (kind) fail_read=(long)at;else fail_alloc=(long)at;
            int64_t r=operate(op,f);fail_read=fail_alloc=-1;
            assert(r<0 || op==LAST_CLOSE);
            if (op==TRUNCATE && !e4_active->engine->tainted) cache_check(f->node);
            if (!d->writes && e4_active->engine->tainted) {
                /* A failed sub-sector RMW read after commit entered I/O is
                 * uncertain even before its first write callback. */
                assert(kind && e4_active->engine->writer->state==JBD2_WRITE_FAILED);
            }
            if (e4_active->engine->tainted) {
                size_t stop=d->events;assert(ext4_sync(e4_active)==-VFS_EIO && d->events==stop);
            }
            if (f && op!=LAST_CLOSE) assert(!vfs_close(f));
            teardown();ext4_fault_restart(d);vfs_init();ext4_mount_t *fs=NULL;
            assert(!ext4_mount_journal_fixture(dev,"/mnt",admitted,&fs));
            recovered_oracle(op,fs->bs);checked++;
        }
        /* Exercise semantic credit reservation with an explicitly injected
         * one-slot ring budget. Restore before retry; no disk identity changes. */
        if (op!=SYNC) {
            f=prepare(dev,initial,op);jbd2_writer_t *w=e4_active->engine->writer;
            uint32_t length=w->io->length;w->io->length=w->io->first+1;
            d->events=d->writes=d->flushes=0;uint64_t offset=f ? f->offset : 0;
            int64_t r=operate(op,f);assert(r==-VFS_ENOSPC || op==LAST_CLOSE);
            assert(!d->writes && !e4_active->engine->tainted && w->state==JBD2_WRITE_IDLE);
            if (f && op!=LAST_CLOSE) assert(f->offset==offset);
            w->io->length=length;
            if (f && op!=LAST_CLOSE) assert(!vfs_close(f));
            assert(!ext4_freeze_and_sync(e4_active));clean_check(e4_active);checked++;
        }
        printf("staging %s PASS reads=%zu allocations=%zu\n",names[op],rd,al);fflush(stdout);
    }
    return checked;
}
#ifndef EXT4_INTEGRATION_LIBRARY
int main(int argc,char **argv) {
    assert(argc==4 || (argc==5 && (!strcmp(argv[4],"--offset-only") || !strcmp(argv[4],"--staging") ||
        !strcmp(argv[4],"--truncate-staging"))));
    size_t n;uint8_t *initial=load(argv[1],&n);
    unsigned ss=(unsigned)strtoul(argv[2],NULL,10),bs=1024u<<e4_u32(initial+1048);
    ext4_fault_disk_t disk={.stable=malloc(n),.volatile_bytes=malloc(n),.bytes=n,.cut=-1};
    assert(disk.stable && disk.volatile_bytes);
    block_dev_t dev={.sector_size=ss,.sector_count=n/ss,.read_sector=ext4_fault_read,
        .write_sector=audited_write,.flush=audited_flush,.priv=&disk};
    size_t cuts=0;
    for (unsigned op=0;argc==4 && op<OPERATIONS;op++) {
        file_t *f=prepare(&dev,initial,op);assert(op!=REUSE || f);
        disk.events=disk.writes=disk.flushes=0;selected_count=0;previous_state=-1;tracing=true;
        assert(operate(op,f)>=0);tracing=false;size_t events=disk.events;
        if (events) { select_event(0);select_event(events-1); }
        size_t points[128],count=selected_count;memcpy(points,selected,count*sizeof(size_t));
        if (f && op!=LAST_CLOSE) assert(!vfs_close(f));
        assert(!ext4_freeze_and_sync(e4_active));clean_check(e4_active);
        save(argv[3],names[op],disk.stable,n);
        for (unsigned persistence=0;persistence<2;persistence++) for (unsigned after=0;after<2;after++)
            for (size_t k=0;k<count;k++) {
                f=prepare(&dev,initial,op);disk.events=disk.writes=disk.flushes=0;
                disk.persistence=persistence;disk.cut=(long)points[k];disk.after=after!=0;
                int64_t r=operate(op,f);assert(r==-VFS_EIO || op==LAST_CLOSE);
                assert(e4_active->engine->tainted);
                size_t stop=disk.events;
                assert(ext4_sync(e4_active)==-VFS_EIO && ext4_freeze_and_sync(e4_active)==-VFS_EIO && disk.events==stop);
                if (f && op!=LAST_CLOSE) assert(!vfs_close(f));
                teardown();ext4_fault_restart(&disk);vfs_init();ext4_mount_t *fs=NULL;
                assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
                recovered_oracle(op,bs);cuts++;
            }
        printf("integration %s PASS events=%zu boundary-cuts=%zu\n",names[op],events,count*4);fflush(stdout);
    }
    size_t stages=argc==5 && (strstr(argv[4],"staging")!=NULL) ? staging_failures(&dev,initial,
        !strcmp(argv[4],"--truncate-staging")) : 0;
    shared_offsets(&dev,initial,argv[3],false);shared_offsets(&dev,initial,argv[3],true);
    /* Defensive legacy entry points remain inaccessible with a writer. */
    (void)prepare(&dev,initial,SYNC);size_t stop=disk.events;
    assert(ext4_engine_finish(e4_active->engine)==-VFS_EOPNOTSUPP);
    uint8_t byte=0;assert(!e4_write_bytes(e4_active->engine,1024,&byte,1) && disk.events==stop);
    assert(metadata_writes && data_writes && journal_writes);
    teardown();free(initial);free(disk.stable);free(disk.volatile_bytes);
    printf("EXT4 integration PASS block=%u sector=%u cuts=%zu metadata=%zu data=%zu journal=%zu stages=%zu\n",
        bs,ss,cuts,metadata_writes,data_writes,journal_writes,stages);return 0;
}
#endif
