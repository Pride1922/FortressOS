# PKG-2 private filesystem and journal adapter

2026-10-21. Test-only refinement of
[the candidate grant records](PKG2_DURABLE_GRANT_DESIGN.md). Adapter code lives
under `tests/` and includes actual EXT4/VFS/JBD2 in the existing host workbench.
There are no new production calls, disk ABIs, syscalls, mount hooks, installation
paths or trust provisioning.

## Filesystem refinement

The fixture owns one journaled EXT4 regular-file image exclusively. Its explicit
test context pins filesystem UUID, synthetic installation/repository/enrollment
identities, marker hash and fixed inode/generation identities. These synthetic
identities are not production trust. It locates records and one payload by name,
validates record cross-bindings, then uses actual checksum-checked inode/extent/
bitmap readers and the existing whole-filesystem orphan/ownership audit to refine
their claims. That audit includes unrelated inodes and fixed filesystem/journal
blocks; pairwise package-record uniqueness cannot hide an unrelated owner alias.

Validation checks actual generations, UID/GID, exact modes, link counts, sizes,
initialized dense maps, i_blocks and allocated data plus external tree blocks.
Fixed metadata maps are inline, at most four extents and exactly rounded to EOF.
Fixed locators match pinned inodes; payload identity matches its role path.
Inspection publishes its view only after all checks pass. Refusal preserves
output and issues no write/flush callback. Heap/read failures are refusals.

The integration layout is narrower than the record codec: one linked CHARGED
payload role and no holes/unwritten extents. Orphan, victim, tombstone and other
layouts refuse. Initial payload inode allocation is fixture preparation, not a
tested atomic grant operation. The journal experiment allocates one data block
and then truncates to zero, refunding exactly that reclaimed block. The inode
remains charged. Eight blocks/one inode and the one-unit headroom check are
artificial test bounds, not production R_acc/account-floor admission.

Traversal counts external extent-tree allocation, but the positive fixture uses
inline extents; fragmented/deep-map coverage remains needed. This does not certify
complete package namespaces, parent growth, immutable maps across hostile writers,
authorization, ownership transfer, competing allocators, pins or async lifetimes.

## Journal coupling and fault model

The adapter reuses actual file allocation/truncate planners, stages ledger/table
file blocks as `E4_REFERENCE` journal metadata images, and uses existing semantic
commit/credit calculation. Bitmaps, counters, payload inode and grant records
share that bounded transaction. Record updates are not ordered file-data writes.
No credit ceiling is raised and no production fault hook/raw writer is added.

The operation campaign cuts before/after every write/flush callback with four
existing persistence modes: cached, all accepted sectors persistent, alternating
and inverse alternating. Every cut gets two successful mount/recovery/validation
cycles. This is a host sector-atomic model, not sub-sector tearing or physical
device acceptance. The operation gate checks valid coupled old/new accounting;
the replay gate additionally requires the exact committed sequence/outcome.

The replay campaign finds a fully committed transaction before its home checkpoint
finishes, then interrupts actual `jbd2_replay` at every write/flush callback under
the same modes. Two later mount cycles must retain the committed sequence and
grant totals. This tests replay restartability, not production grant reconstruction
before constructor orphan cleanup or authenticated mount-failure routing.

Actual C readers check invariants after every cut. Independent Linux e2fsck and
debugfs apply to retained completed/restored/recovered snapshots, not every
transient cut image. The debugfs oracle checks payload bytes, hashes, sequence/
charges and actual inode generation, mode, owner, size and block count. Selected
snapshot checks must not be described as a byte oracle for every cut.

## Finding: recovery identity geometry

A 16 MiB, 4 KiB, single-block-group filesystem reproduced a real failure after
replay interruption: charge, all accepted sectors persistent, cut-before callback
27, constructor stage `journal-source`, EIO. The transaction had committed;
partial home superblock checkpoint left a checksum mismatch. Existing bootstrap
requires a checksummed group-1 backup corroborating immutable identity fields;
this geometry has no group 1. The failure image and diagnostics are retained.

Production EXT4 is unchanged. The private grant profile now refuses fewer than
two groups and checks group-1 superblock/descriptor backup checksums and immutable
identities before grant preparation. Ordinary legacy mount support is not narrowed.
Future package admission must enforce equivalent eligibility or separately review
recovery for this geometry. Larger fixtures do not resolve or erase the original
failure. Single-group and corrupt-backup refusals explicitly test the exclusion.

Review also replaced an overflowing `G + floor` comparison in this new private
validator with subtraction after `free >= G`. An empty-payload UINT64_MAX escrow
case tests the corrected refusal. Production allocation policy is unchanged.

## Running and preservation

These standalone targets never include release build rules:

```
make -f tests/pkg_grant_candidate.mk test-pkg-grant-fs
make -f tests/pkg_grant_candidate.mk test-pkg-grant-fs-negative
make -f tests/pkg_grant_candidate.mk test-pkg-grant-fs-recovery
```

Negative/recovery runners retain generated C with alternate entry points; they
do not rewrite shared C while another campaign runs. Compiler dependencies
distinguish compiled-input changes from unrelated concurrent repository work.
Broad inventories and their original USB-related guard failures remain, with
separate reconciliation. This is not an unchanged-repository, release-media or
boot-acceptance claim. See [exact results](../roadmap/pkg1.md).

Remaining gates include inode allocation/free debits, extent-tree costs, orphan/
pinned reclaim, namespace/resource admission, competing ordinary/account writes,
credit-exhaustion refusal and authenticated pending recovery. These precede
protected mount integration and private Ring 3 acceptance. Installation,
production trust provisioning, release builds, pushes and physical writes stay
disabled.
