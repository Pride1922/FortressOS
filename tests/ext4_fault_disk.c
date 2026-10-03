#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "ext4_fault_disk.h"
bool ext4_fault_read(block_dev_t *dev,uint64_t lba,void *data) {
    ext4_fault_disk_t *d=dev->priv;assert(lba<dev->sector_count);
    if (d->offline) return false;
    memcpy(data,d->volatile_bytes+lba*dev->sector_size,dev->sector_size);return true;
}
bool ext4_fault_write(block_dev_t *dev,uint64_t lba,const void *data) {
    ext4_fault_disk_t *d=dev->priv;assert(lba<dev->sector_count);
    if (d->offline) return false;
    if (d->trace) printf("replay event=%zu op=write lba=%llu\n",d->events,(unsigned long long)lba);
    bool cut=d->cut==(long)d->events++;d->writes++;
    if (cut && d->tear_bytes) {
        assert(d->tear_bytes<dev->sector_size);
        memcpy(d->stable+lba*dev->sector_size,data,d->tear_bytes);d->offline=true;return false;
    }
    if (cut && !d->after) { d->offline=true;return false; }
    memcpy(d->volatile_bytes+lba*dev->sector_size,data,dev->sector_size);
    if (d->persistence==1 || (d->persistence==2 && d->writes%2) ||
        (d->persistence==3 && !(d->writes%2)))
        memcpy(d->stable+lba*dev->sector_size,data,dev->sector_size);
    if (cut) { d->offline=true;return false; }
    return true;
}
bool ext4_fault_flush(block_dev_t *dev) {
    ext4_fault_disk_t *d=dev->priv;if (d->offline) return false;
    if (d->trace) printf("replay event=%zu op=flush\n",d->events);
    bool cut=d->cut==(long)d->events++;d->flushes++;
    if (cut && !d->after) { d->offline=true;return false; }
    memcpy(d->stable,d->volatile_bytes,d->bytes);
    if (cut) { d->offline=true;return false; }
    return true;
}
void ext4_fault_restart(ext4_fault_disk_t *d) {
    memcpy(d->volatile_bytes,d->stable,d->bytes);d->offline=false;
    d->cut=-1;d->events=d->writes=d->flushes=0;d->tear_bytes=0;
}
