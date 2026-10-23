# Private mid-callback account-orphan recovery

2026-10-23. Continues [account cancellation and kernel tests](PKG2_ACCOUNT_CANCEL_KERNEL_FS.md).
Production integration, installation and production trust provisioning remain
disabled. This work changes private test candidates and exact copied-source
overlays. It adds no production mount caller, filesystem format or syscall ABI.

## Admission and provenance

A cancelled fixed-role remove can durably remove its name, then stop before
reclaiming data or the inode. The generic EXT4 constructor already audits the
complete allocation bitmap, namespace references and JBD2 preview. Its package
preflight must also decide whether that orphan belongs to account recovery.
Root ownership and a familiar mode alone do not establish this origin.

The workbench captures a trusted origin immediately before the actual fixed-role
remove, while the named inode is still linked. It records filesystem UUID,
role, parent inode, inode number/generation, original size, initialized extent
mapping and physical allocations, including extent-tree blocks. The fixture
retains this in a separate `.origin` sidecar across fresh host processes. This
is a native x86 host test record supplied by trusted fixture code, not an
authenticated on-media record or production provenance authority. Do not adopt
it from untrusted input or infer one from a discovered orphan.

Before replay or any write/flush, the private constructor permits at most one
account orphan. It must match that exact origin, have zero links, retain the
original size and generation, have the role's root owner/group and exact mode,
and be absent from every live fixed-role name. Its role must be vacant and its
recorded parent must match the freshly discovered account namespace anchor.
Package marker/grant/table/transaction and charged inode identities are excluded.
Every remaining data extent must match an original logical/physical prefix;
remaining physical ranges must be contained in original allocations. Holes,
unwritten extents, excess allocation, wrong `i_blocks`, unexpected identities
and additional orphans reject. The existing complete ownership audit remains
mandatory; none of these checks replaces its bitmap/reference/alias checks.

The policy is deliberately narrower than generic orphan recovery: linked
truncate orphans, package-charged orphans and unknown origins are not admitted.
Mounted ownership/trust flags and allocation counters are never fabricated.

## Capacity conservation

The read-only certificate computes `R = C - A_linked - A_orphan`, using actual
remaining allocation and the still-allocated orphan inode. A vacant role and
absence of aliases prevent double counting. It counts only blocks still owned
by the orphan, rather than the original byte length or an optimistic future
free. An unlinked inode with no remaining data still occupies one inode.

When actual constructor cleanup frees `B` blocks and `I` inodes, both actual
free space and required capacity increase by `(B,I)`. Thus `F - G - R` remains
unchanged. The fixture compares the pre-replay certificate with a fresh mounted
certificate after cleanup, requiring exactly this difference. After cancellation
inside a callback, it also compares a fresh orphan-aware certificate with the
per-call capacity tracker derived from real superblock allocation deltas.
The public fixed-role account namespace fence is enabled only after constructor
cleanup has removed all orphans; ordinary admission remains closed throughout.

## Interruption campaign

`python3 scripts/test_pkg_cancel_recovery.py --account-mid` injects retained-call
cancellation immediately after each successful actual semantic commit of an
account UPDATE. A commit that already won completes; the next commit, mutating
callback or sync refuses EINTR. Owner unwind drains all bindings, pins and
admissions; authentication stays disabled and ACCOUNT_CHECK returns EAGAIN.
The interrupted process closes its image without freeze or cleanup.

The runner optionally accepts `--local-images --jobs 8`: each probe uses a
disposable Linux regular-file copy and returns its image/sidecars, including on
failure. Independent cuts use distinct host processes and distinct images; the
seed must remain byte-identical. This parallelizes tests, not filesystem writers
on a shared mount. All full constructor ownership audits remain enabled.

All three new databases differ from the old coherent root-only database. Each
new file is exactly 8192 bytes, with parser-valid comment padding, so staging
cleanup exercises multi-block account allocations at 1/2/4 KiB block sizes.
This remains a root-only account profile, not broader user/group policy testing
or an external-extent-tree coverage claim. Recovery can encounter fragmented
inline extents after freed holes are reused; its commit count is measured from
the real baseline rather than inferred from file count or aggregate bytes.

Each interrupted image undergoes an unchanged-image, zero-write/flush negative
marker check. Orphan cases additionally reject missing or mismatching origin
identity/generation, invalid role, wrong parent/UUID, package identity and, where
data remains, an empty original allocation map. Directed disposable copies also
change actual orphan UID, GID and mode using Linux debugfs, then require rejection
before writes/flushes. These test real inode metadata, rather than only sidecar
fields. Two fresh processes perform constructor preflight, orphan cleanup and
actual account-service redo recovery, followed by independent Linux audits.

A separate copy is cancelled again at the first actual account-service recovery
commit, then recovered twice. If constructor cleanup has already completed all
remaining work, this attempt correctly reports zero commits and no interruption;
it is not counted as a cancellation. This specifically exercises repeated redo
callback cancellation, not cancellation inside constructor orphan cleanup.

A separate focused 4 KiB/512-byte-sector campaign starts from UPDATE cuts
8, 17 and 23, then cancels every semantic commit of actual account-service
recovery. These states respectively retain `intent.tmp`, `group.publish` and
`shadow.publish`. They enumerate the measured recovery commits and cover removals
that a first-recovery-commit-only test would miss. Each cut undergoes negative
preflight, two fresh recoveries and the same independent Linux oracle. The
controller and exact source/binary guards are retained with private evidence;
it reuses the checked-in transport/oracle functions rather than weaker copies.

The ordinary mismatching-package-identity negative alone does not isolate the
charged-orphan exclusion. A directed fixture instead starts from a real UPDATE
cut with vacant canonical shadow, then constructs an actual allocated charged
orphan with matching expected inode/generation, valid allocation and resealed
grant/table records. Complete constructor ownership preview succeeds, and the
explicit charged-identity policy rejects before writes/flushes. This is a
negative fixture assembled in a disposable regular file, not charged-orphan
recovery or a production-origin enrollment operation.

Linux e2fsck and byte-exact debugfs reads verify complete old or complete new
canonical databases, absent pending/staging/publish files, unchanged package
payload and unrelated ordinary file. An independent wire/inode oracle checks
grant/table hashes and linkage, T/G/Q, sequence/transaction identity, actual
charged inode generation/owner/mode/link/size/block count and canonical metadata.
Each Linux audit must preserve the whole image hash.

Host fflush barriers and semantic commit cuts do not establish sector/flush
interruption safety, uncertain-I/O behavior, controller ordering or physical
power-loss durability. Near-full recovery progress is not exercised by these
fixtures. There is no new Ring 3 or boot acceptance for this feature;
the previous combined kernel campaign is separate historical evidence.
Authenticated durable orphan provenance and production integration remain open.
The next [enrollment and durable provenance contract](PKG2_ENROLLMENT_PROVENANCE.md)
separates signed enrollment from kernel-controlled mutable provenance and defines
atomic activation/retirement requirements. It is design only; the sidecar tests
above remain unchanged and do not establish those guarantees.
Exact invocations, outcomes and retained preliminary failures belong in the
[roadmap](../roadmap/pkg1.md).
