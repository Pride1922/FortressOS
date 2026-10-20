/* Actual fixture syscall helper with mocked fd/pointer/credential admission. */
#define FORTRESS_EXT4_STAGE_TEST
#define TEST_PERMISSIONS_ENFORCEMENT
#define TEST_EXT4_ATOMIC_COUNTERS
#define main previous_mount_main
#include "ext4_mount_host.c"
#undef main
static tcb_t caller={.tid=42};static file_t *selected;static creds_t root;
static uint8_t input[EXT4_STAGE_TEST_MAX];static bool actor_valid=true;
static tcb_t *stage_current(void){return &caller;}
static file_t *fd_get(tcb_t *self,int fd){assert(self==&caller);return fd==3?selected:NULL;}
static uint64_t *vmm_get_active_pml4_virt(void){return NULL;}
static bool vmm_validate_user_range(uint64_t *unused,uintptr_t p,size_t n,bool write){
    (void)unused;assert(!write);return p==(uintptr_t)input&&n<=sizeof(input);
}
static bool syscall_actor(creds_t *out){*out=root;return actor_valid;}
static int64_t syscall_from_vfs_error(int64_t n){
    switch(n){case 0:return 0;case -VFS_EAGAIN:return SYSCALL_EAGAIN;case -VFS_ENOMEM:return SYSCALL_ENOMEM;
    case -VFS_EOPNOTSUPP:return SYSCALL_EOPNOTSUPP;case -VFS_EIO:return SYSCALL_EIO;default:assert(0);return 0;}
}
#define thread_current stage_current
#include "../src/kernel/ext4_stage_test.inc"
#undef thread_current
int main(int argc,char **argv){
    assert(argc==2);size_t n;uint8_t *initial=load(argv[1],&n);creds_init_root(&root);memset(input,'C',sizeof(input));
    ext4_fault_disk_t d={.stable=malloc(n),.volatile_bytes=malloc(n),.bytes=n,.cut=-1};assert(d.stable&&d.volatile_bytes);
    block_dev_t dev={.sector_size=512,.sector_count=n/512,.read_sector=ext4_fault_read,.write_sector=ext4_fault_write,.flush=ext4_fault_flush,.priv=&d};
    ext4_mount_t *fs;reset(&d,initial);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
    selected=vfs_open("/mnt/sys-stage",VFS_O_CREAT|VFS_O_RDWR);assert(selected);int refs=selected->ref_count;size_t blocks=live,events=d.events;
    assert(sys_stage_test_write(99,0,0,0,0)==SYSCALL_EBADF);
    assert(sys_stage_test_write(3,0,0,0,0)==SYSCALL_EINVAL);
    assert(sys_stage_test_write(3,0,1,0,0)==SYSCALL_EFAULT);
    for(unsigned i=0;i<4;i++)stage_test_owners[i]=100+i;
    assert(sys_stage_test_write(3,(uintptr_t)input,1,0,0)==SYSCALL_EAGAIN);
    for(unsigned i=0;i<4;i++)assert(stage_test_owners[i]==100+i);
    memset(stage_test_owners,0,sizeof(stage_test_owners));stage_test_owners[2]=caller.tid;
    assert(sys_stage_test_write(3,(uintptr_t)input,1,0,0)==SYSCALL_EAGAIN&&stage_test_owners[2]==caller.tid);
    stage_test_owners[2]=0;fail_alloc=0;
    assert(sys_stage_test_write(3,(uintptr_t)input,1,0,0)==SYSCALL_ENOMEM);fail_alloc=-1;
    actor_valid=false;assert(sys_stage_test_write(3,(uintptr_t)input,1,0,0)==SYSCALL_ESRCH);actor_valid=true;
    assert(live==blocks&&selected->ref_count==refs&&d.events==events);
    for(unsigned i=0;i<4;i++)assert(!stage_test_owners[i]);
    assert(sys_stage_test_write(3,(uintptr_t)input,sizeof(input),0,0)==sizeof(input));
    assert(selected->offset==sizeof(input)&&selected->ref_count==refs);
    input[0]='A';assert(sys_stage_test_write(3,(uintptr_t)input,1,sizeof(input),0)==1);
    memset(input,'D',sizeof(input));
    assert(sys_stage_test_write(3,(uintptr_t)input,sizeof(input),sizeof(input)+1,0)==sizeof(input)-1);
    assert(selected->offset==2*sizeof(input));
    uint64_t off=selected->offset;assert(!sys_stage_test_sync(3)&&selected->offset==off);
    for(unsigned i=0;i<4;i++)assert(!stage_test_owners[i]);
    selected->offset=0;
    for(size_t at=0;at<2*sizeof(input);at+=4096){assert(vfs_read(selected,input,4096)==4096);
        for(unsigned i=0;i<4096;i++){size_t position=at+i;assert(input[i]==(position<sizeof(input)?'C':position==sizeof(input)?'A':'D'));}}
    assert(!vfs_read(selected,input,1));assert(!vfs_close(selected));selected=NULL;
    assert(sys_stage_test_sync(3)==SYSCALL_EBADF);assert(!ext4_freeze_and_sync(fs));teardown();
    free(initial);free(d.stable);free(d.volatile_bytes);puts("PASS actual syscall helper: bounded credits, references, OOM/actor unwind, positive counts and selected sync");return 0;
}
