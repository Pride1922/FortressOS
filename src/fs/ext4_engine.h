#ifndef FORTRESS_EXT4_ENGINE_H
#define FORTRESS_EXT4_ENGINE_H
#include "block.h"
#include "jbd2.h"
/* Internal Phase-3 workbench, not a VFS RW mount. Caller must exclusively own
 * disposable/eligible media; never share with a mounted filesystem.
 * One staged operation, 64 metadata images, <=64KiB newly zeroed data per grow.
 * Standalone workbench calls require unlocked thread context; commit may disable IRQs for
 * bounded synchronous I/O. No namespace operations or journal/crash guarantee.
 * Successful staging writes nothing; outputs remain provisional until commit.
 * Any commit I/O/barrier failure taints the context permanently. Abort only
 * drops an uncommitted plan; it cannot undo a possibly persisted commit.
 * Close never marks clean. Finish marks clean only after successful commits
 * and zero outstanding reserved inodes. Phase 4 owns namespace publication.
 * Phase 8.5 privately reuses journal file-write/namespace/orphan helpers and
 * abort under the mounted EXT4 rank-1 IRQ-save lock. The owner assertion selects
 * that contract only for the private mounted engine; constructors, close and
 * exclusive workbench handles remain unlocked. Legacy direct-home mutators
 * remain unavailable on an engine with a writer. */
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
/* Phase 8.1 exclusive transaction workbench, never a production/VFS mount.
 * Explicit durability admission; discovers and owns the journal guard/map.
 * Caller serializes every call, including close, from unlocked thread context.
 * Only presealed block-image transactions are enabled; grow/trim/inode and
 * finish remain disabled pending operation and mount integration (8.2-8.5).
 * One transaction: <=64 combined metadata/data snapshots, <=64 revokes.
 * Begin reserves worst-case ring credits before staging; duplicate replacement
 * consumes no credit. Any staging error latches until abort, with zero writes.
 * Commit synchronously checkpoints; cache/descriptor publication follows all
 * barriers. Uncertain I/O taints permanently. Close never writes/marks clean. */
int ext4_engine_open_journal(block_dev_t *dev, bool admitted, ext4_engine_t **out);
int ext4_engine_transaction_begin(ext4_engine_t *engine, jbd2_credits_t credits);
int ext4_engine_transaction_metadata(ext4_engine_t *engine, uint32_t block, const void *bytes);
int ext4_engine_transaction_data(ext4_engine_t *engine, uint32_t block, const void *bytes);
int ext4_engine_transaction_revoke(ext4_engine_t *engine, uint32_t block);
/* Phase 8.2 file workbench: empty supported journal, existing RECOVER bit,
 * consistent checksummed allocation ownership. Same exclusive single-caller
 * contract; no VFS publication, namespace, truncate, orphan or clean-state API.
 * Existing regular singly-linked files only. <=32KiB per write, 64 combined
 * data/metadata images, 64 old-tree revokes. Append reads authoritative EOF;
 * *offset advances only after durable commit/checkpoint. Shared handles use
 * the same offset pointer; independent handles have independent pointers.
 * Atomic metadata, ordered exposed data; overwritten data is NOT atomic on
 * crash. Staging failures write nothing; uncertain I/O permanently taints.
 * Raw image transactions are disabled in this mode. Caller seals no metadata. */
int ext4_engine_open_journal_files(block_dev_t *dev, bool admitted, ext4_engine_t **out);
int64_t ext4_engine_file_write(ext4_engine_t *engine, uint32_t ino, uint64_t *offset,
                               bool append, const void *data, size_t len);
/* Phase 8.3 exclusive namespace workbench, extending the 8.2 admission above.
 * No VFS mount/handles may coexist. Caller serializes every call and owns media
 * exclusively; open-target deletion and orphan/restartable cleanup are 8.4.
 * One transaction per operation, <=64 combined images and <=64 freed blocks.
 * Create/mkdir, regular-file rename without replacement, closed single-link
 * unlink and empty rmdir only. Directory moves/replacement reject unchanged.
 * Parent links, directory checksums, inode/bitmap/counter ownership and bounded
 * reclamation commit together. Output inode changes only after durability.
 * Unsupported/over-budget plans write nothing. I/O failure permanently taints.
 * Production journaling and clean-state transitions remain disabled. */
int ext4_engine_open_journal_namespace(block_dev_t *dev, bool admitted, ext4_engine_t **out);
int ext4_engine_namespace_create(ext4_engine_t *engine, uint32_t parent,
                                 const char *name, bool directory, uint32_t *ino);
int ext4_engine_namespace_remove(ext4_engine_t *engine, uint32_t parent,
                                 const char *name, bool directory);
int ext4_engine_namespace_rename(ext4_engine_t *engine, uint32_t old_parent,
                                 const char *old_name, uint32_t new_parent, const char *new_name);
/* Phase 8.4 exclusive workbench; no production mount, sync or clean claim.
 * Admission validates the entire traditional orphan chain (<=64), allocation
 * ownership and absence of directory references to deleted orphans before writes.
 * Workbench bounds: <=1,048,576 blocks, <=65,536 inodes, <=1,024 directories; rmdir only
 * a single dot block. Larger volumes/multi-block directories reject unchanged.
 * Caller must replay JBD2 first, then explicitly recover before normal work.
 * Shrink only: publish size + orphan intent atomically, then reclaim rightmost
 * extents in <=32-data-block steps, with <=2 tree-node revokes and no allocation.
 * A failed later step leaves durable intent; staging errors do not taint, I/O
 * uncertainty does. Recovery/last close retry cleanup; close(engine) writes
 * nothing. Partial-tail zeroing is journaled with size, preserving old bytes
 * if intent fails to commit. orphan_file and multi-link files remain excluded.
 * Sixteen independent handles; dup shares offset/lifetime. Tokens are scoped
 * to this engine. Final close consumes the handle even if cleanup fails; the
 * orphan remains recoverable. Unlink commits the name/link/orphan together;
 * allocations live until last close. Restart has no surviving handles. */
int ext4_engine_open_journal_orphans(block_dev_t *dev, bool admitted, ext4_engine_t **out);
int ext4_engine_orphan_recover(ext4_engine_t *engine);
int ext4_engine_file_truncate(ext4_engine_t *engine, uint32_t ino, uint64_t size);
int ext4_engine_orphan_unlink(ext4_engine_t *engine, uint32_t parent, const char *name, bool directory);
int ext4_engine_handle_open(ext4_engine_t *engine, uint32_t ino, uint64_t *handle);
int ext4_engine_handle_dup(ext4_engine_t *engine, uint64_t handle);
int ext4_engine_handle_close(ext4_engine_t *engine, uint64_t handle);
int64_t ext4_engine_handle_read(ext4_engine_t *engine, uint64_t handle, void *bytes, size_t len);
int64_t ext4_engine_handle_write(ext4_engine_t *engine, uint64_t handle, bool append, const void *bytes, size_t len);
int ext4_engine_handle_truncate(ext4_engine_t *engine, uint64_t handle, uint64_t size);
#endif
