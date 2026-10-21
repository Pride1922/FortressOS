/* Actual EXT4 + redo; deterministic interruption at the durable empty-create
 * boundary. Host-only callback adapter, not a production fault hook. */
#define TEST_PERMISSIONS_ENFORCEMENT
#define main unused_mount_main
#include "ext4_mount_host.c"
#undef main
#include "../src/kernel/creds.c"
#include "../src/fs/permission_values.c"
#include "../src/include/account_redo.h"
static FILE *image;
static unsigned char *disk_bytes;
static ext4_mount_t *mounted;
static account_io_t backend;
static account_workspace_t workspace;
static unsigned interrupt_role=ACCOUNT_ROLE_COUNT,writes_seen,interrupted;
#ifdef TEST_PKG_NAMESPACE_ADMISSION
static bool ns_deny_reads;
#endif
static bool rd(block_dev_t *d,uint64_t l,void *p) {
#ifdef TEST_PKG_NAMESPACE_ADMISSION
    if (ns_deny_reads) return false;
#endif
    if(l>=d->sector_count)return false;
    memcpy(p,disk_bytes+l*d->sector_size,d->sector_size);return true;
}
static bool wr(block_dev_t *d,uint64_t l,const void *p) {
    writes_seen++;
    if(l>=d->sector_count)return false;
    memcpy(disk_bytes+l*d->sector_size,p,d->sector_size);
    return !fseeko(image,l*d->sector_size,SEEK_SET) && fwrite(p,1,d->sector_size,image)==d->sector_size;
}
static bool flush(block_dev_t *d) {(void)d;return !fflush(image);}
static int valid(void *p,const char *const values[3],const size_t lengths[3]) {
    (void)p;(void)values;
    for(unsigned i=0;i<3;i++)if(!lengths[i] || lengths[i]>8192)return -VFS_EINVAL;
    return 0; /* Synthetic byte profile; not account-service validation. */
}
static int write_role(void *p,unsigned role,const void *data,size_t length) {
    if(role!=interrupt_role)return backend.write(p,role,data,length);
    const char *name;int error;vfs_node_t *parent=e4_ac_parent(p,role,&name,&error);assert(parent);
    vfs_create_attrs_t attrs={.mode=e4_ac_mode(role),.uid=0,.gid=0};
    vfs_node_t *node=e4_jcreate_account(parent,name,VFS_FILE,&error,true,&attrs,NULL,true);
    vfs_node_put(parent);assert(node);assert(node->size==0);vfs_node_put(node);
    assert(!backend.sync(p));interrupted++;
    printf("INTERRUPT durable_empty_role=%u\n",role);return -VFS_EIO;
}
#ifdef TEST_PKG_NAMESPACE_ADMISSION
#include "pkg_namespace_kernel_host.inc"
#endif
int main(int argc,char **argv) {
    assert(argc==4);unsigned sector=(unsigned)strtoul(argv[2],NULL,10);
    image=fopen(argv[1],"r+b");assert(image && !fseeko(image,0,SEEK_END));
    size_t bytes=ftello(image);assert(bytes && bytes%sector==0);
    disk_bytes=malloc(bytes);assert(disk_bytes && !fseeko(image,0,SEEK_SET));
    assert(fread(disk_bytes,1,bytes,image)==bytes);
    block_dev_t dev={.sector_size=sector,.sector_count=bytes/sector,.read_sector=rd,.write_sector=wr,.flush=flush};
    vfs_init();assert(!ext4_prepare_root(&dev,true,&mounted));
#ifdef TEST_PKG_NAMESPACE_ADMISSION
    bool namespace_only_test=!strcmp(argv[3],"reject") || !strcmp(argv[3],"oom") || !strcmp(argv[3],"read-error");
    if (!namespace_only_test) {assert(!ext4_root_account_io(mounted,&backend));}
#else
    assert(!ext4_root_account_io(mounted,&backend));
#endif
    account_io_t io=backend;
    io.write=write_role;io.validate=valid;
#ifdef TEST_PKG_NAMESPACE_ADMISSION
    if (ns_kernel_test(argv[3])) {
        assert(!ext4_freeze_and_sync(mounted));assert(!fclose(image));free(disk_bytes);return 0;
    }
#endif
    unsigned char sb[1024];assert(e4_bytes(mounted,1024,sb,sizeof(sb)));
    assert((uint64_t)e4_u32(sb+12)*mounted->bs>262144 && e4_u32(sb+16)>16);
    printf("HEADROOM blocks=%u inodes=%u block_size=%u\n",e4_u32(sb+12),e4_u32(sb+16),mounted->bs);
    bool update=!strcmp(argv[3],"update") || !strcmp(argv[3],"shadow");
    if(!strcmp(argv[3],"shadow"))interrupt_role=ACCOUNT_TEMP_SHADOW;
    if(!strcmp(argv[3],"group"))interrupt_role=ACCOUNT_TEMP_GROUP;
    int result;
#ifdef TEST_PKG_NAMESPACE_ADMISSION
    unsigned recovery_start=writes_seen;
#endif
    if(update) {
        assert(!account_redo_recover(&io,&workspace));
        for(unsigned i=0;i<3;i++){workspace.lengths[i]=8192;memset(workspace.files[i],'A'+i,8192);}
        result=account_redo_update(&io,&workspace,1);
    } else result=account_redo_recover(&io,&workspace);
#ifdef TEST_PKG_NAMESPACE_ADMISSION
    if(!strcmp(argv[3],"bad-intent")) {
        assert(result==-VFS_EINVAL && writes_seen==recovery_start);
        puts("SEMANTIC REJECT PASS corrupt intent rejected by redo after structural fencing; writes=0");
    } else
#endif
    if(interrupt_role<ACCOUNT_ROLE_COUNT){assert(interrupted==1 && result==-VFS_EIO);}
    else if(!strcmp(argv[3],"blocked")) {
        assert(result==-VFS_EFBIG);assert(!backend.sync(backend.context));
        unsigned before=writes_seen;
        assert(account_redo_recover(&io,&workspace)==-VFS_EFBIG);
        assert(writes_seen==before);
        puts("BLOCKED expected_EFBIG repeated_attempt_writes=0");
    } else assert(!result);
    assert(!ext4_freeze_and_sync(mounted));assert(!fclose(image));free(disk_bytes);
    printf("PASS mode=%s result=%d writes=%u\n",argv[3],result,writes_seen);return 0;
}
