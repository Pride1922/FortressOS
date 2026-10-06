/* Persistent actual mounted recovery fault worker; disposable host media only. */
#define EXT4_RECOVERY_LIBRARY
#include "ext4_crash_recover_host.c"
static uint8_t *dirty_sectors,*trace_bytes;
static size_t trace_size,trace_capacity;
static unsigned persistence_profile;
static void trace_event(block_dev_t *dev,unsigned kind,uint64_t lba,const void *bytes) {
    size_t n=16+(bytes ? dev->sector_size : 0);
    if (trace_size+n>trace_capacity) {
        trace_capacity=(trace_size+n)*2;trace_bytes=realloc(trace_bytes,trace_capacity);assert(trace_bytes);
    }
    uint32_t fields[4]={kind,(uint32_t)lba,e4_active!=NULL,bytes ? dev->sector_size : 0};
    memcpy(trace_bytes+trace_size,fields,16);
    if (bytes) memcpy(trace_bytes+trace_size+16,bytes,dev->sector_size);
    trace_size+=n;
}
static bool cut_write(block_dev_t *dev,uint64_t lba,const void *bytes) {
    ext4_fault_disk_t *d=dev->priv;if (d->offline) return false;
    assert(lba<dev->sector_count);trace_event(dev,1,lba,bytes);touched[lba]=1;
    bool accepted=d->cut!=(long)d->events || d->after;
    bool r=ext4_fault_write(dev,lba,bytes);
    if (accepted) {
        dirty_sectors[lba]=1;bool persist=persistence_profile==1 ||
            (persistence_profile==4 && d->writes%2) || (persistence_profile==5 && !(d->writes%2));
        uint64_t selected=lba;
        if ((persistence_profile==2 || persistence_profile==3) && !(d->writes%4)) {
            if (persistence_profile==2) {selected=0;while (!dirty_sectors[selected]) selected++;}
            else {selected=dev->sector_count-1;while (!dirty_sectors[selected]) selected--;}
            persist=true;
        }
        if (persist) {memcpy(d->stable+selected*dev->sector_size,d->volatile_bytes+selected*dev->sector_size,dev->sector_size);dirty_sectors[selected]=0;}
    }
    return r;
}
static bool cut_flush(block_dev_t *dev) {
    ext4_fault_disk_t *d=dev->priv;if (d->offline) return false;
    trace_event(dev,0,0,NULL);bool accepted=d->cut!=(long)d->events || d->after;
    bool r=ext4_fault_flush(dev);if (accepted) memset(dirty_sectors,0,dev->sector_count);return r;
}
int main(int argc,char **argv) {
    assert(argc==3);size_t bytes;uint8_t *base=load(argv[1],&bytes);unsigned ss=(unsigned)strtoul(argv[2],NULL,10);
    assert((ss==512 || ss==4096) && !(bytes%ss));
    ext4_fault_disk_t disk={.stable=malloc(bytes),.volatile_bytes=malloc(bytes),.bytes=bytes,.cut=-1};
    touched=calloc(bytes/ss,1);dirty_sectors=calloc(bytes/ss,1);assert(touched && dirty_sectors && disk.stable && disk.volatile_bytes);
    block_dev_t dev={.sector_size=ss,.sector_count=bytes/ss,.read_sector=ext4_fault_read,.write_sector=cut_write,.flush=cut_flush,.priv=&disk};
    uint32_t request[5];
    while (fread(request,sizeof(request),1,stdin)==1) {
        unsigned count=request[0],op=request[1];assert(count<=dev.sector_count && op<OPERATIONS && request[3]<2 && request[4]<6);
        reset(&disk,base);memset(touched,0,dev.sector_count);memset(dirty_sectors,0,dev.sector_count);trace_size=0;
        for (unsigned k=0;k<count;k++) {
            uint32_t lba;assert(fread(&lba,4,1,stdin)==1 && lba<dev.sector_count && !touched[lba]);touched[lba]=1;
            assert(fread(disk.stable+(size_t)lba*ss,ss,1,stdin)==1);
        }
        ext4_fault_restart(&disk);disk.cut=request[2]==UINT32_MAX ? -1 : (long)request[2];disk.after=request[3]!=0;persistence_profile=request[4];
        ext4_mount_t *fs=NULL;int r=ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs);unsigned phase=0;
        if (r) assert(r==-VFS_EIO && disk.offline && !fs && !e4_active && !vfs_lookup("/mnt") && !e4_engine_busy);
        else {
            phase=1;assert(fs);exact_namespace(op,true,fs->bs);r=ext4_freeze_and_sync(fs);
            if (r) assert(r==-VFS_EIO && disk.offline && fs->engine->tainted);
            else {assert(!disk.offline);clean_check(fs);phase=2;}
        }
        uint32_t changed=0;for (size_t k=0;k<dev.sector_count;k++) if (touched[k] && memcmp(disk.stable+k*ss,base+k*ss,ss)) changed++;
        assert(trace_size<=UINT32_MAX);
        uint32_t response[7]={(uint32_t)r,phase,(uint32_t)disk.events,(uint32_t)disk.writes,(uint32_t)disk.flushes,changed,(uint32_t)trace_size};
        assert(fwrite(response,sizeof(response),1,stdout)==1);
        for (size_t k=0;k<dev.sector_count;k++) if (touched[k] && memcmp(disk.stable+k*ss,base+k*ss,ss)) {
            uint32_t lba=(uint32_t)k;assert(fwrite(&lba,4,1,stdout)==1 && fwrite(disk.stable+k*ss,ss,1,stdout)==1);
        }
        assert(fwrite(trace_bytes,trace_size,1,stdout)==1 && !fflush(stdout));
    }
    assert(feof(stdin));teardown();free(base);free(touched);free(dirty_sectors);free(trace_bytes);free(disk.stable);free(disk.volatile_bytes);return 0;
}
