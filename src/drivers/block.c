#include "block.h"
#include "nvme.h"
#include "string.h"
#include "serial.h"

static block_dev_t *g_block_devs[MAX_BLOCK_DEVS];
static size_t       g_block_dev_count = 0;

void block_init(void) {
    memset(g_block_devs, 0, sizeof(g_block_devs));
    g_block_dev_count = 0;
}

bool block_register_dev(block_dev_t *dev) {
    if (!dev || g_block_dev_count >= MAX_BLOCK_DEVS) {
        return false;
    }

    /* Verify no duplicate names */
    for (size_t i = 0; i < g_block_dev_count; i++) {
        if (strcmp(g_block_devs[i]->name, dev->name) == 0) {
            return false;
        }
    }

    g_block_devs[g_block_dev_count++] = dev;
    serial_puts("[BLOCK] Registered block device: ");
    serial_puts(dev->name);
    serial_puts(" (");
    serial_print_dec(dev->sector_count);
    serial_puts(" sectors, ");
    serial_print_dec(dev->sector_size);
    serial_puts(" bytes/sector, Total: ");
    serial_print_dec((dev->sector_count * dev->sector_size) / (1024 * 1024));
    serial_puts(" MiB)\n");

    return true;
}

bool block_unregister_dev(block_dev_t *dev) {
    if (!dev) return false;
    for (size_t i = 0; i < g_block_dev_count; i++) {
        if (g_block_devs[i] == dev) {
            for (size_t j = i; j < g_block_dev_count - 1; j++) {
                g_block_devs[j] = g_block_devs[j + 1];
            }
            g_block_devs[--g_block_dev_count] = NULL;
            return true;
        }
    }
    return false;
}

block_dev_t *block_get_dev_by_name(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < g_block_dev_count; i++) {
        if (strcmp(g_block_devs[i]->name, name) == 0) {
            return g_block_devs[i];
        }
    }
    return NULL;
}

block_dev_t *block_get_dev_by_index(size_t index) {
    if (index >= g_block_dev_count) {
        return NULL;
    }
    return g_block_devs[index];
}

size_t block_get_dev_count(void) {
    return g_block_dev_count;
}

bool block_read_sector(block_dev_t *dev, uint64_t lba, void *buf) {
    if (!dev || !dev->read_sector || !buf) {
        return false;
    }
    if (lba >= dev->sector_count) {
        return false;
    }
    return dev->read_sector(dev, lba, buf);
}

bool block_write_sector(block_dev_t *dev, uint64_t lba, const void *buf) {
    if (!dev || !dev->write_sector || !buf) {
        return false;
    }
    if (lba >= dev->sector_count) {
        return false;
    }
    return dev->write_sector(dev, lba, buf);
}

bool block_flush(block_dev_t *dev) {
    if (!dev || !dev->flush) {
        return false;
    }
    return dev->flush(dev);
}

/* Backing implementations for base NVMe controller */
static bool nvme_block_read(block_dev_t *dev, uint64_t lba, void *buf) {
    (void)dev;
    return nvme_read_sector(lba, buf);
}

static bool nvme_block_write(block_dev_t *dev, uint64_t lba, const void *buf) {
    (void)dev;
    return nvme_write_sector(lba, buf);
}

static bool nvme_block_flush(block_dev_t *dev) {
    (void)dev;
    return nvme_flush();
}

static block_dev_t g_nvme_base_dev;

bool block_register_nvme(void) {
    if (!nvme_is_initialized()) {
        return false;
    }

    memset(&g_nvme_base_dev, 0, sizeof(g_nvme_base_dev));
    const char *dev_name = "nvme0n1";
    size_t name_len = strlen(dev_name);
    if (name_len >= sizeof(g_nvme_base_dev.name)) {
        name_len = sizeof(g_nvme_base_dev.name) - 1;
    }
    memcpy(g_nvme_base_dev.name, dev_name, name_len);
    g_nvme_base_dev.name[name_len] = '\0';

    g_nvme_base_dev.sector_size  = nvme_get_sector_size();
    g_nvme_base_dev.sector_count = nvme_get_sector_count();
    g_nvme_base_dev.read_sector  = nvme_block_read;
    g_nvme_base_dev.write_sector = nvme_block_write;
    g_nvme_base_dev.flush        = nvme_block_flush;
    g_nvme_base_dev.priv         = NULL;

    return block_register_dev(&g_nvme_base_dev);
}
