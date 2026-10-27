/* Actual GPT / root policy / VFS / EXT4 / JBD2. PCI/NVMe inventory and ELF/VMM
 * are labelled host adapters; real ELF execution is a separate guest gate. */
#define gpt_get_partition_count unused_fixture_partition_count
#define gpt_get_partition unused_fixture_partition
#define TEST_PERMISSIONS_ENFORCEMENT
#define main unused_phase0_main
#include "ext4_mount_host.c"
#undef main
#undef gpt_get_partition_count
#undef gpt_get_partition
#include <unistd.h>
#include "../src/kernel/creds.c"
#include "../src/fs/permission_values.c"
static bool image_read(block_dev_t *dev,uint64_t lba,void *bytes) {
    FILE *stream=dev->priv;
    return !fseeko(stream,lba*dev->sector_size,SEEK_SET) && fread(bytes,1,dev->sector_size,stream)==dev->sector_size;
}
static bool image_write(block_dev_t *dev,uint64_t lba,const void *bytes) {
    FILE *stream=dev->priv;
    return !fseeko(stream,lba*dev->sector_size,SEEK_SET) && fwrite(bytes,1,dev->sector_size,stream)==dev->sector_size;
}
static bool image_flush(block_dev_t *dev) {FILE *f=dev->priv;return !fflush(f) && !fsync(fileno(f));}
#include "../src/fs/gpt.c"
#include "../src/lib/crc32.c"
#include "../src/kernel/installed_root.h"
#include "../src/kernel/elf.h"
#include "../src/mm/vmm.h"
#include "../src/drivers/pci.h"
static block_dev_t disk;
static block_dev_t *registry[16];static size_t devices;
static unsigned controllers=1;
static bool inventory_complete=true;
static bool unique=true,flush_ok=true,write_ok=true;
static size_t io_writes;
void serial_print_dec(uint64_t n) {(void)n;}
void serial_print_hex(uint64_t n) {(void)n;}
void console_puts(const char *s) {(void)s;}
void mouse_dev_init(vfs_node_t *directory) {(void)directory;}
void block_init(void) {devices=0;}
bool block_register_dev(block_dev_t *d) {if (devices==16) return false;registry[devices++]=d;return true;}
bool block_unregister_dev(block_dev_t *d) {
    for (size_t i=0;i<devices;i++) if (registry[i]==d) {registry[i]=registry[--devices];return true;}
    return false;
}
block_dev_t *block_get_dev_by_name(const char *name) {
    for (size_t i=0;i<devices;i++) if (!strcmp(registry[i]->name,name)) return registry[i];
    return NULL;
}
bool block_register_nvme(void) {return block_register_dev(&disk);}
bool nvme_init(void) {return true;}
bool nvme_has_unique_namespace(void) {return unique;}
bool nvme_root_geometry_supported(void) {return true;}
uint32_t nvme_get_sector_size(void) {return 512;}
bool nvme_quiesce(void) {return true;}
/* Pinned storage inventory adapter. The old "multiple" negative fixture
 * models an incoherent inventory; actual multi-disk discovery is a guest gate. */
size_t storage_inventory_boot(void) {return inventory_complete && controllers==1 ? 1 : 0;}
bool storage_inventory_select(unsigned i) {return i==0 && inventory_complete && controllers==1;}
bool storage_read_sector(uint64_t l,void *p) {return image_read(&disk,l,p);}
bool storage_has_unique_namespace(void) {return nvme_has_unique_namespace();}
bool storage_root_geometry_supported(void) {return true;}
uint32_t storage_get_sector_size(void) {return 512;}
uint64_t storage_get_sector_count(void) {return disk.sector_count;}
bool storage_init(void) {return true;}
bool storage_register_selected(void) {return block_register_nvme();}
block_dev_t *storage_selected_disk(void) {return &disk;}
bool storage_quiesce(void) {return nvme_quiesce();}
bool pci_inventory_complete(void) {return inventory_complete;}
size_t pci_find_all_devices(uint8_t c,uint8_t s,uint8_t p,pci_device_t *out,size_t cap) {
    (void)c;(void)s;(void)p;(void)out;assert(cap==2);return controllers;
}
int elf_load_executable(const void *p,size_t n,elf_loaded_process_t *out) {
    if (n<64 || memcmp(p,"\177ELF",4)) return ELF_ERR_INVALID;
    memset(out,0,sizeof(*out));return ELF_OK;
}
int vmm_destroy_pml4(uintptr_t p,bool owned) {(void)p;assert(owned);return VMM_OK;}
static bool write_adapter(block_dev_t *d,uint64_t l,const void *p) {io_writes++;return write_ok && image_write(d,l,p);}
static bool flush_adapter(block_dev_t *d) {return flush_ok && image_flush(d);}
static void seed_pending(void) {
    block_init();assert(block_register_nvme());gpt_policy_result_t policy;
    assert(gpt_parse_ex(&disk,&policy) && policy==GPT_POLICY_PRIMARY_CONSISTENT);
    block_dev_t *part=&gpt_get_partition(1)->block_dev;
    uint8_t sb[1024];assert(block_read_sector(part,2,sb) && block_read_sector(part,3,sb+512));
    e4_p32(sb+96,e4_u32(sb+96)|4);e4_p16(sb+58,0);e4_p32(sb+1020,e4_crc(UINT32_MAX,sb,1020));
    assert(block_write_sector(part,2,sb) && block_write_sector(part,3,sb+512) && block_flush(part));
    ext4_engine_t *engine=NULL;assert(!ext4_engine_open_journal_orphans(part,true,&engine));
    e4_inode_t inode;uint8_t *raw;
    assert(!e4_orphan_begin(engine) && !e4_load_plan(engine,2,&inode,&raw));
    e4_inode_sum(engine->fs,2,raw);
    assert(engine->images==1 && !jbd2_writer_begin(engine->writer,(jbd2_credits_t){1,0,0}));
    assert(!jbd2_writer_metadata(engine->writer,engine->image[0].block,engine->image[0].bytes));
    assert(!jbd2_writer_commit(engine->writer));ext4_engine_close(engine);
    jbd2_plan_t *plan=NULL;jbd2_report_t report;
    assert(!ext4_journal_analyze(part,&plan,&report) && report.transactions==1);jbd2_release(plan);
    puts("PASS committed uncheckpointed root journal seed (actual JBD2 writer)");
}
int main(int argc,char **argv) {
    assert(argc==8);FILE *stream=fopen(argv[1],"r+b");assert(stream);
    assert(!fseeko(stream,0,SEEK_END));uint64_t sectors=ftello(stream)/512;
    disk=(block_dev_t){.name="nvme0n1",.sector_size=512,.sector_count=sectors,.read_sector=image_read,.write_sector=write_adapter,.flush=flush_adapter,.priv=stream};
    if (!strcmp(argv[7],"seed-replay")) {seed_pending();assert(!fclose(stream));return 0;}
    size_t kn,bn,rn,cn;void *kernel=load(argv[2],&kn),*bootstrap=load(argv[3],&bn),*ready=load(argv[4],&rn),*credential=load(argv[5],&cn);
    boot_info_t info={.has_initramfs=true,.installed_modules_valid=true,.kernel_file_vaddr=(uint64_t)kernel,.kernel_file_size=kn,
        .initramfs_vaddr=(uint64_t)bootstrap,.initramfs_size=bn,.readiness_vaddr=(uint64_t)ready,.readiness_size=rn};
    snprintf(info.cmdline,sizeof(info.cmdline),"boot_mode=installed root=PARTUUID=%s",argv[6]);
    vfs_init();vfs_node_t *initial=g_vfs_root;
    vfs_node_t *n=vfs_create_node("/bootstrap/recovery.v1",VFS_FILE,cn,credential);assert(n);n->mode=VFS_S_IFREG|0600;n->parent->mode=VFS_S_IFDIR|0700;
    const char *kind=argv[7];
    if (!strcmp(kind,"multiple")) controllers=2;
    if (!strcmp(kind,"inventory")) inventory_complete=false;
    if (!strcmp(kind,"namespaces")) unique=false;
    if (!strcmp(kind,"flush")) flush_ok=false;
    if (!strcmp(kind,"replay")) write_ok=false;
    if (!strcmp(kind,"truncated")) info.cmdline_truncated=true;
    bool good=installed_root_boot(&info);
    if (!strcmp(kind,"success") || !strcmp(kind,"freeze-taint")) {
        assert(good && g_vfs_root!=initial && e4_active);
        creds_t root;creds_init_root(&root);int error;
        file_t *f=vfs_open_creds("/root/host-proof",VFS_O_CREAT|VFS_O_WRONLY,&root,&error);assert(f);
        const char proof[]="actual admitted root\n";
        assert(vfs_write_creds(f,proof,sizeof(proof)-1,&root)==sizeof(proof)-1);vfs_close(f);
        bool handled;assert(!ext4_sync_installed_root(&handled) && handled);
        assert(ext4_quiesce_installed_root(nvme_quiesce)==-VFS_EAGAIN);
        if (!strcmp(kind,"freeze-taint")) {
            flush_ok=false;assert(ext4_freeze_installed_root(&handled)==-VFS_EIO && handled && e4_active->frozen && e4_active->engine->tainted);
            assert(e4_state(ext4_prepared_root(e4_active))==-VFS_EINVAL);
            assert(e4_state(g_vfs_root)==-VFS_EIO && ext4_sync_installed_root(&handled)==-VFS_EIO);
            assert(ext4_quiesce_installed_root(nvme_quiesce)==-VFS_EIO);
        } else {
            assert(!ext4_freeze_installed_root(&handled) && handled);
            assert(!ext4_quiesce_installed_root(nvme_quiesce));
            size_t before=reads;uint8_t bytes[512];assert(!ext4_device_read_sector(&disk,0,bytes,&handled) && handled && reads==before);
            assert(ext4_sync_installed_root(&handled)==-VFS_EIO);
        }
    } else {
        assert(!good && g_vfs_root==initial && vfs_boot_namespace_ready());
        assert(installed_root_recovery_hash());
        if (strcmp(kind,"replay")) assert(!io_writes);
        else assert(io_writes);
    }
    /* Explicit simulated reboot, preserving production boot-lifetime policy. */
    if (e4_active) {ext4_mount_t *fs=e4_active;g_vfs_root=initial;e4_active=NULL;fs->journal_mounted=false;e4_engine_busy=false;e4_engine_dispose(fs->engine);}
    vfs_node_t *directory=n->parent;kfree(n);kfree(directory);initial->children=NULL;
    kfree(initial);g_vfs_root=NULL;assert(!live && !e4_engine_busy);
    free(kernel);free(bootstrap);free(ready);free(credential);assert(!fclose(stream));
    printf("PASS actual root admission %s; %zu writes (PCI/NVMe/ELF host adapters)\n",kind,io_writes);
}
