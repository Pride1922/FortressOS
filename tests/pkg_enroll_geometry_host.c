/* Actual GPT + EXT4/JBD2 read-only structural preview. Not boot admission. */
#define main retained_ext4_mount_main
#define gpt_get_partition_count retained_gpt_count_stub
#define gpt_get_partition retained_gpt_get_stub
#include "ext4_mount_host.c"
#undef main
#undef gpt_get_partition_count
#undef gpt_get_partition
#include "../src/fs/gpt.c"
#include "../src/lib/crc32.c"
void serial_print_dec(uint64_t n) {(void)n;}
void serial_print_hex(uint64_t n) {(void)n;}
bool block_register_dev(block_dev_t *d) {(void)d;return true;}
bool block_unregister_dev(block_dev_t *d) {(void)d;return true;}
static bool denied_write(block_dev_t *d,uint64_t l,const void *b) {
    (void)l;(void)b;((ext4_fault_disk_t *)d->priv)->writes++;return false;
}
static bool denied_flush(block_dev_t *d) {
    ((ext4_fault_disk_t *)d->priv)->flushes++;return false;
}
int main(int argc,char **argv) {
    assert(argc==3);size_t n;uint8_t *initial=load(argv[1],&n);
    ext4_fault_disk_t d={.stable=initial,.volatile_bytes=initial,.bytes=n,.cut=-1};
    block_dev_t disk={.sector_size=512,.sector_count=n/512,.read_sector=ext4_fault_read,
        .write_sector=denied_write,.flush=denied_flush,.priv=&d};
    gpt_policy_result_t policy;bool parsed=gpt_parse_ex(&disk,&policy);
    int r=-VFS_EINVAL;unsigned slots[2]={0};ext4_engine_t *e=NULL;
    if (parsed && policy==GPT_POLICY_PRIMARY_CONSISTENT && gpt_get_partition_count()==2) {
        gpt_partition_t *p=gpt_get_partition(0);gpt_guid_t wanted;
        assert(gpt_str_to_guid("12345678-1234-4567-89ab-123456789abc",&wanted));
        if (!strcmp(argv[2],"refuse-io")) fail_read=0;
        if (gpt_guid_equal(&p->unique_guid,&wanted) && gpt_guid_equal(&p->type_guid,&GPT_GUID_LINUX_FS) &&
            p->starting_lba==2048 && p->sector_count==65536)
            r=e4_open_journal_orphans_owned(&p->block_dev,true,&e,false,true);
        if (!r) {
            ext4_mount_t *fs=e->fs;e4_ns_workspace_t *w=kcalloc(1,sizeof(*w));assert(w);
            e4_inode_t root;r=e4_inode(fs,2,&root);
            if (!r && (root.mode!=VFS_S_IFDIR || root.uid || root.gid || (root.full_mode&07777)!=0755)) r=-VFS_EACCES;
            if (!r) r=e4_ns_child(fs,2,"etc",0755,&w->anchors[0]);
            if (!r) r=e4_ns_child(fs,2,"var",0755,&w->anchors[1]);
            if (!r) r=e4_ns_child(fs,w->anchors[1],"lib",0755,&w->anchors[2]);
            if (!r) r=e4_ns_child(fs,w->anchors[2],"fortress",0755,&w->anchors[3]);
            if (!r) r=e4_ns_child(fs,w->anchors[3],"accounts",0700,&w->anchors[4]);
            if (!r) r=e4_ns_directory(fs,w,0,2);
            if (!r) r=e4_ns_directory(fs,w,1,w->anchors[3]);
            if (!r && w->report.present_roles!=7) r=-VFS_EINVAL;
            slots[0]=w->report.slots[0];slots[1]=w->report.slots[1];
            assert(!fs->account_namespace_fenced && !fs->journal_mounted && !e4_active);
            kfree(w);ext4_engine_close(e);
        }
    }
    assert(!d.writes && !d.flushes && !live && !e4_engine_busy);
    bool accept=!strcmp(argv[2],"accept");assert(accept ? !r : r!=0);
    printf("PASS structural-preview expected=%s result=%d slots=%u,%u writes=%zu flushes=%zu policy=%u\n",
        argv[2],r,slots[0],slots[1],d.writes,d.flushes,(unsigned)policy);
    free(initial);return 0;
}
