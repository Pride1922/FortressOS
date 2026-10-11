/* Actual detached EXT4/JBD2 and account durability engine, host disk adapter. */
#define TEST_PERMISSIONS_ENFORCEMENT
#define main phase0_main
#include "ext4_mount_host.c"
#undef main
#include <unistd.h>
#include "../src/kernel/creds.c"
#include "../src/fs/permission_values.c"
#include "../src/kernel/accounts.c"
uint64_t *vmm_get_active_pml4_virt(void) {return (void *)1;}
bool vmm_validate_user_range(uint64_t *p,uintptr_t a,size_t n,bool output) {(void)p;(void)a;(void)n;(void)output;return false;}
int64_t syscall_from_vfs_error(int64_t e) {return e;}
static unsigned writes;
static bool read_image(block_dev_t *d,uint64_t l,void *p) {
    return !fseeko(d->priv,l*d->sector_size,SEEK_SET) && fread(p,1,d->sector_size,d->priv)==d->sector_size;
}
static bool write_image(block_dev_t *d,uint64_t l,const void *p) {
    writes++;return !fseeko(d->priv,l*d->sector_size,SEEK_SET) && fwrite(p,1,d->sector_size,d->priv)==d->sector_size;
}
static bool flush_image(block_dev_t *d) {return !fflush(d->priv) && !fsync(fileno(d->priv));}
int main(int argc,char **argv) {
    assert(argc==3);unsigned sector=strtoul(argv[2],NULL,10);FILE *f=fopen(argv[1],"r+b");assert(f);
    block_dev_t d={.priv=f,.sector_size=sector,.sector_count=256*1024*1024/sector,
        .read_sector=read_image,.write_sector=write_image,.flush=flush_image};
    vfs_init();vfs_node_t *running=g_vfs_root;vfs_boot_seal_namespace();
    file_t *normal=vfs_open_terminal(VFS_O_RDONLY);assert(normal && !normal->exclusive_owner);assert(!vfs_close(normal));
    assert(vfs_create_node("/ordinary",VFS_FILE,1,"x"));
    normal=vfs_open("/ordinary",VFS_O_RDONLY);assert(normal && !normal->exclusive_owner);assert(!vfs_close(normal));
    size_t baseline_live=live;
    ext4_mount_t *fs=NULL;
    assert(ext4_prepare_installer_root(&d,false,&fs,NULL)==-VFS_EACCES && !fs && !writes);
    fail_alloc=0;assert(ext4_prepare_installer_root(&d,true,&fs,NULL)==-VFS_ENOMEM && !fs && !writes);fail_alloc=-1;
    assert(!ext4_prepare_installer_root(&d,true,&fs,NULL));assert(fs && g_vfs_root==running);
    assert(ext4_publish_root(fs)==-VFS_EINVAL && g_vfs_root==running);
    const char passwd[]="root:x:0:0::/root:/bin/shell\noperator:x:1000:1000::/home/operator:/bin/shell\n";
    const char group[]="root:x:0:\noperator:x:1000:\nwheel:x:10:operator\n";
    const char shadow[]="root:!:::::::\noperator:$5$rounds=100000$abcdefghijklmnop$JAlrLh9YVBYa4XSl2fqcB6N55eikn17ite1nvkjVHI0:::::::\n";
    const char *bytes[3]={passwd,group,shadow};size_t lengths[3]={sizeof(passwd)-1,sizeof(group)-1,sizeof(shadow)-1};
    unsigned before=writes;const char *bad[3]={"broken",group,shadow};size_t bad_lengths[3]={6,lengths[1],lengths[2]};
    assert(accounts_provision_detached(fs,bad,bad_lengths)==-VFS_EINVAL && writes==before && !enabled && !storage.context);
    int provision_result=accounts_provision_detached(fs,bytes,lengths);
    if (provision_result) fprintf(stderr,"provision failed %d coherence=%d\n",provision_result,coherent_detached(fs,bytes,lengths));
    assert(!provision_result);assert(!enabled && !storage.context && g_vfs_root==running);
    account_io_t io;assert(!ext4_root_account_io(fs,&io));char actual[8192];size_t n;
    for (unsigned i=0;i<3;i++) {assert(!io.read(fs,i,actual,sizeof(actual),&n));assert(n==lengths[i] && !memcmp(actual,bytes[i],n));}
    assert(!ext4_freeze_and_sync(fs));assert(!ext4_discard_prepared_root(fs));
    assert(live==baseline_live && g_vfs_root==running && vfs_boot_namespace_ready());
    assert(ext4_prepare_root(&d,true,&fs)==-VFS_EPERM && !fs);fclose(f);
    printf("PASS installer detached provision sector=%u, sealed namespace preserved, no service publication, exact redo bytes, freeze/reclamation\n",sector);
}
