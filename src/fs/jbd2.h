#ifndef FORTRESS_JBD2_H
#define FORTRESS_JBD2_H
#include "block.h"

#define JBD2_MAP_MAX 32768u
#define JBD2_IMAGES_MAX 1024u
#define JBD2_REVOKES_MAX 1024u
#define JBD2_TX_MAX 128u
typedef struct jbd2_plan jbd2_plan_t;
typedef struct {
    uint32_t block_size, filesystem_blocks, journal_blocks;
    const uint32_t *journal_map; /* Complete logical -> partition block map. */
    uint8_t uuid[16];
    /* Optional filesystem identity guard, called only during analysis. */
    bool (*validate_home)(void *context,uint32_t block,const uint8_t *bytes);
    void *context;
} jbd2_source_t;
typedef struct {
    uint32_t transactions, images, revokes, replayed, revoked, next_sequence;
    bool incomplete_tail;
} jbd2_report_t;
/* Exclusive, stable partition ownership is required from analysis to release.
 * Analysis is read-only, snapshots all committed payloads, and rejects unknown
 * features/corruption before writes. Profile: v2 superblock, CSUM_V3 + REVOKE,
 * 32-bit tags, no async/fast commit or external journal. Bounded resources.
 * Caller provides a validated journal inode map/UUID (ext4_journal_analyze
 * derives these from disk). No VFS publication or implicit mount recovery. */
int jbd2_analyze(block_dev_t *partition,const jbd2_source_t *source,
                 jbd2_plan_t **out,jbd2_report_t *report);
/* Explicit writable admission only. Checkpoints committed, unrevoked images,
 * flushes home blocks, then empties the journal and flushes its superblock.
 * Failure poisons this plan; retry requires fresh analysis after restart.
 * Does not clear ext4 RECOVER, clean-state or orphan metadata (Phase 8). */
int jbd2_replay(jbd2_plan_t *plan,bool writable_admitted);
void jbd2_release(jbd2_plan_t *plan);

#define JBD2_WRITE_IMAGES_MAX 64u
typedef struct jbd2_writer jbd2_writer_t;
typedef struct { unsigned metadata, ordered_data, revokes; } jbd2_credits_t;
typedef enum {
    JBD2_WRITE_IDLE, JBD2_WRITE_STAGING, JBD2_WRITE_COMMITTING,
    JBD2_WRITE_DURABLE, JBD2_WRITE_CHECKPOINTING, JBD2_WRITE_FAILED
} jbd2_write_state_t;
/* Exclusive workbench only: caller serializes ALL calls and owns a stable
 * partition/source context until close. Open validates an empty journal and
 * requires external writable/durability admission. No filesystem state bits
 * or VFS operations are changed. Recover a nonempty journal before opening.
 * Source validate_home applies to metadata images; ordered-data ownership and
 * allocation correctness are the filesystem caller's responsibility. */
int jbd2_writer_open(block_dev_t *partition,const jbd2_source_t *source,
                      bool writable_admitted,jbd2_writer_t **out);
void jbd2_writer_close(jbd2_writer_t *writer);
jbd2_write_state_t jbd2_writer_state(const jbd2_writer_t *writer);
int jbd2_writer_begin(jbd2_writer_t *writer,jbd2_credits_t credits);
/* Block-sized snapshots; duplicate images replace staged bytes without
 * consuming another credit. Metadata/data/revoke overlap is rejected. */
int jbd2_writer_metadata(jbd2_writer_t *writer,uint32_t block,const void *bytes);
int jbd2_writer_data(jbd2_writer_t *writer,uint32_t block,const void *bytes);
int jbd2_writer_revoke(jbd2_writer_t *writer,uint32_t block);
int jbd2_writer_abort(jbd2_writer_t *writer); /* Staging only, zero disk writes. */
/* Commit flushes ordered data, log images, active tail, then commit record.
 * DURABLE success does not imply checkpoint. Home metadata is untouched until
 * checkpoint; any uncertain I/O poisons the writer, never claims rollback.
 * Close discards memory only; committed logs remain recoverable. */
int jbd2_writer_commit(jbd2_writer_t *writer);
int jbd2_writer_checkpoint(jbd2_writer_t *writer);
#endif
