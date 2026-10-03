# EXT4 Phase 7 — ordered journal writer and checkpoints

Implemented 2026-10-03 as an exclusive JBD2 workbench. This builds on the
[Phase-6 reader](ext4-phase6.md); it does not yet connect VFS mutations or enable
journaled production mounts. Phase 8 owns that integration and filesystem
RECOVER/clean-state/orphan transitions. Phase-5 Dell performance remains
unaccepted: the latest user-reported download is approximately 50 seconds per
1 MiB. Continuing development is explicitly authorized; this writer is not a
performance fix.

## Ownership, credits and state

The public interface lives in `src/fs/jbd2.h`, with the implementation in
`jbd2_write.inc` included by `jbd2.c` so it shares the reader's scalar byte/CRC
and sector-preserving I/O helpers.

```c
int jbd2_writer_open(block_dev_t *, const jbd2_source_t *, bool admitted,
                     jbd2_writer_t **);
int jbd2_writer_begin(jbd2_writer_t *, jbd2_credits_t);
int jbd2_writer_metadata(jbd2_writer_t *, uint32_t, const void *);
int jbd2_writer_data(jbd2_writer_t *, uint32_t, const void *);
int jbd2_writer_revoke(jbd2_writer_t *, uint32_t);
int jbd2_writer_abort(jbd2_writer_t *);
int jbd2_writer_commit(jbd2_writer_t *);
int jbd2_writer_checkpoint(jbd2_writer_t *);
void jbd2_writer_close(jbd2_writer_t *);
```

The caller exclusively owns the partition and serializes all calls until close.
The source map is copied; any metadata validation callback/context must outlive
the writer. All operations require unlocked thread context. No new spinlock,
scheduler wait, DMA lifetime or USB durability-policy change was introduced.
Admission is explicit and external: available callbacks alone do not establish
a device's power-loss durability.

Open performs the Phase-6 geometry/identity/feature/checksum validation and
requires an empty journal. Pending recovery returns ENOSPC without writes.
Begin reserves worst-case ring credits for all descriptors, payloads, revokes
and commit before disk mutation. Each transaction permits at most 64 metadata
images, 64 ordered-data images and 64 revokes. Multiple descriptor blocks are
supported when tags exceed a descriptor's capacity. One transaction is active;
the next begin cannot succeed until checkpoint retires the previous transaction.

Staging snapshots complete blocks, replaces duplicate images without another
credit, rejects metadata/data/revoke overlap and journal-data self-writes, and
returns EFBIG/ENOSPC before writes for credit/ring exhaustion. Metadata uses the
source's optional identity guard. The filesystem caller still owns ordered-data
allocation and all metadata coverage decisions. Staging allocation failures
leave the existing transaction abortable; abort/close write nothing.

States are IDLE, STAGING, COMMITTING, DURABLE, CHECKPOINTING and terminal FAILED.
Every commit/checkpoint read, write or flush failure poisons the writer. It does
not claim rollback: an uncertain commit may have reached stable media. Close
only discards memory; it never silently checkpoints, clears a tail or marks an
EXT4 volume clean. Fresh recovery is required after failure/restart.

## Durable write order

1. Write ordered data, then flush it when present.
2. Write descriptor/payload/revoke records and zero the future commit slot;
   flush these records before activating the log. This invalidates a possibly
   stale commit record before a reused circular slot can be exposed.
3. Publish the journal superblock's sequence/start and flush. A crash before
   activation sees an empty journal; after activation it sees durable valid
   records followed by a zero, uncommitted tail.
4. Write the checksummed commit record and flush. Successful return means
   DURABLE; metadata home blocks have not yet been written.
5. Checkpoint metadata images, then flush home blocks.
6. Clear the journal start, advance its head/sequence, and flush before allowing
   the ring to be reused.

The initial implementation checkpoints each transaction synchronously. It
supports circular placement, UINT32 sequence wrap, escaped payload bytes,
CSUM_V3 tags/descriptor/revoke/commit checksums and revoke-only transactions.
Earlier transactions are fully checkpointed before new space/block reuse;
there is no concurrent chain of uncheckpointed transactions. Ordered-data
overwrites are not promised atomic if a transaction does not commit.

The protocol follows the [Linux JBD2 format](https://www.kernel.org/doc/html/latest/filesystems/ext4/journal.html)
and [commit implementation](https://github.com/torvalds/linux/blob/master/fs/jbd2/commit.c).
The explicit activation/invalidation barriers favor a reviewable first writer;
future batching must preserve the same ordering and pass the crash gate.

## Verification and evidence limits

`make test-jbd2-write-host` compiles actual writer/reader/bootstrap code under
ASan/UBSan and operates only on newly generated regular images below
`build/jbd2-write`. Six block/sector geometries (1/2/4 KiB by 512/4096 bytes) are
tested at ordinary and circular/sequence-wrap starting positions: 12 cases.
Inputs are checksummed empty internal journals initialized with debugfs CSUM_V3;
the generator explicitly sets REVOKE, empty start/head and the disposable
filesystem's RECOVER bit. Production does not perform these transitions yet.

Each case stages a valid inode permission update, a journal image beginning
with JBD2 magic, ordered file data and a revoke. Before checkpoint, exact home
metadata must still match the original. After checkpoint/recovery, exact inode,
escaped file bytes and ordered data must match the independently expected new
images; revoked home bytes stay unchanged. Linux independently replays a durable
uncheckpointed output, a checkpointed output and a FortressOS-recovered output
for every case (36 copies), then runs `e2fsck -fn`, exact byte and mode audits.
Writer output requires no UUID normalization.

Every write/flush event is cut before and after under the Phase-6 disk model's
four stable/volatile persistence patterns. Fresh filesystem discovery and
recovery must produce the whole old or committed metadata set; committed
metadata requires the ordered data. A successfully returned commit must survive
any subsequent checkpoint interruption. Every failure blocks further writer
I/O, and recovered replay is idempotent. Sector-prefix tears must recover or
reject corruption without writes; they are not a promise to reconstruct arbitrary
torn journal metadata. Open/staging OOM and every open/read-modify-write read
failure are injected and ownership is audited.

Additional gates exercise 64 metadata + 64 data + 64 revoke credits, multi-block
descriptors, ring-full rejection, duplicate replacement/revokes, write-free
abort, invalid targets, blocked begin while durable, close-before-checkpoint,
revoke-only transactions and consecutive head/sequence reuse. The maximum-credit
gate uses generic home blocks and is a codec test, not an allocation/namespace
transaction coverage claim.

Logs retain numbered sector writes/flushes, exact argv, input/output images,
tool versions and output hashes. Timing and barrier counts are recorded for the
representative transaction; host CPU timing includes sanitizers, memory copies
and fixture output and is not USB/device latency. The full operation/crash
campaign and QEMU power cuts remain Phase 9. No native Linux-kernel power-cut,
USB or physical crash-consistency claim is made.

Commands actually run and passed:

| Command | Result / retained evidence |
| --- | --- |
| `make test-jbd2-write-host` | ASan/UBSan 12/12, 3,568 atomic cut/restart cases, sector tears, credits/reuse/read/OOM gates and 36 Linux replay/fsck/bytes/mode copies; `build/jbd2-write/run-7l5f447x` |
| `make test-jbd2-replay-host` | Phase-6 regression 12/12 plus 48 zero-write corruption rejections; `build/jbd2-replay/run-q88sb0oh` |
| `make test-ext4-write-host` | Six geometry VFS/persistence/failure and Linux byte/fsck audits; `build/ext4-phase4/host-glvhlrie` |
| `make bin/fortress.elf` then `make` | Strict kernel build/link and default ext2 image verification; no new warnings |

The representative transaction uses six logical flushes (one ordered-data,
three journal-commit, two checkpoint barriers). This baseline favors explicit
ordering over batching and does not resolve the reported Dell throughput.
Earlier incomplete fixture-initialization runs are retained but are not passes.
