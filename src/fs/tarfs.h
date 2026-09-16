#ifndef FORTRESS_TARFS_H
#define FORTRESS_TARFS_H

#include "types.h"
#include "vfs.h"

int tarfs_init(const void *archive_data, size_t archive_size);

#endif /* FORTRESS_TARFS_H */
