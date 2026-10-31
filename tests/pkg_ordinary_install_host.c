/* Actual ordinary-root callback, VFS, EXT4 and JBD2; host disk/credentials. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
static int refusal(unsigned line,int code) { fprintf(stderr,"refusal line=%u error=%d\n",line,code);return code; }
#define PX_REJECT(code) refusal(__LINE__,code)
#define FORTRESS_THREAD_H /* Use tests/host/thread.h with the host filesystem. */
#define main retained_mount_main
#include "ext4_mount_host.c"
#undef main
void mouse_dev_init(vfs_node_t *dev_dir) { (void)dev_dir; }
long tool_syscall(long n,uintptr_t a,uintptr_t b,uintptr_t c) {
    (void)n;(void)a;(void)b;(void)c;assert(0);return -1;
}

int main(int argc,char **argv) {
    assert(argc==5);
    size_t n,an,cn,tn;
    uint8_t *initial=load(argv[1],&n),*artifact=load(argv[2],&an);
    uint8_t *catalog=load(argv[3],&cn),*trust=load(argv[4],&tn);
    ext4_fault_disk_t d={.stable=malloc(n),.volatile_bytes=malloc(n),.bytes=n,.cut=-1};
    assert(d.stable&&d.volatile_bytes);
    memcpy(d.stable,initial,n);memcpy(d.volatile_bytes,initial,n);
    block_dev_t dev={.sector_size=512,.sector_count=n/512,.read_sector=ext4_fault_read,
        .write_sector=ext4_fault_write,.flush=ext4_fault_flush,.priv=&d};
    vfs_init();ext4_mount_t *fs=NULL;
    assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
    assert(!px_ensure_dir(fs,"/etc/pkg",0755));
    file_t *f=vfs_open("/mnt/etc/pkg/trust.conf",VFS_O_CREAT|VFS_O_WRONLY);
    assert(f&&vfs_write(f,trust,tn)==(int64_t)tn&&!vfs_close(f));
    uint8_t package[32]={0};memcpy(package,"toolbox",7);
    pkg_transaction_status s={0};
    assert(!px);
    int r=ext4_pkg_install_source(fs,package,catalog,cn,artifact,an,&s);
    fprintf(stderr,"ordinary install result=%d\n",r);
    assert(!r&&s.phase==PKG_TX_IDLE&&!px);
    f=vfs_open("/mnt/usr/bin/hello",VFS_O_RDONLY);assert(f);
    uint8_t elf[4];assert(vfs_read(f,elf,4)==4&&!memcmp(elf,"\177ELF",4));assert(!vfs_close(f));
    size_t events=d.events;
    r=ext4_pkg_install_source(fs,package,catalog,cn,artifact,an,&s);
    fprintf(stderr,"ordinary reinstall result=%d\n",r);assert(!r);
    assert(d.events==events&&!px);
    catalog[cn-1]^=1;
    assert(ext4_pkg_install_source(fs,package,catalog,cn,artifact,an,&s));
    assert(d.events==events&&!px);
    catalog[cn-1]^=1;
    artifact[an-1]^=1;
    assert(ext4_pkg_install_source(fs,package,catalog,cn,artifact,an,&s));
    assert(d.events==events&&!px);
    fail_read=0;
    assert(ext4_pkg_install_source(fs,package,catalog,cn,artifact,an,&s)==-VFS_EIO);
    fail_read=-1;
    assert(d.events==events&&!px);
    assert(!ext4_freeze_and_sync(fs));teardown();
    free(initial);free(artifact);free(catalog);free(trust);free(d.stable);free(d.volatile_bytes);
    puts("PASS ordinary callback: first install, verified no-op, bad signature/artifact refusal, I/O error return");
    return 0;
}

