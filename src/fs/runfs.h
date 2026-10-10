#ifndef FORTRESS_RUNFS_H
#define FORTRESS_RUNFS_H
#include "vfs.h"
/* Boot mounts /run and /tmp. One rank-1 exclusion, static 64-node pool,
 * 4096 bytes/file, no allocation or storage I/O under the lock. Owned refs
 * retain unlinked nodes until final put; legacy callers retain boot lifetime.
 * /run/user is created root:root 0755; login is a later phase. */
void runfs_init(vfs_node_t *root);
#endif
