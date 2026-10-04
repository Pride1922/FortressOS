#ifndef FORTRESS_BLOCK_H
#define FORTRESS_BLOCK_H

#include "types.h"

#define BLOCK_MAX_NAME 32
#define MAX_BLOCK_DEVS 16

typedef struct block_dev {
    char        name[BLOCK_MAX_NAME];
    uint32_t    sector_size;
    uint64_t    sector_count;
    bool      (*read_sector)(struct block_dev *dev, uint64_t lba, void *buf);
    bool      (*write_sector)(struct block_dev *dev, uint64_t lba, const void *buf);
    bool      (*flush)(struct block_dev *dev);
    void       *priv;
    /* Optional synchronous runs, bounded by max_run_bytes. Failure may have accepted
     * a prefix; callers must not retry writes or claim rollback. */
    bool (*read_sectors)(struct block_dev *,uint64_t,uint32_t,void *);
    bool (*write_sectors)(struct block_dev *,uint64_t,uint32_t,const void *);
    uint32_t max_run_bytes; /* Zero retains the legacy 4096-byte limit. */
} block_dev_t;

#define BLOCK_MAX_RUN_BYTES 16384u
static inline uint32_t block_get_max_run_bytes(const block_dev_t *d) {
    uint32_t n=d && d->max_run_bytes ? d->max_run_bytes : 4096u;
    return n>BLOCK_MAX_RUN_BYTES ? BLOCK_MAX_RUN_BYTES : n;
}

void         block_init(void);
bool         block_register_dev(block_dev_t *dev);
bool         block_unregister_dev(block_dev_t *dev);
block_dev_t *block_get_dev_by_name(const char *name);
block_dev_t *block_get_dev_by_index(size_t index);
size_t       block_get_dev_count(void);

/* Geometry and Capacity Accessors */
static inline uint32_t block_get_sector_size(const block_dev_t *dev) {
    return dev ? dev->sector_size : 0;
}

static inline uint64_t block_get_sector_count(const block_dev_t *dev) {
    return dev ? dev->sector_count : 0;
}

static inline uint64_t block_get_capacity_bytes(const block_dev_t *dev) {
    return dev ? (dev->sector_count * (uint64_t)dev->sector_size) : 0;
}

/* Bounds-Checked Uniform Block I/O Operations */
bool         block_read_sector(block_dev_t *dev, uint64_t lba, void *buf);
bool         block_write_sector(block_dev_t *dev, uint64_t lba, const void *buf);
bool         block_flush(block_dev_t *dev);

/* Bounded run adapters retain existing single-sector backends and host fault
 * shims. Validate the complete run before any callback; never retry a failed
 * bulk callback through the sector fallback. */
static inline bool block_read_sectors(block_dev_t *d,uint64_t l,uint32_t n,void *b) {
    if (!d || !b || !d->read_sector || !d->sector_size || !n ||
        n>block_get_max_run_bytes(d)/d->sector_size || l>=d->sector_count || n>d->sector_count-l) return false;
    if (d->read_sectors) return d->read_sectors(d,l,n,b);
    for (uint32_t i=0;i<n;i++) if (!block_read_sector(d,l+i,(uint8_t *)b+i*d->sector_size)) return false;
    return true;
}
static inline bool block_write_sectors(block_dev_t *d,uint64_t l,uint32_t n,const void *b) {
    if (!d || !b || !d->write_sector || !d->sector_size || !n ||
        n>block_get_max_run_bytes(d)/d->sector_size || l>=d->sector_count || n>d->sector_count-l) return false;
    if (d->write_sectors) return d->write_sectors(d,l,n,b);
    for (uint32_t i=0;i<n;i++) if (!block_write_sector(d,l+i,(const uint8_t *)b+i*d->sector_size)) return false;
    return true;
}

/* Register NVMe active namespace as block device "nvme0n1" */
bool         block_register_nvme(void);

/* Register USB Mass Storage as block device "sda" */
bool         block_register_usb(void);

#endif /* FORTRESS_BLOCK_H */
