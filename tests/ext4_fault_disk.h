#ifndef EXT4_FAULT_DISK_H
#define EXT4_FAULT_DISK_H
#include "block.h"
/* Host-only power-loss model. Successful flush persists all accepted writes.
 * Sector callbacks are atomic unless the selected tear test says otherwise. */
typedef struct {
    uint8_t *stable,*volatile_bytes;
    size_t bytes,events,writes,flushes;
    long cut;
    bool after,offline;
    unsigned persistence; /* 0 cached, 1 all, 2 alternating, 3 inverse. */
    unsigned tear_bytes;
    bool trace;
} ext4_fault_disk_t;
bool ext4_fault_read(block_dev_t *dev,uint64_t lba,void *data);
bool ext4_fault_write(block_dev_t *dev,uint64_t lba,const void *data);
bool ext4_fault_flush(block_dev_t *dev);
void ext4_fault_restart(ext4_fault_disk_t *disk);
#endif
