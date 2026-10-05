/* Focused bootstrap negative controls; no production admission or disk repair. */
#define main phase85_bootstrap_main
#include "ext4_mount_host.c"
#undef main
int main(int argc,char **argv) {
    assert(argc==5);size_t n;uint8_t *initial=load(argv[1],&n);unsigned ss=(unsigned)strtoul(argv[2],NULL,10);
    bool recover=!strcmp(argv[3],"recover");assert(recover || !strcmp(argv[3],"reject"));
    ext4_fault_disk_t disk={.stable=malloc(n),.volatile_bytes=malloc(n),.bytes=n,.cut=-1};
    assert(disk.stable && disk.volatile_bytes);
    block_dev_t dev={.sector_size=ss,.sector_count=n/ss,.read_sector=ext4_fault_read,.write_sector=ext4_fault_write,
        .flush=ext4_fault_flush,.priv=&disk};
    reset(&disk,initial);ext4_mount_t *fs=NULL;
    assert(ext4_mount_rw(&dev,"/mnt",&fs)<0 && !fs && !disk.writes && !disk.flushes);
    teardown();reset(&disk,initial);int r=ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs);
    if (recover) {
        assert(!r && fs && vfs_lookup("/mnt/new.bin"));assert(!ext4_freeze_and_sync(fs));clean_check(fs);
        save(argv[4],"recovered",disk.stable,n);
    } else assert(r==-VFS_EIO && !fs && !e4_active && !disk.writes && !disk.flushes && !memcmp(disk.stable,initial,n));
    teardown();free(initial);free(disk.stable);free(disk.volatile_bytes);puts("bootstrap guard PASS");return 0;
}
