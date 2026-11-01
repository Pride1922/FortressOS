/* Host image producer: actual package backend, EXT4 and JBD2 on a regular file.
 * Never compiled into the kernel or installer. Accounts/trust are preserved. */
#define PKG_ORDINARY_INCLUDED
#include "pkg_ordinary_install_host.c"

int main(int argc,char **argv) {
    assert(argc==5);
    size_t n,an,cn;
    uint8_t *initial=load(argv[1],&n),*artifact=load(argv[2],&an),*catalog=load(argv[3],&cn);
    ext4_fault_disk_t d={.stable=malloc(n),.volatile_bytes=malloc(n),.bytes=n,.cut=-1};
    assert(d.stable&&d.volatile_bytes);
    memcpy(d.stable,initial,n);memcpy(d.volatile_bytes,initial,n);
    block_dev_t dev={.sector_size=512,.sector_count=n/512,.read_sector=ext4_fault_read,
        .write_sector=ext4_fault_write,.flush=ext4_fault_flush,.priv=&d};
    vfs_init();ext4_mount_t *fs=NULL;
    assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
    uint8_t package[32]={0};memcpy(package,"rot13",5);
    pkg_transaction_status status={0};
    int r=ext4_pkg_install_source(fs,package,catalog,cn,artifact,an,&status);
    fprintf(stderr,"template rot13 installation result=%d\n",r);
    assert(!r&&status.phase==PKG_TX_IDLE&&!px);
    file_t *file=vfs_open("/mnt/usr/bin/rot13",VFS_O_RDONLY);assert(file);
    uint8_t elf[4];assert(vfs_read(file,elf,4)==4&&!memcmp(elf,"\177ELF",4));
    assert(!vfs_close(file)&&!ext4_freeze_and_sync(fs));
    FILE *output=fopen(argv[4],"wb");
    assert(output&&fwrite(d.stable,1,n,output)==n&&!fclose(output));
    teardown();free(initial);free(artifact);free(catalog);free(d.stable);free(d.volatile_bytes);
    puts("PASS real-backend rot13 template installation and clean freeze");
    return 0;
}
