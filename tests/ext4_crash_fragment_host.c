/* Build a disposable, independently audited fragmented mounted fixture. */
#define EXT4_INTEGRATION_LIBRARY
#include "ext4_integration_host.c"

int main(int argc,char **argv) {
    (void)prepare;(void)operate;(void)shared_offsets;(void)staging_failures;
    (void)recovered_oracle;(void)audited_write;(void)audited_flush;
    assert(argc==3);size_t n;uint8_t *initial=load(argv[1],&n);
    ext4_fault_disk_t disk={.stable=malloc(n),.volatile_bytes=malloc(n),.bytes=n,.cut=-1};
    assert(disk.stable && disk.volatile_bytes);
    block_dev_t dev={.sector_size=512,.sector_count=n/512,.read_sector=ext4_fault_read,
        .write_sector=audited_write,.flush=audited_flush,.priv=&disk};
    reset(&disk,initial);ext4_mount_t *fs=NULL;
    assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
    file_t *target=vfs_open("/mnt/target.bin",VFS_O_RDWR);assert(target);
    assert(!vfs_truncate(target->node,0));
    uint8_t bytes[4096];memset(bytes,'O',sizeof(bytes));
    for (unsigned k=0;k<3;k++) {
        assert(vfs_write(target,bytes,fs->bs)==fs->bs);
        char path[80];snprintf(path,sizeof(path),"/mnt/blocker-%u",k);
        file_t *blocker=vfs_open(path,VFS_O_CREAT|VFS_O_RDWR);assert(blocker);
        assert(vfs_write(blocker,bytes,fs->bs)==fs->bs);assert(!vfs_close(blocker));
    }
    assert(!vfs_close(target));
    for (unsigned k=0;k<3;k++) {
        char path[80];snprintf(path,sizeof(path),"/mnt/blocker-%u",k);assert(!vfs_unlink(path));
    }
    assert(!ext4_freeze_and_sync(fs));clean_check(fs);
    FILE *out=fopen(argv[2],"wbx");assert(out && fwrite(disk.stable,1,n,out)==n);assert(!fclose(out));
    teardown();free(initial);free(disk.stable);free(disk.volatile_bytes);
    puts("Fragmented fixture PASS");return 0;
}
