/* Actual EXT4/EXT2 lock adapters plus devfs. Host exclusion, not IRQ proof. */
#define TEST_PERM_DEVICE_IO 1
#define main baseline_mount_main
#include "ext4_mount_host.c"
#undef main
#include "../src/fs/ext2.c"
#include <errno.h>
static spinlock_t *expected_device_lock;
static unsigned device_observations;
void permissions_device_lock_observed(void) {
 if(!expected_device_lock)return;
 assert(ext4_host_lock_depth==1);
 /* In this single-caller probe the selected mutex is otherwise free. A
  * wrong/excluded lock therefore fails this deterministic ownership check. */
 assert(pthread_mutex_trylock(&expected_device_lock->mutex)==EBUSY);
 device_observations++;
}
int main(int argc,char **argv) {
 assert(argc==2);size_t n;uint8_t *initial=load(argv[1],&n);
 ext4_fault_disk_t d={.stable=malloc(n),.volatile_bytes=malloc(n),.bytes=n,.cut=-1};assert(d.stable && d.volatile_bytes);
 block_dev_t dev={.sector_size=512,.sector_count=n/512,.read_sector=ext4_fault_read,.write_sector=ext4_fault_write,.flush=ext4_fault_flush,.priv=&d};
 reset(&d,initial);vfs_node_t raw={.size=n,.fs_private=&dev};uint8_t data[3];
 expected_device_lock=&ext2_lock;assert(read_partition(&raw,511,data,3)==3);
 assert(device_observations==2 && !memcmp(data,initial+511,3));expected_device_lock=NULL;
 ext4_mount_t *fs;assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
 expected_device_lock=&e4_lock;device_observations=0;
 assert(read_partition(&raw,511,data,3)==3 && device_observations==2 && !memcmp(data,d.volatile_bytes+511,3));
 /* Handled I/O failure must not fall back/retry through the other lock. */
 fail_read=0;device_observations=0;assert(read_partition(&raw,0,data,3)==-VFS_EIO && device_observations==1);
 fail_read=-1;expected_device_lock=NULL;assert(!ext4_freeze_and_sync(fs));teardown();
 free(initial);free(d.stable);free(d.volatile_bytes);
 puts("PASS devfs actual EXT4/EXT2 exclusion: selected filesystem mutex, bounded read, no error fallback or writes");return 0;
}
