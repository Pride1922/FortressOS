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
} block_dev_t;

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

/* Register NVMe active namespace as block device "nvme0n1" */
bool         block_register_nvme(void);

#endif /* FORTRESS_BLOCK_H */
