# Private durable account generation and atomic origin slot

2026-10-23. Actual EXT4/JBD2 primitive workbench following
[the enrollment codec gate](PKG2_ENROLLMENT_CODEC.md). Production integration,
installation, trust provisioning, release-media builds, pushes and physical
disk writes remain disabled. No shared production source is edited.

The runner copies sources, applies exact private planner/preview hooks and uses
the actual filesystem engine, VFS and JBD2 writer/replay implementation with the
existing sector callback fault adapter. A synthetic account file occupies the
real `/etc/shadow` pathname in the disposable fixture. Its bytes are not an
authenticated account database; account-service BEGIN/UPDATE/redo integration
and pending/trust coherence remain later gates.

## Durable records and generation ownership

The previously defined 1024-byte origin slot is paired with a separate
preallocated 1024-byte account-generation file. The fixture initially creates
both while unenrolled, then exports an actual inode/map enrollment body. Python
cryptography/OpenSSL signs that body with public deterministic test keys and
supplies an independent full-descriptor pin. Every subsequent process verifies
the descriptor through the existing private signature gate before mounting.
No private key enters the filesystem or the C verifier. This is private fixture
enrollment, not implementation of the production boot selector or provisioning.

The descriptor binds all sixteen real named inode/generation/map locators,
filesystem UUID and geometry. Preview compares actual names, root ownership,
mode, size, initialized inline maps and block counts. The full constructor
allocation/reference audit still runs. Neither a trusted native `.origin`
sidecar nor an orphan-derived expected identity is supplied.

Account-generation bytes are canonical unsigned little endian:

| Offset | Encoding |
| --- | --- |
| 0 | 16-byte `FORT-ACC-GEN-v1` domain including terminal zero |
| 16 | u32 length=1024, u16 version=1, u16 phase (IDLE=0/OPEN=1) |
| 24 | u64 slot sequence |
| 32 | u64 account-operation generation |
| 40 | u64 enrollment epoch |
| 48 | Enrollment UUID[16], filesystem UUID[16], account-profile hash[32] |
| 112 | SHA-256 of the complete 1024-byte origin record |
| 144 | 848 zero bytes |
| 992 | SHA-256 of bytes [0,992) |

The state provides the independent expected sequence/generation used by the
origin decoder. The origin does not hash the generation record, so there is no
cyclic digest dependency. Both checksums detect corruption, not arbitrary offline
rewrites or complete-filesystem rollback. Mutable authority remains dependent on
exclusive kernel ownership and enforced write exclusion; production enforcement
is not supplied by this host-only dispatcher.

Initial state is IDLE, sequence 1, generation 1, with an EMPTY origin. Starting
an operation requires IDLE plus EMPTY and no orphan. It increments generation,
increments sequence and rebinds the EMPTY origin in one journal commit. An OPEN
operation survives owner loss; cleanup uses its existing generation. Opening a
second operation while OPEN or ACTIVE refuses. Ending requires OPEN plus EMPTY
and no orphan, advances sequence and publishes IDLE without advancing generation.
These are private primitives, not public syscalls or a package writer lease.
The generation identifies a durable recovery operation; it does not replace
the account service's credential snapshot/authentication generation protocol.
Counters never wrap. Preview and live BEGIN conservatively reserve room for
four remaining pair transitions; a boundary test finishes the last admitted
operation and then requires the next BEGIN to refuse before I/O. Exhausted
roots refuse subsequent admission rather than silently resetting counters.

## Atomic planner updates

The copied `ext4_engine_orphan_unlink` hook captures the actual canonical shadow
inode, parent, generation, size and original extent/allocation maps under the
same planner/exclusion as unlink. It activates the origin and updates the
generation record's sequence/digest before that planner's semantic commit.
The directory entry removal, zero-link inode and orphan-chain insertion therefore
share a JBD2 commit with both records. There is no separate ARMED state or
post-callback witness write.

Right-edge reclamation leaves the original ACTIVE witness unchanged. Remaining
maps must match its logical/physical prefixes and original physical allocations;
the global audit proves unique ownership. The final orphan-detachment/inode-free
planner clears the slot and updates the generation record together. The two
record data blocks are journal metadata images, not ordered independent writes.
The semantic hook requires both record images when staging a pair and refuses
ordinary attempts to journal either protected record through that boundary.
Journal image/credit rejection precedes device writes; no split-commit fallback.

Constructor preview validates the committed replay view of names, maps and both
records before actual replay, superblock activation, orphan cleanup or publication.
ACTIVE requires exactly the recorded orphan, a vacant canonical role and OPEN
operation. EMPTY with an orphan, a stale pairing, malformed fields, unknown
orphan, changed identity/map or counter exhaustion fails closed. Missing records
are not recreated. No mount ownership flags or free counters are fabricated.

This first primitive profile admits only unlinked canonical shadow, pre-intent
phase, up to 8192 bytes. It does not admit multiple/linked/charged orphans, general
ordinary namespace mutation, arbitrary account paths or a complete pending redo
policy. Partial cleanup and constructor cleanup use the actual unchanged orphan
algorithm. Account-capacity/grant accounting remains the earlier separate work;
this campaign does not claim integration with those adapters or near-full progress.

## Verification scope

`scripts/test_pkg_account_durable.py` enumerates actual callback writes/flushes
for generation begin/end, pinned unlink, final-close cleanup and constructor
recovery. It cuts before/after each callback under cached/all/alternating/inverse
sector persistence, then restarts twice. The second restart must leave the pair's
sequence/generation unchanged. A counter records preview states newer than their
home generation block to require committed-uncheckpointed recovery coverage.
`--jobs 1..8` controls independent persistence-profile host processes (default 8).
Each has its own private RAM disk and cut loop. All eight profiles must report
the same measured event counts and collectively cover all before/after cuts;
this is not concurrent mutation of one filesystem. No audit is disabled.

The runner also starts fresh processes from ACTIVE and partially reclaimed images
and recovers each twice without an origin sidecar. Linux e2fsck and exact debugfs
reads audit selected final images: independent Python bytes for both records,
absent shadow, unchanged unrelated payload and protected fixture contents.
Linux audits are not run for every cut. Negative records are resealed to isolate
semantic checks, then require zero underlying writes/flushes, no published mount,
released constructor ownership and unchanged image bytes.
Direct ordinary VFS attempts to overwrite/unlink the two records, competing
BEGIN and END while ACTIVE also require rejection without I/O. Source guards
record unrelated concurrent repository changes separately from strict checks
on compiled repository dependencies, all copied inputs and canonical media.

Fault-adapter callbacks model sector-atomic writes and flush persistence. They
do not prove torn-sector, hardware/controller ordering, physical power-loss or
uncertain-I/O retry safety. In-process restarts drain host objects; separate fresh
process recoveries establish disk-only provenance in this bounded fixture scope.
No new scheduler, signals, Ring 3, QEMU boot or physical acceptance is implied.
Exact measured results and retained failures belong in the roadmap after testing.

The next private layer binds the pair to the actual account service and redo
protocol; see [account-service integration](PKG2_ACCOUNT_SERVICE_DURABLE.md).
