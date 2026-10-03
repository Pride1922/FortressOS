#ifndef FORTRESS_EXT4_H
#define FORTRESS_EXT4_H

#include "block.h"

/* Phase 2 implements read-only metadata and extent-backed reads at /mnt.
 * Production USB dispatch is not enabled. Read callbacks return at most 64 KiB;
 * trees are limited to depth 2 and 4096 nodes/extents per mounted map budget.
 * Mounted media must remain immutable; validated maps are cached for its lifetime.
 * Caller owns the partition block device for the entire mounted lifetime.
 * All calls require unlocked thread/boot context. Return 0 or negative VFS
 * errno; failed admission performs no filesystem writes and publishes nothing.
 * RO never replays a journal onto the source device. RW requires external
 * PARTUUID/GPT/durability admission plus the driver's feature validation. */
typedef struct ext4_mount ext4_mount_t;
int ext4_mount_ro(block_dev_t *partition, const char *path, ext4_mount_t **out);
int ext4_mount_rw(block_dev_t *partition, const char *path, ext4_mount_t **out);

/* Mid-session: commit/checkpoint (when journaled), then device barrier.
 * Does not freeze or mark clean. Failed barrier taints the mount. */
int ext4_sync(ext4_mount_t *mount);

/* Shutdown-only: stop new mutations, drain, barrier, then mark clean only
 * if healthy. A failure leaves frozen/tainted state and never claims clean.
 * Mount objects/VFS nodes stay alive; no unmount/lifetime change is implied. */
int ext4_freeze_and_sync(ext4_mount_t *mount);

#endif
