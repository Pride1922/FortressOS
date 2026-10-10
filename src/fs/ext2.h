#ifndef FORTRESS_EXT2_H
#define FORTRESS_EXT2_H
#include "block.h"

/* Trusted devfs fallback read adapter: no locks held, no EXT4 mount published.
 * Boot-only mount selection; shares EXT2 exclusion, never nested rank-1 locks. */
bool ext2_device_read_sector(block_dev_t *,uint64_t,void *);

/* Standard read-only ext2 mount (default). Refuses all writes and file creation. */
/* Both mount modes require Linux creator OS: foreign osd2 ID layouts are not
 * decoded as Linux ownership. Rejection performs no writes/publication. */
bool ext2_mount(block_dev_t *dev, const char *path);

/* Explicit opt-in read-write ext2 mount.
 * Refuses write mounting if:
 * 1. Device lacks write_sector or flush.
 * 2. Device has unsupported incompat features (anything other than FILETYPE).
 * 3. Device has unknown ro-compat features (anything other than SPARSE_SUPER, LARGE_FILE).
 * 4. Filesystem state is dirty / unclean (s_state != 1).
 */
bool ext2_mount_rw(block_dev_t *dev, const char *path);

/* Node lifetime: owned lookup/create references and independent opens are
 * acquired/released under ext2_lock. Detached nodes retire after the last
 * reference; raw legacy lookups retain boot-lifetime tombstones. The cache is
 * bounded to 1024 nodes including tombstones. Unlink or rename replacement of
 * an inode with active opens returns EOPNOTSUPP before writes: this nonjournaled
 * implementation does not retain unlinked inode/block contents. */

/* Shutdown-only: freeze writes and mark healthy mounts clean. False on I/O
 * failure or taint; tainted mounts issue no further writes or flushes. */
bool ext2_sync_all(void);

/* Test-only: mark mounted ext2 filesystem as tainted (e.g. for Phase C audit).
 * Safe no-op if no filesystem is mounted. */
void ext2_mark_tainted(void);

#include "syscall_abi.h"
/* Passive, read-only query of mounted ext2 metrics (zero I/O, zero locks). */
bool ext2_get_mount_info(const void *vfs_node, mount_info_t *out);
#endif
