# PKG-2 private inode and orphan grant lifecycle

Candidate host workbench only, 2026-10-21. Extends the
[filesystem adapter](PKG2_FILESYSTEM_GRANT_ADAPTER.md); does not implement or
enable production mount admission, installation or trust provisioning.

## Private integration boundary

The runner copies source into a retained workspace, verifies source hashes, then
makes exactly two explicit overlays in that copy: rename the original semantic
commit implementation so a private wrapper can stage grant metadata before it;
call a private reconstruction validator before constructor orphan cleanup. The
original journal writer/replay, allocation, namespace, orphan and VFS lifetime
implementations remain in use. Original/overlay hashes and exact replacements
are recorded. There is no shared-tree production hook or new syscall.

Constructor cleanup receives its engine explicitly. It cannot use the mounted
adapter's `fs->engine`, which is not published yet. The private helper stages
fixed record blocks through that explicit engine; it never fakes trust/mounted
flags or temporarily publishes a root. Existing read-only ownership preview
still precedes actual replay. Grant validation here occurs after filesystem replay
and journal activation, before orphan cleanup; grant-aware preview before the
first replay write is NOT implemented. This does not provide an independently
authenticated enrollment selector or a production early-grant policy.

## Narrow lifecycle profile

One fixed transaction directory holds one role-1 regular file `unit`. The grant
is eight blocks/one inode; this is a synthetic fixture bound. Baseline metadata
and directory storage are prepared first. New target inode selection comes from
the actual allocation bitmap; namespace create supplies root ownership and 0600.

The wrapper observes the real planner's staged inode, inode bitmap and superblock.
Charged blocks derive from actual i_blocks, and charged inode count from its
allocated bit. The complete filesystem ownership audit and checksum-checked map
traversal independently refine those claims between operations and at recovery.
This profile rejects unexpected free-counter deltas (including parent growth)
before I/O. It does not provide general typed-operation authorization or account/
ordinary-writer admission. Unrelated orphan frees would also violate this narrow
delta profile and refuse; mixed baseline/tagged orphan recovery is not supported.
All test operations are serialized host calls on one regular data file, not a
running command binary.

Each transaction writes the remaining escrow, charged counts, sequence, role
class/flags, size and record hashes as journal metadata images in the SAME semantic
commit as allocation, links, orphan-chain, inode and bitmap changes. Existing
credit ceilings still apply. Sequence wrap and an over-budget planned object
refuse; these are private accounting checks, not a frozen production disk ABI.

States exercised:

1. Empty ownership table; the grant has one unused inode credit.
2. Real namespace creation allocates the inode and consumes that credit.
3. A real write allocates a block and consumes one block credit.
4. Unlink commits name removal, zero links and orphan membership; a retained
   descriptor keeps all allocations charged. A duplicate reference delays close.
5. Final close or restart reclaims blocks in existing orphan steps, refunding
   only the blocks actually freed in each committed step.
6. Orphan detach and actual inode-bitmap release refund the inode once. A RETIRED
   entry retains role/inode/generation but has zero size/allocations/flags.

Refund restores escrow, not ordinary free headroom. These fixtures remain
RESERVED with T=(8,1); clearing tombstones, releasing unused escrow and publishing
IDLE/new-writer admission are later protocol work.

Reconstruction distinguishes linked, orphan, absent and retired states using
actual directory references, chain membership, inode bitmap/generation and map
ownership. Deleted orphans can legitimately retain old logical size while their
right-edge extents are reclaimed; size alone is not the charge. Tombstones require
the inode to remain free with the retained generation in this private profile.
Ordinary reuse of such an inode needs a separately reviewed retirement policy.

## Acceptance boundary

The campaign cuts before/after each write/flush callback under cached/all/
alternating/inverse persistence for create, pinned unlink, final close and whole
constructor orphan recovery. Every failed attempt is followed by two complete
mount cycles. Create/unlink may choose a coherent old/new outcome; last-close
and already-committed orphan recovery must end fully refunded. A repeated cleanup
after refund must issue zero writes/flushes. Live pinned reads check original
bytes after unlink; the extra file reference must produce no cleanup I/O.
The stdout `commits` counter counts acknowledged successful semantic returns,
not all durable commits: an uncertain error can still have committed on disk.

Linux e2fsck and debugfs audit retained completed/recovered snapshots; they do not
inspect every transient callback-cut image. Sector callbacks are atomic in this
model; no physical device, firmware, Ring 3, SMP/IRQ or sector-tear claim follows.
Each case injects one failure followed by two complete mounts; exhaustive chains
of successive interrupted recovery attempts are not covered.
The fixture's external identity context survives host filesystem resets; this
does not establish a fresh-process trusted-boot identity source.

Run `make -f tests/pkg_grant_candidate.mk test-pkg-grant-lifecycle` in WSL.
Exact results, retained failures and source-preservation qualifications are in
[the roadmap](../roadmap/pkg1.md). Installation, production trust provisioning,
release builds, pushes and physical writes remain disabled.
