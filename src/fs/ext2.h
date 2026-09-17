#ifndef FORTRESS_EXT2_H
#define FORTRESS_EXT2_H
#include "block.h"

/* Standard read-only ext2 mount (default). Refuses all writes and file creation. */
bool ext2_mount(block_dev_t *dev, const char *path);

/* Explicit opt-in read-write ext2 mount.
 * Refuses write mounting if:
 * 1. Device lacks write_sector or flush.
 * 2. Device has unsupported incompat features (anything other than FILETYPE).
 * 3. Device has unknown ro-compat features (anything other than SPARSE_SUPER, LARGE_FILE, BTREE_DIR).
 * 4. Filesystem state is dirty / unclean (s_state != 1).
 */
bool ext2_mount_rw(block_dev_t *dev, const char *path);

/* Sync all mounted ext2 filesystems and mark clean on shutdown */
void ext2_sync_all(void);
#endif
