/* Persistent host recovery worker; exact sector deltas, fresh mount per input. */
#define EXT4_INTEGRATION_LIBRARY
#include "ext4_integration_host.c"
static uint8_t *touched;
static bool tracked_write(block_dev_t *dev,uint64_t lba,const void *bytes) {
    assert(lba<dev->sector_count);touched[lba]=1;return ext4_fault_write(dev,lba,bytes);
}
static void exact_file(const char *path,bool exists,uint64_t size,unsigned bs,bool fresh) {
    vfs_node_t *node=vfs_lookup(path);assert((node!=NULL)==exists);if (!node) return;
    assert(node->type==VFS_FILE && node->size==size);file_t *f=vfs_open(path,VFS_O_RDONLY);assert(f);
    uint8_t bytes[4096];uint64_t offset=0;
    while (offset<size) {
        size_t count=size-offset;if (count>sizeof(bytes)) count=sizeof(bytes);
        assert(vfs_read(f,bytes,count)==(int64_t)count);
        for (size_t k=0;k<count;k++) assert(bytes[k]==(fresh || offset+k>=3u*bs ? 'I' : 'O'));
        offset+=count;
    }
    assert(!vfs_read(f,bytes,1) && !vfs_close(f));
}
static void exact_namespace(unsigned op,bool committed,unsigned bs) {
    bool target=op!=LAST_CLOSE && op!=REUSE;
    if (committed && (op==RENAME || op==UNLINK || op==PIN_UNLINK)) target=false;
    uint64_t size=3u*bs;
    if (committed && op==WRITE) size=5u*bs;
    if (committed && op==APPEND) size+=17;
    if (committed && op==TRUNCATE) size=bs+17;
    exact_file("/mnt/target.bin",target,size,bs,false);
    exact_file("/mnt/sub/renamed.bin",op==RENAME && committed,3u*bs,bs,false);
    exact_file("/mnt/reuse.bin",op==REUSE,committed ? 2u*bs : 0,bs,true);
    exact_file("/mnt/new.bin",op==CREATE && committed,0,bs,false);
    assert((vfs_lookup("/mnt/newdir")!=NULL)==(op==MKDIR && committed));
    assert((vfs_lookup("/mnt/sub")!=NULL)==(op==RENAME || (op==RMDIR && !committed)));
    const char *allowed[]={".","..","lost+found","target.bin","new.bin","newdir","sub","reuse.bin"};
    bool seen[8]={false};vfs_dirent_t entry;vfs_node_t *root=vfs_lookup("/mnt");
    for (unsigned k=0;;k++) {
        int r=vfs_readdir(root,k,&entry);assert(r>=0);if (!r) break;
        unsigned i=0;while (i<8 && strcmp(entry.name,allowed[i])) i++;
        assert(i<8 && !seen[i]);seen[i]=true;
    }
    assert(seen[2] && seen[3]==target && seen[4]==(op==CREATE && committed) &&
        seen[5]==(op==MKDIR && committed) && seen[6]==(op==RENAME || (op==RMDIR && !committed)) && seen[7]==(op==REUSE));
    assert(!e4_active->engine->orphan_count);
}
int main(int argc,char **argv) {
    (void)shared_offsets;(void)staging_failures;(void)recovered_oracle;(void)prepare;(void)operate;(void)names;
    (void)audited_write;(void)audited_flush;
    assert(argc==3);size_t bytes;uint8_t *base=load(argv[1],&bytes);unsigned ss=(unsigned)strtoul(argv[2],NULL,10);
    assert((ss==512 || ss==4096) && !(bytes%ss));
    ext4_fault_disk_t disk={.stable=malloc(bytes),.volatile_bytes=malloc(bytes),.bytes=bytes,.cut=-1};
    touched=calloc(bytes/ss,1);assert(touched && disk.stable && disk.volatile_bytes);
    block_dev_t dev={.sector_size=ss,.sector_count=bytes/ss,.read_sector=ext4_fault_read,
        .write_sector=tracked_write,.flush=ext4_fault_flush,.priv=&disk};
    uint32_t request[3];
    while (fread(request,sizeof(request),1,stdin)==1) {
        unsigned count=request[0],op=request[1],expected=request[2];assert(count<=dev.sector_count && op<OPERATIONS && expected<3);
        reset(&disk,base);memset(touched,0,bytes/ss);
        for (unsigned k=0;k<count;k++) {
            uint32_t lba;assert(fread(&lba,sizeof(lba),1,stdin)==1 && lba<dev.sector_count && !touched[lba]);
            touched[lba]=1;assert(fread(disk.stable+(size_t)lba*ss,ss,1,stdin)==1);
        }
        ext4_fault_restart(&disk);ext4_mount_t *fs=NULL;
        int r=ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs);
        if (expected==2) assert((r==-VFS_EIO || r==-VFS_EOPNOTSUPP) && !fs && !e4_active &&
            !disk.writes && !disk.flushes && !vfs_lookup("/mnt"));
        else {
            assert(!r && fs);exact_namespace(op,expected==1,fs->bs);
            assert(!ext4_freeze_and_sync(fs));clean_check(fs);
        }
        uint32_t changed=0;
        for (size_t k=0;k<bytes/ss;k++) if (touched[k] && memcmp(disk.stable+k*ss,base+k*ss,ss)) changed++;
        uint32_t response[5]={(uint32_t)(expected==2), (uint32_t)r,(uint32_t)disk.writes,(uint32_t)disk.flushes,changed};
        assert(fwrite(response,sizeof(response),1,stdout)==1);
        for (size_t k=0;k<bytes/ss;k++) if (touched[k] && memcmp(disk.stable+k*ss,base+k*ss,ss)) {
            uint32_t lba=(uint32_t)k;assert(fwrite(&lba,sizeof(lba),1,stdout)==1 && fwrite(disk.stable+k*ss,ss,1,stdout)==1);
        }
        assert(!fflush(stdout));
    }
    assert(feof(stdin));teardown();free(touched);free(base);free(disk.stable);free(disk.volatile_bytes);return 0;
}
