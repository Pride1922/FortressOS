/* Private account redo cancellation at completed mutating callback boundaries.
 * A callback's bounded orphan sequence finishes before cancellation is armed.
 * Mid-callback account orphans remain rejected by the constructor certificate. */
static unsigned account_cancel_at,account_cancel_callbacks,account_cancel_writes;
static bool account_cancel_armed;
static int account_cancel_after(int result,unsigned before){
    if(!result&&account_cancel_armed&&writes_seen!=before){
        assert(!mounted->engine->orphan_count);
        if(++account_cancel_callbacks==account_cancel_at){
            assert(pc_cancel_current(thread_current()));account_cancel_writes=writes_seen;
        }
    }return result;
}
static int account_cancel_sync(void *p){
    return pc_cancelled(thread_current())?-VFS_EINTR:backend.sync(p);
}
int main(int argc,char **argv){
    assert(argc==4);setvbuf(stdout,NULL,_IONBF,0);
    if(!strcmp(argv[3],"seed"))return cancel_fixture_main(argc,argv);
    (void)call_fixture_main;
    unsigned sector=(unsigned)strtoul(argv[2],NULL,10);
    image=fopen(argv[1],"r+b");assert(image&&!fseeko(image,0,SEEK_END));size_t size=ftello(image);
    disk_bytes=malloc(size);assert(disk_bytes&&!fseeko(image,0,SEEK_SET)&&fread(disk_bytes,1,size,image)==size);
    block_dev_t dev={.sector_size=sector,.sector_count=size/sector,.read_sector=rd,.write_sector=wr,.flush=flush};
    cancel_context(argv[1],true);integration_pre_required=true;
    bool reject=!strcmp(argv[3],"reject"),recover=!strcmp(argv[3],"recover");
    if(reject)binding.marker_sha256[0]^=1;
    vfs_init();const char *stage;int prepared=ext4_prepare_root_diagnose(&dev,true,&mounted,&stage);
    if(reject){assert(prepared==-VFS_EIO&&!mounted&&!writes_seen&&!integration_flushes&&integration_pre_calls==1);
        assert(!fclose(image));free(disk_bytes);puts("PASS account cancellation rejection zero-write-flush");return 0;}
    if(prepared)fprintf(stderr,"account cancel constructor stage=%s status=%d\n",stage,prepared);
    assert(!prepared&&integration_pre_calls==1&&integration_capacity_calls==1);
    ext4_account_namespace_report_t report;
    assert(!ext4_root_account_io(mounted,&backend)&&!ext4_root_account_namespace_admit(mounted,&report));
    integration_birth(&call_boot_task,90);call_host_current=&call_boot_task;
    assert(pm_configure(mutation_mount));integrated=life_active=true;
    pm_key key;mutation_call call;assert(!pm_account(mutation_mount,&key)&&!mutation_enter(key,&call));
    assert(!accounts_prepare(mounted));pg_integration_relax();mutation_leave(&call);integration_publish();
    if(!recover){
        account_request_t request={.size=64,.version=1};
        assert(!pm_account(mutation_mount,&key)&&!mutation_control(key,&call_boot_task,ACCOUNT_BEGIN,&request));
        request.passwd=(uintptr_t)integration_new[0];request.group=(uintptr_t)integration_new[1];request.shadow=(uintptr_t)integration_new[2];
        request.passwd_bytes=strlen(integration_new[0]);request.group_bytes=strlen(integration_new[1]);request.shadow_bytes=strlen(integration_new[2]);
        const char *equals=NULL;for(const char *p=argv[3];*p;p++)if(*p=='=')equals=p;
        account_cancel_at=equals?(unsigned)strtoul(equals+1,NULL,10):UINT32_MAX;
        account_cancel_armed=true;assert(!pm_account(mutation_mount,&key));
        int64_t result=mutation_control(key,&call_boot_task,ACCOUNT_UPDATE,&request);account_cancel_armed=false;
        if(equals)assert(result==SYSCALL_EINTR&&account_cancel_callbacks==account_cancel_at&&writes_seen==account_cancel_writes&&!enabled);
        else assert(!result);
        unsigned count,pins;bool priority;pm_counts(&count,&pins,&priority);assert(!count&&!pins&&!priority&&!pc_count());
        /* The actual service blocks snapshots while the failed writer remains. */
        unsigned before=writes_seen;account_request_t check={.size=64,.version=1,.generation=generation};
        assert(accounts_control(ACCOUNT_CHECK,(uintptr_t)&check,sizeof check,&call_boot_task.creds,90)==SYSCALL_EAGAIN&&before==writes_seen);
        char bytes[ACCOUNT_FILE_MAX];size_t n=0;int pending=backend.read(mounted,ACCOUNT_PENDING,bytes,sizeof bytes,&n);
        bool forward=!pending;
        if(!forward&&!backend.read(mounted,ACCOUNT_PASSWD,bytes,sizeof bytes,&n))
            forward=n==strlen(integration_new[0])&&!memcmp(bytes,integration_new[0],n);
        printf("PASS account cancellation callbacks=%u interrupted=%u forward=%u orphans=%u\n",account_cancel_callbacks,equals!=NULL,forward,mounted->engine->orphan_count);
        assert(!mounted->engine->orphan_count);
        if(equals){assert(!fclose(image));free(disk_bytes);return 0;}
        request.passwd=request.group=request.shadow=0;request.passwd_bytes=request.group_bytes=request.shadow_bytes=0;
        assert(!pm_account(mutation_mount,&key)&&!mutation_control(key,&call_boot_task,ACCOUNT_FINISH,&request));
    }
    uint64_t irq=spin_lock_irqsave(&e4_lock);pg_view view;
    assert(!life_validate(mounted->engine,&view)&&view.charged[0]==3&&view.charged[1]==1&&view.remaining[0]==5&&!view.remaining[1]&&!mounted->engine->orphan_count);
    spin_unlock_irqrestore(&e4_lock,irq);assert(!ext4_freeze_and_sync(mounted));assert(!fclose(image));free(disk_bytes);
    puts("PASS account cancellation recovery G=5,0 Q=3,1");return 0;
}
