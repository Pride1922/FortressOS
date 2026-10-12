/* Actual deployment + detached EXT4/account engines; regular-file transport.
 * No driver, scheduler, IRQ, DMA or volatile-cache model is claimed. */
#define TEST_PERMISSIONS_ENFORCEMENT
#define gpt_get_partition_count install_host_partition_count
#define gpt_get_partition install_host_partition
#define main phase0_main
#include "ext4_mount_host.c"
#undef main
#undef gpt_get_partition_count
#undef gpt_get_partition
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "../src/kernel/creds.c"
#include "../src/fs/permission_values.c"
#include "../src/kernel/accounts.c"
#include "install_runtime.h"
#include "install_payload.h"
/* Bulk growth itself is covered by the actual heap sanitizer gate. This
 * transport harness retains the same malloc/OOM/live-allocation adapter. */
void *kmalloc_large(size_t n) {return kmalloc(n);}
uint64_t *vmm_get_active_pml4_virt(void) {return (void *)1;}
bool vmm_validate_user_range(uint64_t *p,uintptr_t a,size_t n,bool output) {(void)p;(void)a;(void)n;(void)output;return false;}
int64_t syscall_from_vfs_error(int64_t e) {return e;}
static int target_fd;
static unsigned target_reads,target_writes,target_flushes,target_identities;
static install_identity_v1_t target_identity;
static int identity(void *ctx,install_identity_v1_t *out) {(void)ctx;target_identities++;*out=target_identity;return 0;}
static int disk_read(void *ctx,uint64_t l,uint32_t n,void *out) {(void)ctx;target_reads++;return pread(target_fd,out,n*512,l*512)==n*512 ? 0 : -VFS_EIO;}
static int disk_write(void *ctx,uint64_t l,uint32_t n,const void *out) {(void)ctx;target_writes++;return pwrite(target_fd,out,n*512,l*512)==n*512 ? 0 : -VFS_EIO;}
static int disk_flush(void *ctx) {(void)ctx;target_flushes++;return fsync(target_fd) ? -VFS_EIO : 0;}
static void *load_module(const char *dir,const char *name,uint64_t *size) {
    char path[1024];snprintf(path,sizeof(path),"%s/%s",dir,name);
    int fd=open(path,O_RDONLY|O_NOFOLLOW);struct stat st;assert(fd>=0 && !fstat(fd,&st) && S_ISREG(st.st_mode));
    void *p=malloc(st.st_size);assert(p && read(fd,p,st.st_size)==st.st_size);close(fd);*size=st.st_size;return p;
}
int main(int argc,char **argv) {
    assert(argc==4 || argc==5);boot_info_t boot={.installer_source_valid=true,.installer_source_optical=true,.installer_payload_valid=true};
    const char *names[]={"esp.img","root.img","bios.img","mbr.bin","payload.v1"};
    for (unsigned i=0;i<5;i++) boot.installer_payload_vaddr[i]=(uintptr_t)load_module(argv[1],names[i],&boot.installer_payload_size[i]);
    boot.initramfs_vaddr=(uintptr_t)load_module(argv[1],"bootstrap-source.tar",&boot.initramfs_size);
    boot.kernel_file_vaddr=(uintptr_t)load_module(argv[3],"fortress.elf",&boot.kernel_file_size);
    target_fd=open(argv[2],O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW,0600);assert(target_fd>=0);
    assert(!ftruncate(target_fd,384*1024*1024));
    target_identity=(install_identity_v1_t){.size=192,.version=1,.generation=1,.sector_count=384*2048,.sector_size=512,.nsid=1,.flags=INSTALL_ID_ELIGIBLE};
    install_backend_t backend={.identity=identity,.read=disk_read,.write=disk_write,.flush=disk_flush};
    install_deploy_v1_t request={.size=256,.version=1,.fd=-1,.flags=INSTALL_DRY_RUN};install_status_v1_t status={.size=32,.version=1};
    vfs_init();vfs_node_t *root=g_vfs_root;vfs_boot_seal_namespace();size_t baseline_live=live;
    assert(!install_runtime_deploy(&boot,&backend,&target_identity,&request,&status));
    assert(!target_reads && !target_writes && !target_flushes && !target_identities && !status.complete && live==baseline_live);
    request.flags=0;request.fd=3;
    memcpy(request.operator_hash,"$5$rounds=100000$abcdefghijklmnop$JAlrLh9YVBYa4XSl2fqcB6N55eikn17ite1nvkjVHI0",78);
    memcpy(request.recovery_hash,"$5$rounds=100000$0123456789abcdef$...........................................",78);
    for (unsigned i=0;i<4;i++) {
        uint8_t *g=i ? request.partition_guid[i-1] : request.disk_guid;
        memset(g,i+1,16);g[7]=0x40;g[8]=0x80;
    }
    memset(request.install_id,0x17,16);
    install_deploy_v2_t named={.base=request};
    if (argc==5) {named.base.size=sizeof(named);named.base.version=2;snprintf(named.username,sizeof(named.username),"%s",argv[4]);}
    int r=install_runtime_deploy(&boot,&backend,&target_identity,argc==5 ? &named.base : &request,&status);
    if (r) fprintf(stderr,"runtime error %d stage %u reads %u writes %u flushes %u\n",r,status.stage,target_reads,target_writes,target_flushes);
    assert(!r && status.complete && status.stage==INSTALL_STAGE_COMPLETE);
    assert(g_vfs_root==root && !enabled && !storage.context && !ext4_storage_owned() && live==baseline_live);
    close(target_fd);
    for (unsigned i=0;i<5;i++) free((void *)boot.installer_payload_vaddr[i]);
    free((void *)boot.initramfs_vaddr);free((void *)boot.kernel_file_vaddr);
    printf("PASS actual runtime deployment, exclusive detached provisioning, readiness last, namespace/service isolation and reclamation\n");
}
