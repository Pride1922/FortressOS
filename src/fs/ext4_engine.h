#ifndef FORTRESS_EXT4_ENGINE_H
#define FORTRESS_EXT4_ENGINE_H
#include "block.h"
/* Internal Phase-3 workbench, not a VFS RW mount. Caller must exclusively own
 * disposable/eligible media; never share with a mounted filesystem.
 * One staged operation, 64 metadata images, <=64KiB newly zeroed data per grow.
 * All functions require unlocked thread context; commit may disable IRQs for
 * bounded synchronous I/O. No namespace operations or journal/crash guarantee.
 * Successful staging writes nothing; outputs remain provisional until commit.
 * Any commit I/O/barrier failure taints the context permanently. Abort only
 * drops an uncommitted plan; it cannot undo a possibly persisted commit.
 * Close never marks clean. Finish marks clean only after successful commits
 * and zero outstanding reserved inodes. Phase 4 owns namespace publication. */
typedef struct ext4_engine ext4_engine_t;
int ext4_engine_open(block_dev_t *dev, ext4_engine_t **out);
void ext4_engine_close(ext4_engine_t *engine);
int ext4_engine_grow(ext4_engine_t *engine, uint32_t ino,
                     uint32_t logical, uint32_t count);
/* Remove complete logical blocks at/above keep. Partial-block truncate and
 * unwritten conversion belong to Phase 4; this does not delete an inode. */
int ext4_engine_trim(ext4_engine_t *engine, uint32_t ino, uint32_t keep);
int ext4_engine_reserve_inode(ext4_engine_t *engine, uint32_t *ino);
int ext4_engine_reserve_inode_kind(ext4_engine_t *engine, bool directory, uint32_t *ino);
int ext4_engine_release_inode(ext4_engine_t *engine, uint32_t ino);
int ext4_engine_commit(ext4_engine_t *engine);
void ext4_engine_abort(ext4_engine_t *engine);
int ext4_engine_finish(ext4_engine_t *engine);
#endif
