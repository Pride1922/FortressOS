/* Phase 9.3: actual recovery event inventory from verified dirty inputs. */
#define EXT4_RECOVERY_LIBRARY
#include "ext4_crash_recover_host.c"
static FILE *recovery_events,*recovery_payload;
static size_t recovery_offset;
static bool recovery_write(block_dev_t *dev,uint64_t lba,const void *bytes) {
    ext4_fault_disk_t *d=dev->priv;
    fprintf(recovery_events,"{\"index\":%zu,\"kind\":\"write\",\"lba\":%llu,\"payload_offset\":%zu,\"published\":%s}\n",
        d->events,(unsigned long long)lba,recovery_offset,e4_active ? "true" : "false");
    assert(fwrite(bytes,dev->sector_size,1,recovery_payload)==1);recovery_offset+=dev->sector_size;
    return ext4_fault_write(dev,lba,bytes);
}
static bool recovery_flush(block_dev_t *dev) {
    ext4_fault_disk_t *d=dev->priv;
    fprintf(recovery_events,"{\"index\":%zu,\"kind\":\"flush\",\"lba\":0,\"payload_offset\":%zu,\"published\":%s}\n",
        d->events,recovery_offset,e4_active ? "true" : "false");
    return ext4_fault_flush(dev);
}
int main(int argc,char **argv) {
    assert(argc==5);size_t bytes;uint8_t *initial=load(argv[1],&bytes);
    unsigned ss=(unsigned)strtoul(argv[2],NULL,10),op=(unsigned)strtoul(argv[3],NULL,10);
    assert((ss==512 || ss==4096) && !(bytes%ss) && op<OPERATIONS);
    char path[1024];assert(snprintf(path,sizeof(path),"%s.events.jsonl",argv[4])<(int)sizeof(path));
    recovery_events=fopen(path,"wb");assert(recovery_events);
    assert(snprintf(path,sizeof(path),"%s.payload.bin",argv[4])<(int)sizeof(path));
    recovery_payload=fopen(path,"wb");assert(recovery_payload);
    ext4_fault_disk_t disk={.stable=malloc(bytes),.volatile_bytes=malloc(bytes),.bytes=bytes,.cut=-1};
    assert(disk.stable && disk.volatile_bytes);
    block_dev_t dev={.sector_size=ss,.sector_count=bytes/ss,.read_sector=ext4_fault_read,
        .write_sector=recovery_write,.flush=recovery_flush,.priv=&disk};
    reset(&disk,initial);ext4_mount_t *fs=NULL;
    assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs) && fs);
    exact_namespace(op,true,fs->bs);size_t admission_events=disk.events;
    assert(!ext4_freeze_and_sync(fs));clean_check(fs);
    save(argv[4],"clean",disk.stable,bytes);
    printf("recovery inventory PASS admission_events=%zu total_events=%zu writes=%zu flushes=%zu\n",
        admission_events,disk.events,disk.writes,disk.flushes);
    assert(!fclose(recovery_events) && !fclose(recovery_payload));teardown();
    free(initial);free(disk.stable);free(disk.volatile_bytes);return 0;
}
