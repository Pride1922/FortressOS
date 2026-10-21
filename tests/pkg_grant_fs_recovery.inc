/* Real JBD2 replay interruption after a committed private grant transaction. */
static const char *recovery_prefix;
static unsigned recovery_op,recovery_persistence,recovery_after;
static size_t recovery_at;
static void replay_boot(block_dev_t *dev,ext4_mount_t **fs){
    teardown();ext4_fault_restart(dev->priv);vfs_init();const char *stage=NULL;
    int r=e4_mount_journal_admitted(dev,"/mnt",true,fs,&stage,false);
    if(r){
        ext4_fault_disk_t *d=dev->priv;
        fprintf(stderr,"RECOVERY FAIL op=%u persistence=%u after=%u cut=%zu stage=%s error=%d\n",
            recovery_op,recovery_persistence,recovery_after,recovery_at,stage?stage:"unknown",r);
        save(recovery_prefix,"failed-replay",d->stable,d->bytes);
    }
    assert(!r);discover();
}
static void exact_outcome(ext4_mount_t *fs,unsigned operation){
    uint64_t lock=spin_lock_irqsave(&e4_lock);pg_view v;assert(!validate(fs,&v));
    assert(v.sequence==(operation?3u:2u));
    assert(v.charged[0]==(operation?0u:1u));
    assert(v.remaining[0]==(operation?8u:7u));
    spin_unlock_irqrestore(&e4_lock,lock);
}
int main(int argc,char **argv){
    assert(argc==4);setvbuf(stdout,NULL,_IONBF,0);recovery_prefix=argv[3];
    size_t size;uint8_t *initial=load(argv[1],&size);unsigned ss=(unsigned)strtoul(argv[2],NULL,10);
    ext4_fault_disk_t d={.stable=malloc(size),.volatile_bytes=malloc(size),.bytes=size,.cut=-1};assert(d.stable&&d.volatile_bytes);
    block_dev_t dev={.sector_size=ss,.sector_count=size/ss,.read_sector=ext4_fault_read,.write_sector=ext4_fault_write,.flush=ext4_fault_flush,.priv=&d};
    ext4_mount_t *fs;reset(&d,initial);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));seed(fs);
    uint8_t *before=malloc(size),*charged=malloc(size),*pending=malloc(size);assert(before&&charged&&pending);
    memcpy(before,d.stable,size);d.events=0;assert(!change(fs,false));size_t charge_events=d.events;memcpy(charged,d.stable,size);
    d.events=0;assert(!change(fs,true));size_t refund_events=d.events;
    unsigned cuts=0;
    for(unsigned op=0;op<2;op++){
        recovery_op=op;
        bool found=false;
        for(size_t at=0;!found && at<(op?refund_events:charge_events);at++){
            reset(&d,op?charged:before);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));discover();
            d.events=0;d.persistence=0;d.after=true;d.cut=(long)at;
            assert(change(fs,op!=0)==-VFS_EIO);
            teardown();ext4_fault_restart(&d);
            jbd2_plan_t *plan=NULL;jbd2_report_t report={0};
            int r=ext4_journal_analyze(&dev,&plan,&report);
            assert(!d.events);
            if(!r && report.transactions){
                assert(report.transactions==1 && report.images);
                memcpy(pending,d.stable,size);found=true;
                printf("COMMITTED op=%u operation-cut=%zu images=%u\n",op,at,report.images);
            }
            jbd2_release(plan);
        }
        assert(found);
        reset(&d,pending);jbd2_plan_t *plan=NULL;jbd2_report_t report;
        assert(!ext4_journal_analyze(&dev,&plan,&report)&&report.transactions==1);
        d.events=0;assert(!jbd2_replay(plan,true));size_t events=d.events;jbd2_release(plan);
        replay_boot(&dev,&fs);exact_outcome(fs,op);assert(!ext4_freeze_and_sync(fs));
        for(unsigned persistence=0;persistence<4;persistence++)for(unsigned after=0;after<2;after++)for(size_t at=0;at<events;at++){
            reset(&d,pending);plan=NULL;assert(!ext4_journal_analyze(&dev,&plan,&report)&&report.transactions==1);
            d.events=0;d.persistence=persistence;d.after=after!=0;d.cut=(long)at;
            recovery_persistence=persistence;recovery_after=after;recovery_at=at;
            assert(jbd2_replay(plan,true)==-VFS_EIO);jbd2_release(plan);
            for(unsigned pass=0;pass<2;pass++){
                replay_boot(&dev,&fs);exact_outcome(fs,op);check(fs);assert(!ext4_freeze_and_sync(fs));
            }cuts++;
        }
        save(argv[3],op?"refund-replay":"charge-replay",d.stable,size);
        printf("REPLAY op=%u events=%zu cuts=%zu\n",op,events,events*8);
    }
    teardown();free(initial);free(before);free(charged);free(pending);free(d.stable);free(d.volatile_bytes);
    printf("PASS actual-jbd2 recovery-cuts=%u two-restarts-per-cut\n",cuts);return 0;
}
