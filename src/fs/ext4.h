#ifndef FORTRESS_EXT4_H
#define FORTRESS_EXT4_H

#include "block.h"
#include "jbd2.h"
#include "ext4_physical_fixture.h"

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
 * Nodes, including removed tombstones, persist for mount lifetime (1024 total).
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
