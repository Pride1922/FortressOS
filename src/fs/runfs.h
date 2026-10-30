#ifndef FORTRESS_RUNFS_H
#define FORTRESS_RUNFS_H
#include "vfs.h"
/* Boot mounts /run and /tmp. One rank-1 exclusion, static 64-node pool,
 * 4096 bytes/ordinary mutable file, no storage I/O under the lock. Owned refs
 * retain unlinked nodes until final put; legacy callers retain boot lifetime.
 * /run/user is created root:root 0755; login is a later phase. */
void runfs_init(vfs_node_t *root);
/* Boot-only readonly tool alias in /run, before root preparation/publication or
 * namespace sealing. Caller supplies authenticated immutable module bytes,
 * reserved for the full boot. Uses one existing node credit, no payload copy,
 * allocation or storage I/O. Ordinary file quota remains 4096 bytes. */
int runfs_publish_boot_tool(const char *,const void *,size_t);
#endif
