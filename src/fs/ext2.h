#ifndef FORTRESS_EXT2_H
#define FORTRESS_EXT2_H
#include "block.h"

/* Boot-time read-only mount. Device and mounted nodes live for the boot lifetime.
 * No disk writes, unmount, symlinks, indexed directories, journal or extents.
 * Supports revision 0/1, 1/2/4 KiB blocks and classic indirect block maps. */
bool ext2_mount(block_dev_t *dev, const char *path);
#endif
