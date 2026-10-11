/* Phase 0 actual EXT4/VFS profile admission on a disposable regular-file copy.
 * Reuse existing mounted-code host adapters; no IRQ/device/root-publication claim.
 */
#define main existing_ext4_mount_harness_main
#include "ext4_mount_host.c"
#undef main
#include <unistd.h>

static bool image_read(block_dev_t *dev,uint64_t lba,void *bytes) {
    FILE *stream=dev->priv;
    return fseeko(stream,(off_t)(lba*dev->sector_size),SEEK_SET)==0 &&
        fread(bytes,1,dev->sector_size,stream)==dev->sector_size;
}
static bool image_write(block_dev_t *dev,uint64_t lba,const void *bytes) {
    FILE *stream=dev->priv;
    return fseeko(stream,(off_t)(lba*dev->sector_size),SEEK_SET)==0 &&
        fwrite(bytes,1,dev->sector_size,stream)==dev->sector_size;
}
static bool image_flush(block_dev_t *dev) {
    FILE *stream=dev->priv;
    return fflush(stream)==0 && fsync(fileno(stream))==0;
}
int main(int argc,char **argv) {
    assert(argc==3);
    unsigned sector=(unsigned)strtoul(argv[2],NULL,10);assert(sector==512 || sector==4096);
    FILE *stream=fopen(argv[1],"r+b");assert(stream);
    assert(fseeko(stream,0,SEEK_END)==0);off_t bytes=ftello(stream);
    assert(bytes==256*1024*1024);
    block_dev_t dev={.sector_size=sector,.sector_count=(uint64_t)bytes/sector,
        .read_sector=image_read,.write_sector=image_write,.flush=image_flush,.priv=stream};
    fail_read=fail_alloc=-1;vfs_init();ext4_mount_t *fs=NULL;
    assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
    const char *paths[]={"/mnt/bin/login","/mnt/bin/sudo","/mnt/etc/shadow","/mnt/home/operator"};
    for (size_t i=0;i<4;i++) {
        int error=0;vfs_node_t *node=vfs_lookup_ref(paths[i],&error);assert(node && !error);
        vfs_metadata_t value;assert(!vfs_metadata(node,&value));
        if (i==2) assert(value.uid==0 && value.gid==0 && (value.mode&07777)==0600);
        if (i==3) assert(value.uid==1000 && value.gid==1000 && (value.mode&07777)==0700);
        vfs_node_put(node);
    }
    file_t *marker=vfs_open("/mnt/etc/fortress-install.fixture",VFS_O_RDONLY);assert(marker);
    char data[128]={0};int64_t n=vfs_read(marker,data,sizeof(data));
    const char expected[]="Phase 0 installed-root fixture; root boot not implemented.\n";
    assert(n==(int64_t)strlen(expected) && !memcmp(data,expected,(size_t)n));
    assert(!vfs_close(marker));
    int error=0;vfs_node_t *created=vfs_create_ref("/mnt/phase0-probe",VFS_FILE,&error);
    assert(created && !error);vfs_node_put(created);
    file_t *probe=vfs_open("/mnt/phase0-probe",VFS_O_WRONLY);assert(probe);
    const char payload[]="phase0-actual-ext4\n";
    assert(vfs_write(probe,payload,sizeof(payload)-1)==(int64_t)(sizeof(payload)-1));
    assert(!vfs_close(probe));
    assert(!ext4_sync(fs));assert(!ext4_freeze_and_sync(fs));
    teardown();assert(fclose(stream)==0);
    puts("PASS actual EXT4/VFS 256 MiB profile: admission, bytes, metadata, mutation, sync/freeze, cleanup");
    return 0;
}
