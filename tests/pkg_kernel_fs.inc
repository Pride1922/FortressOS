/* Appended only to copied EXT4 TU. RAM device is loaded from immutable TarFS;
 * no hardware block device is registered or attached. QEMU exports a read-only
 * memory snapshot for independent Linux inspection after freeze. */
static ext4_mount_t *kernel_fs;
/* Private fixture backing is loader-mapped BSS: no runtime mapping/shootdown
 * campaign is conflated with grant validation or a stalled caller. */
static uint8_t kernel_fs_storage[32u*1024u*1024u];
static uint8_t *kernel_fs_bytes;
static size_t kernel_fs_size;
static unsigned kernel_fs_writes,kernel_fs_flushes,kernel_fs_ready,kernel_fs_rejected;
static bool kernel_fs_preparing;
static pw_key kernel_fs_owner;
static uint64_t kernel_fs_session;
static uint64_t kernel_fs_cpus;
static uint64_t kernel_fs_tlb_services;
static block_dev_t kernel_fs_device;
static bool kernel_fs_read(block_dev_t *d,uint64_t l,void *b){
    kernel_fs_tlb_services+=smp_tlb_service_local();
    if(l>=d->sector_count)return false;
    memcpy(b,kernel_fs_bytes+l*d->sector_size,d->sector_size);return true;
}
static bool kernel_fs_write(block_dev_t *d,uint64_t l,const void *b){
    kernel_fs_tlb_services+=smp_tlb_service_local();
    if(l>=d->sector_count)return false;
    memcpy(kernel_fs_bytes+l*d->sector_size,b,d->sector_size);kernel_fs_writes++;return true;
}
static bool kernel_fs_flush(block_dev_t *d){(void)d;kernel_fs_tlb_services+=smp_tlb_service_local();kernel_fs_flushes++;return true;}
static void kernel_fs_load(const char *path,void *out,size_t size){
    int error;file_t *f=vfs_open_creds(path,VFS_O_RDONLY,&thread_current()->creds,&error);assert(f&&f->node->size==size);
    for(size_t offset=0;offset<size;){size_t n=size-offset;if(n>32768)n=32768;
        assert(vfs_read(f,(uint8_t *)out+offset,n)==(int64_t)n);offset+=n;}
    assert(!vfs_close(f));
}
static int kernel_fs_preview(ext4_engine_t *engine){
    if(!kernel_fs_preparing)return -VFS_EACCES;
    pg_view view;uint64_t allocated,required[2];ext4_mount_t *fs=engine->fs;
    if(engine->orphan_count||life_validate(engine,&view)||view.phase!=PG_IDLE)return -VFS_EIO;
    for(unsigned i=0;i<3;i++)if(object(fs,ids[i],i?binding.generation[i-1]:marker_generation,
        i==0?512:i==1?1024:8192,false,true,&allocated))return -VFS_EIO;
    if(object(fs,ids[3],binding.generation[2],fs->bs,true,true,&allocated)||capacity_preview(engine,required))return -VFS_EIO;
    uint8_t sb[1024];if(!e4_bytes(fs,1024,sb,sizeof sb))return -VFS_EIO;
    if(memcmp(sb+104,binding.uuid[0],16))return -VFS_EIO;
    e4_inode_t root,pkg;uint32_t named=0;
    if(e4_inode(fs,2,&root)||e4_scan(&root,"pkg",0,NULL,&named)<0||!named||e4_inode(fs,named,&pkg)||
       pkg.full_mode!=(0x4000|0700)||pkg.uid||pkg.gid)return -VFS_EIO;
    const char *names[]={"enrollment.v1","grant.v1","ownership.v1","transaction.v1"};
    for(unsigned i=0;i<4;i++)if(e4_scan(&pkg,names[i],0,NULL,&named)<0||named!=ids[i])return -VFS_EIO;
    uint64_t free[2]={e4_u32(sb+12),e4_u32(sb+16)};
    return capacity_preview_budget(fs,free,view.remaining,required);
}
static int e4_semantic_commit(ext4_engine_t *e){
    const pm_view *view=pc_view(thread_current());
    if(!kernel_fs||e!=kernel_fs->engine||!view||view->kind!=PM_PACKAGE||view->mount!=1)return -VFS_EACCES;
    if(!pc_commit_begin(thread_current()))return -VFS_EINTR;
    int r=kernel_fs_tagged(e);pc_commit_end(thread_current());return r;
}
static void kernel_fs_bind(void){
    tcb_t *t=thread_current();pm_key key;assert(!pm_package(1,kernel_fs_owner,kernel_fs_session,&key));
    assert(pc_start(t,key,1,kernel_fs)&&pc_pin(t));t->package_call_seen=true;
    __atomic_fetch_or(&kernel_fs_cpus,1ULL<<cpu_current()->id,__ATOMIC_RELAXED);
}
static int64_t kernel_fs_append(void){
    uint8_t *bytes=kmalloc(kernel_fs->bs);assert(bytes);memset(bytes,'K',kernel_fs->bs);
    uint64_t irq=spin_lock_irqsave(&e4_lock),offset=0;
    unsigned before=kernel_fs_writes;bool cancelled=pc_cancelled(thread_current());
    int64_t r=e4_file_write_actor(kernel_fs->engine,life_target,&offset,true,bytes,kernel_fs->bs,&thread_current()->creds);
    if(cancelled)assert(r==-VFS_EINTR&&kernel_fs_writes==before);
    spin_unlock_irqrestore(&e4_lock,irq);kfree(bytes);return r;
}
int64_t kernel_fs_test(uint64_t op){
    spin_debug_assert_unheld();tcb_t *t=thread_current();
    if(op==20){
        serial_puts("\nKERNEL FS PHASE load\n");
        assert(!kernel_fs);kernel_fs_size=sizeof kernel_fs_storage;kernel_fs_bytes=kernel_fs_storage;
        kernel_fs_load("/pkg-kernel.fixture",kernel_fs_bytes,kernel_fs_size);
        serial_puts("KERNEL FS PHASE context\n");
        /* Context is trusted fixture input, never an on-disk authorization. */
        size_t n=sizeof(binding)+sizeof(ids)+sizeof(marker_generation);uint8_t *ctx=kmalloc(n);assert(ctx);
        kernel_fs_load("/pkg-kernel.context",ctx,n);memcpy(&binding,ctx,sizeof binding);
        memcpy(ids,ctx+sizeof binding,sizeof ids);memcpy(&marker_generation,ctx+sizeof binding+sizeof ids,sizeof marker_generation);kfree(ctx);marker_ino=ids[0];
        kernel_fs_device=(block_dev_t){.sector_size=512,.sector_count=kernel_fs_size/512,.read_sector=kernel_fs_read,.write_sector=kernel_fs_write,.flush=kernel_fs_flush};
        const char *stage;kernel_fs_preparing=true;
        serial_puts("KERNEL FS PHASE constructor\n");
        int r=e4_mount_journal_admitted(&kernel_fs_device,"/",true,&kernel_fs,&stage,true);kernel_fs_preparing=false;
        if(r){serial_raw_puts("KERNEL FS MOUNT ");serial_raw_puts(stage);}assert(!r);
        serial_puts("KERNEL FS PHASE namespace\n");
        ext4_account_namespace_report_t ns;assert(!ext4_root_account_namespace_admit(kernel_fs,&ns));
        serial_puts("KERNEL FS PHASE capacity\n");
        uint64_t irq=spin_lock_irqsave(&e4_lock);e4_ns_workspace_t *w=kcalloc(1,sizeof(*w));assert(w);
        memcpy(w->anchors,kernel_fs->account_namespace_anchors,sizeof w->anchors);assert(!capacity_certificate(kernel_fs,w,compete_floor));kfree(w);
        pg_view view;assert(!kernel_fs_locked_validate(kernel_fs->engine,&view)&&view.phase==PG_IDLE);spin_unlock_irqrestore(&e4_lock,irq);
        assert(pm_configure(1)&&pw_register(t->tid,&kernel_fs_owner)&&pw_begin(kernel_fs_owner,true,&kernel_fs_session));
        serial_puts("KERNEL FS PHASE reserve\n");
        assert(!compete_admit(kernel_fs,view.sequence,'J'));kernel_fs_bind();assert(!life_create(kernel_fs));assert(pc_end(t));
        serial_puts("KERNEL FS PHASE writers\n");return 0;
    }
    assert(kernel_fs);
    if(op==21||op==25){
        kernel_fs_bind();assert(kernel_fs_append()==(int64_t)kernel_fs->bs);
        __atomic_add_fetch(&kernel_fs_ready,1,__ATOMIC_RELEASE);
        while(!process_signal_interrupt())thread_yield();
        assert(pc_cancel_current(t));
        assert(kernel_fs_append()==-VFS_EINTR);
        __atomic_add_fetch(&kernel_fs_rejected,1,__ATOMIC_RELEASE);
        if(op==25)assert(pc_end(t));
        return SYSCALL_EINTR;
    }
    if(op==22)return __atomic_load_n(&kernel_fs_ready,__ATOMIC_ACQUIRE);
    if(op==23){
        assert(pc_cancelled(t)&&pc_end(t));return 0;
    }
    if(op==24){
        assert(!pc_count()&&pw_references(kernel_fs_owner)==2&&kernel_fs_ready==8&&kernel_fs_rejected==8);
        assert(pw_close(kernel_fs_owner,kernel_fs_session)&&pw_detach(kernel_fs_owner)&&!pw_references(kernel_fs_owner));
        uint64_t irq=spin_lock_irqsave(&e4_lock);pg_view view;
        assert(!kernel_fs_locked_validate(kernel_fs->engine,&view)&&view.charged[0]==8&&view.charged[1]==1&&!view.remaining[0]&&!view.remaining[1]);
        spin_unlock_irqrestore(&e4_lock,irq);
        pw_key next;uint64_t session;assert(pw_register(t->tid,&next));
        assert(!pw_begin(next,view.phase==PG_IDLE,&session)&&pw_detach(next));
        assert(!ext4_freeze_and_sync(kernel_fs));
        /* Frozen backing, drained callers: audit a fresh, naturally unpublished
         * engine under the retained exclusive device reservation. Never change
         * the mounted engine's ownership flags to evade its lock contract. */
        unsigned before=kernel_fs_writes,flushes=kernel_fs_flushes;ext4_engine_t *audit=NULL;
        assert(!e4_open_journal_orphans_owned(&kernel_fs_device,true,&audit,true,true));
        assert(!life_validate(audit,&view));e4_engine_dispose(audit);
        assert(before==kernel_fs_writes&&flushes==kernel_fs_flushes);
        serial_puts("\nKERNEL FS SNAPSHOT address=");serial_print_hex((uintptr_t)kernel_fs_bytes);serial_puts(" bytes=");serial_print_dec(kernel_fs_size);
        serial_puts(" commits=");serial_print_dec(life_commits);serial_puts(" writes=");serial_print_dec(kernel_fs_writes);serial_puts(" flushes=");serial_print_dec(kernel_fs_flushes);
        serial_puts(" cpus=");serial_print_dec(kernel_fs_cpus);serial_puts(" tlb=");serial_print_dec(kernel_fs_tlb_services);serial_puts("\n");return 0;
    }
    return SYSCALL_EINVAL;
}
