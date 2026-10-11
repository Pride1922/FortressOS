#ifndef FORTRESS_EXT4_H
#define FORTRESS_EXT4_H

#include "block.h"
#include "vfs.h"
#include "jbd2.h"
#include "ext4_physical_fixture.h"
#include "account_redo.h"

/* Trusted devfs read adapter, no locks held on entry. handled distinguishes
 * no boot-published EXT4 mount from a handled I/O error (never retry errors).
 * Mount selection is boot-only; this is not a hot-mount/transport API. */
bool ext4_device_read_sector(block_dev_t *,uint64_t,void *,bool *handled);
/* Passive installer exclusion query. Includes a detached/preparing owner. */
bool ext4_storage_owned(void);

/* E4-A restricted-profile mounts at /mnt. Production USB dispatch uses the existing
 * explicit PARTUUID, GPT and durability admission policy. Reads <=64KiB, writes <=32KiB per callback;
 * extent depth <=2, 4096 extent/node traversal budget, 64 metadata/data images
 * per operation. Excess credits fail before writes. RW has immediate allocation
 * and synchronous barriers, with no journal or crash-consistency guarantee.
 * create/mkdir, unlink/rmdir, regular-file rename without replacement, and
 * truncate-to-zero are supported. Directory rename and active-target deletion
 * return EOPNOTSUPP; replacement returns EEXIST without removing either name.
 * RW owns an eight-block clean metadata cache under the filesystem lock.
 * The mounted device requires exclusive write ownership; external/raw changes
 * require remount. Cache hits retain all metadata checksum/structure checks.
 * E4-A nodes persist for mount lifetime (1024 total). Journaled E4-B retains
 * at most 1024 cached nodes: owned VFS references/open handles protect nodes;
 * unpinned tombstones are reclaimed and idle leaf nodes evicted at capacity.
 * Legacy raw lookup/create results stay stable for mount lifetime, so callers
 * must use the owned VFS API for reclaimable nodes. Cached descendants retain
 * parent addresses. Fully pinned capacity reports EFBIG, never false ENOENT.
 * RO media stays immutable; RW refreshes mappings after changes. Caller owns
 * the stable partition device throughout the mount and supplies external
 * PARTUUID/GPT/durability admission before calling mount_rw. All calls require
 * unlocked thread/boot context. Failed admission writes nothing and publishes
 * nothing. Dirty/recovery-needed volumes reject; RO never replays a journal. */
typedef struct ext4_mount ext4_mount_t;
#ifdef FORTRESS_EXT4_COMMIT_PAUSE_TEST
/* Disposable test only; callback runs under mounted exclusion after durable
 * commit, before checkpoint. No device I/O, scheduling or filesystem calls. */
int ext4_test_arm_commit_pause(ext4_mount_t *mount,void (*pause)(void));
#endif
/* Boot-only trusted-root constructor. Caller admits exclusive recovery/write
 * policy BEFORE entry. No namespace publication; owned lookups validate detached
 * root before publish. Module/bootstrap backing survives for the entire boot.
 * Discard requires all validation references closed; does not undo replay writes. */
#ifdef TEST_ROOT_PHASE1
void ext4_test_snapshot_fault(unsigned fault);
#endif
int ext4_prepare_root(block_dev_t *,bool admitted,ext4_mount_t **);
int ext4_prepare_root_diagnose(block_dev_t *,bool admitted,ext4_mount_t **,const char **stage);
/* Trusted installer handoff only: caller suspends all raw lease operations and
 * owns the target exclusively until freeze/discard. This detached namespace can
 * never be published as the running root and does not alter boot preparation. */
int ext4_prepare_installer_root(block_dev_t *,bool exclusive,ext4_mount_t **,const char **stage);
vfs_node_t *ext4_prepared_root(ext4_mount_t *);
int ext4_publish_root(ext4_mount_t *);
int ext4_discard_prepared_root(ext4_mount_t *);
/* Phase2 boot-only pin of immutable account policy. No mutable authentication
 * until the Phase3 coherent reader/writer service replaces this exclusion. */
int ext4_root_lock_accounts(ext4_mount_t *,vfs_node_t *nodes[4]);
/* Trusted account service only. Fixed-role callbacks bypass account inode
 * immutability only inside each filesystem operation, with no global bypass.
 * Caller serializes readers/writers and supplies database validation callback. */
int ext4_root_account_io(ext4_mount_t *,account_io_t *);
int ext4_root_account_headroom(ext4_mount_t *);
int ext4_sync_installed_root(bool *handled);
int ext4_freeze_installed_root(bool *handled);
/* After successful freeze: excludes all filesystem/raw reads, then invokes the
 * polling controller stop under the same exclusion. No reopen in this boot. */
int ext4_quiesce_installed_root(bool (*stop)(void));

int ext4_mount_ro(block_dev_t *partition, const char *path, ext4_mount_t **out);
int ext4_mount_rw(block_dev_t *partition, const char *path, ext4_mount_t **out);
/* Production bounded E4-B RW mount. Caller explicitly admits exclusive writes
 * and recovery after target/GPT/durability checks and successful flush preflight.
 * Uses the same validated replay, orphan cleanup and transactional lifecycle
 * as the accepted fixture. No implicit admission or format conversion. */
int ext4_mount_journal_rw(block_dev_t *partition,const char *path,ext4_mount_t **out);
/* Phase 8.5 test admission only. Never used by production USB dispatch.
 * Caller owns an explicit disposable partition exclusively and admits both
 * recovery writes and RW durability. Validate replayed ownership/namespace
 * before recovery I/O, recover orphans, durably activate RECOVER, then publish.
 * Rejection before admitted recovery writes nothing; I/O failure can leave a
 * recoverable prefix, never a published mount. Existing E4-A admission stays
 * unchanged. Bounds are the Phase 8.4 profile; no orphan_file/hard links. */
typedef struct { bool disposable_fixture, writable, recovery; } ext4_journal_admission_t;
int ext4_mount_journal_fixture(block_dev_t *partition,const char *path,
                               ext4_journal_admission_t admission,ext4_mount_t **out);
/* Same fixture mount and cleanup, with optional caller-owned diagnostic output.
 * stage receives a static string naming the last attempted step, or "mounted"
 * on success. No logging, extra I/O, allocation or policy override is added. */
int ext4_mount_journal_fixture_diagnose(block_dev_t *partition,const char *path,
    ext4_journal_admission_t admission,ext4_mount_t **out,const char **stage);
/* Existing SYS_SYNC / shutdown routing fallback for the explicit fixture
 * mount only. No fixture: sync returns EROFS; shutdown succeeds as a no-op. */
int ext4_sync_journal_fixture(void);
int ext4_freeze_journal_fixture(void);

/* Mid-session: commit/checkpoint (when journaled), then device barrier.
 * Does not freeze or mark clean. Failed barrier taints the mount. */
int ext4_sync(ext4_mount_t *mount);

/* Bounded diagnostic snapshot; unlocked thread context, no media I/O. */
size_t ext4_io_profile_format(char *out,size_t capacity);

/* Shutdown-only: stop new mutations, drain, barrier, then mark clean only
 * if healthy. A failure leaves the mount frozen and never reports clean.
 * Uncertain I/O taints; staging failure or pinned orphans permit a later retry.
 * Mount objects/VFS nodes stay alive; no unmount/lifetime change is implied. */
int ext4_freeze_and_sync(ext4_mount_t *mount);

/* Phase-6 exclusive recovery workbench: derive internal journal identity/map
 * from disk, analyze without writes or VFS publication. Production mounts still
 * reject journals. Caller exclusively owns disposable/eligible partition;
 * jbd2_replay requires explicit admission and does not clear ext4 RECOVER. */
int ext4_journal_analyze(block_dev_t *partition,jbd2_plan_t **out,
                         jbd2_report_t *report);

#endif
