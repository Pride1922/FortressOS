/* Phase 9.1 event inventory. Actual mounted VFS/EXT4/JBD2; no fault campaign. */
#define EXT4_INTEGRATION_LIBRARY
#include "ext4_integration_host.c"

static FILE *events_file,*payload_file;
static bool recording;
static size_t payload_offset;
static void inventory_event(block_dev_t *dev,const char *kind,uint64_t lba,const void *bytes) {
    if (!recording) return;
    ext4_fault_disk_t *d=dev->priv;
    jbd2_writer_t *w=e4_active->engine->writer;
    fprintf(events_file,"{\"index\":%zu,\"kind\":\"%s\",\"lba\":%llu,\"state\":%u,"
        "\"sequence\":%u,\"metadata\":%u,\"data\":%u,\"revokes\":%u,\"payload_offset\":%zu,\"parts\":[",
        d->events,kind,(unsigned long long)lba,(unsigned)w->state,w->sequence,
        w->metadata_count,w->data_count,w->revoke_count,payload_offset);
    if (bytes) {
        const uint8_t *p=bytes;uint64_t start=lba*dev->sector_size;bool first=true;
        for (unsigned at=0;at<dev->sector_size;) {
            unsigned block=(unsigned)((start+at)/w->io->bs),skip=(unsigned)((start+at)%w->io->bs);
            unsigned take=w->io->bs-skip;if (take>dev->sector_size-at) take=dev->sector_size-at;
            bool journal=false;
            for (unsigned k=0;k<w->io->length;k++) if (w->map[k]==block) { journal=true;break; }
            const char *role=journal ? "journal" : !memcmp(p+at,d->volatile_bytes+start+at,take) ?
                "unchanged" : w->state==JBD2_WRITE_CHECKPOINTING ? "metadata" : "ordered";
            /* 4096-byte RMW neighbors can contain an old commit header. Only
             * changed slices identify a newly submitted journal record. */
            unsigned type=journal && !skip && take>=12 && memcmp(p+at,d->volatile_bytes+start+at,take) &&
                j_be(p+at)==J_MAGIC ? j_be(p+at+4) : 0;
            fprintf(events_file,"%s{\"block\":%u,\"skip\":%u,\"offset\":%u,\"length\":%u,"
                "\"role\":\"%s\",\"journal_type\":%u}",first ? "" : ",",block,skip,at,take,role,type);
            first=false;at+=take;
        }
        assert(fwrite(bytes,1,dev->sector_size,payload_file)==dev->sector_size);
        payload_offset+=dev->sector_size;
    }
    fputs("]}\n",events_file);
}
static bool inventory_write(block_dev_t *dev,uint64_t lba,const void *bytes) {
    inventory_event(dev,"write",lba,bytes);return audited_write(dev,lba,bytes);
}
static bool inventory_flush(block_dev_t *dev) {
    inventory_event(dev,"flush",0,NULL);return audited_flush(dev);
}
int main(int argc,char **argv) {
    (void)shared_offsets;(void)staging_failures;(void)recovered_oracle;
    assert(argc==4 || (argc==5 && !strcmp(argv[4],"--write-only")));
    size_t n;uint8_t *initial=load(argv[1],&n);
    unsigned ss=(unsigned)strtoul(argv[2],NULL,10);
    assert((ss==512 || ss==4096) && n%ss==0);
    ext4_fault_disk_t disk={.stable=malloc(n),.volatile_bytes=malloc(n),.bytes=n,.cut=-1};
    assert(disk.stable && disk.volatile_bytes);
    block_dev_t dev={.sector_size=ss,.sector_count=n/ss,.read_sector=ext4_fault_read,
        .write_sector=inventory_write,.flush=inventory_flush,.priv=&disk};
    for (unsigned op=0;op<OPERATIONS;op++) {
        if (argc==5 && op!=WRITE) continue;
        file_t *f=prepare(&dev,initial,op);char prefix[1024],path[1100];
        assert(snprintf(prefix,sizeof(prefix),"%s-%s",argv[3],names[op])<(int)sizeof(prefix));
        save(prefix,"before",disk.stable,n);
        snprintf(path,sizeof(path),"%s.events.jsonl",prefix);events_file=fopen(path,"wx");assert(events_file);
        snprintf(path,sizeof(path),"%s.payload.bin",prefix);payload_file=fopen(path,"wbx");assert(payload_file);
        disk.events=disk.writes=disk.flushes=0;payload_offset=0;recording=true;
        assert(operate(op,f)>=0);recording=false;
        assert(!fclose(events_file) && !fclose(payload_file));save(prefix,"after",disk.stable,n);
        printf("inventory %s events=%zu writes=%zu flushes=%zu\n",names[op],disk.events,disk.writes,disk.flushes);
        if (f && op!=LAST_CLOSE) assert(!vfs_close(f));
        int freeze=ext4_freeze_and_sync(e4_active);
        if (freeze) fprintf(stderr,"freeze=%d taint=%u ready=%u error=%d orphans=%u revokes=%u images=%u\n",
            freeze,e4_active->engine->tainted,e4_active->engine->ready,e4_active->engine->error,
            e4_active->engine->orphan_count,e4_active->engine->revoke_count,e4_active->engine->images);
        assert(!freeze);clean_check(e4_active);save(prefix,"clean",disk.stable,n);
    }
    assert(metadata_writes && data_writes && journal_writes);
    teardown();free(initial);free(disk.stable);free(disk.volatile_bytes);puts("inventory PASS");return 0;
}
