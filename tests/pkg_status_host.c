/* Actual package-status callback + VFS/EXT4/JBD2; host exclusion/disk adapters.
 * The read oracle fails deterministically on access without filesystem exclusion. */
#define TEST_EXT4_ATOMIC_COUNTERS
#define PKG_ORDINARY_INCLUDED
#include "pkg_ordinary_install_host.c"
#include <sched.h>
static _Thread_local bool status_read;
static _Atomic unsigned protected_reads;
static bool checked_read(block_dev_t *dev,uint64_t lba,void *bytes) {
    if(status_read) {
        if(ext4_host_lock_depth!=1) {
            fputs("PKG_STATUS_UNPROTECTED_READ\n",stderr);exit(2);
        }
        protected_reads++;
    }
    return ext4_fault_read(dev,lba,bytes);
}
static void check_status(ext4_mount_t *fs) {
    pkg_transaction_status status={0};status_read=true;
    int r=ext4_pkg_status(fs,&status);status_read=false;
    assert(!r&&status.phase==PKG_TX_IDLE&&status.sequence==1&&
        status.account_sequence==1&&status.credential_generation==1);
}
static void *queries(void *arg) {
    for(unsigned i=0;i<300;i++) {check_status(arg);sched_yield();}
    return NULL;
}
static void *file_activity(void *arg) {
    file_t *f=arg;char bytes[32];memset(bytes,'H',sizeof bytes);
    for(unsigned i=0;i<150;i++) {
        assert(vfs_write(f,bytes,sizeof bytes)==sizeof bytes);sched_yield();
    }
    return NULL;
}
int main(int argc,char **argv) {
    assert(argc==3);size_t n;uint8_t *initial=load(argv[1],&n);
    ext4_fault_disk_t d={.stable=malloc(n),.volatile_bytes=malloc(n),.bytes=n,.cut=-1};
    assert(d.stable&&d.volatile_bytes);memcpy(d.stable,initial,n);memcpy(d.volatile_bytes,initial,n);
    block_dev_t dev={.sector_size=512,.sector_count=n/512,.read_sector=checked_read,
        .write_sector=ext4_fault_write,.flush=ext4_fault_flush,.priv=&d};
    vfs_init();ext4_mount_t *fs=NULL;assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
    check_status(fs); /* Fresh root, database not yet created. */
    status_read=true;assert(!px_ensure_database(fs));status_read=false;check_status(fs);
    file_t *f=vfs_open("/mnt/history",VFS_O_CREAT|VFS_O_RDWR);assert(f);
    pthread_t readers[2],writer;
    for(unsigned i=0;i<2;i++)assert(!pthread_create(&readers[i],NULL,queries,fs));
    assert(!pthread_create(&writer,NULL,file_activity,f));
    for(unsigned i=0;i<2;i++)assert(!pthread_join(readers[i],NULL));
    assert(!pthread_join(writer,NULL)&&!vfs_close(f));
    pkg_transaction_status status;
    fail_read=0;assert(ext4_pkg_status(fs,&status)==-VFS_EIO);fail_read=-1;check_status(fs);
    fail_alloc=0;assert(ext4_pkg_status(fs,&status)==-VFS_ENOMEM);fail_alloc=-1;check_status(fs);
    assert(protected_reads&& !ext4_freeze_and_sync(fs));
    FILE *out=fopen(argv[2],"wb");assert(out&&fwrite(d.stable,1,n,out)==n&&!fclose(out));
    teardown();free(initial);free(d.stable);free(d.volatile_bytes);
    puts("PASS package status: filesystem exclusion, concurrent reads/writes, I/O/OOM cleanup");
    return 0;
}
