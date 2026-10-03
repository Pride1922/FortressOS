#ifndef FORTRESS_EXT4_H
#define FORTRESS_EXT4_H

#include "block.h"

/* E4-A restricted-profile mounts at /mnt. Production USB dispatch remains
 * disabled pending Phase 5. Reads <=64KiB, writes <=32KiB per callback;
 * extent depth <=2, 4096 extent/node traversal budget, 64 metadata/data images
 * per operation. Excess credits fail before writes. RW has immediate allocation
 * and synchronous barriers, with no journal or crash-consistency guarantee.
 * create/mkdir, unlink/rmdir, regular-file rename without replacement, and
 * truncate-to-zero are supported. Directory rename and active-target deletion
 * return EOPNOTSUPP; replacement returns EEXIST without removing either name.
 * Nodes, including removed tombstones, persist for mount lifetime (1024 total).
 * RO media stays immutable; RW refreshes mappings after changes. Caller owns
 * the stable partition device throughout the mount and supplies external
 * PARTUUID/GPT/durability admission before calling mount_rw. All calls require
 * unlocked thread/boot context. Failed admission writes nothing and publishes
 * nothing. Dirty/recovery-needed volumes reject; RO never replays a journal. */
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
