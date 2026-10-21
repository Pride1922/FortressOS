# PKG-2 restart-safe namespace admission candidate

Status: sufficient, deliberately restrictive `/etc` profile and host test
specification. No production scanner, persistent grant, writer fence, root
provisioning, account protocol change or boot integration is implemented.

This addresses the [observed two-interruption failure](PKG2_NAMESPACE_REPLAY_REVIEW.md).
It does not authorize mutation or replace block/inode/pool admission. A normal
compact directory can fail this profile despite being usable; no automatic
relayout or provisioning fallback is specified.

## Exact record profile

Assume the complete validated directory consists of:

- Exactly sized 24-byte records which are free or occupied by one of the six
  fixed account names: passwd/group/shadow or their `.publish` temporaries.
  At least **six** such records must exist in total; count occupied records too.
- Free records of 12 or 16 bytes. They cannot hold a publication name.
- Canonical entries in 16-byte records, or the 24-byte records above. All three
  canonical names use exactly 16 bytes after alignment; publication names use 24.
- Other live entries with less than 16 bytes of slack. Their ownership and
  geometry are immutable for the admitted transaction/recovery interval.

Reject other free-record sizes, account entry sizes, foreign usable slack,
duplicate fixed names and unexpected roles. The test classifier deliberately
does not generalize a larger free record into several virtual slots: insertions
can split records and lose that supposed capacity across repeated recovery.

The six records may be scattered through the directory. No directory growth is
credited. After interrupted recovery, admission reconstructs the count from
actual physical records and validated role ownership; it does not demand six
currently free records or reset a resource promise on reboot.

This is a structural classifier after validation, not a raw-byte parser or an
authorization check. Before treating a name as an account role, a future mount
validator must authenticate parent/mount identity, regular-file type, inode
identity and exclusive ownership, root uid/gid/mode, lack of foreign aliases,
allowed partial lengths, and pending/staging integrity and protocol state.
Missing canonicals require a valid recoverable pending state. A path string alone
never establishes ownership. Invalid metadata must fail admission, not be
relabeled immutable or absent.

## Inductive capacity argument

Every insertion uses 16 or 24 bytes. Under this profile it can use only a free
16-byte record (canonical only) or a free 24-byte record. No occupied record has
16 bytes of slack, so insertion never splits an occupied record. A canonical in
a 24-byte record leaves only eight bytes of slack. Deletion clears the inode
number and preserves record length. Therefore the eligible 24-byte records
remain the same physical records across creation, removal and rename.

Immediately before creation of a publication temporary, that role's old
temporary has been removed. At most three canonicals and two other temporaries
occupy eligible records: at most five records are occupied.

Immediately before rename inserts the destination canonical, that canonical
has been removed. At most two canonicals and three temporaries exist, including
the rename source: again at most five records are occupied. The actual rename
planner inserts the destination before deleting the source, so the proof
explicitly covers both names during the atomic operation.

With six eligible records, either insertion has a free record. Other steps only
write bytes, remove names, reclaim objects, sync or validate. At an arbitrary
restart, the protocol starts from the first role and performs the same removal
before insertion. Durable partial temporary contents do not alter name sizes.
Whole journal transactions must select valid before/after namespace states;
torn/replay-corrupt states are rejected by the separate filesystem admission.

Thus no admitted namespace transition needs directory growth, assuming the
existing account publication protocol and the writer boundary below. This is
namespace capacity only: allocation, heap/cache, journal credits, orphan pins,
I/O failure and authentication can still stop recovery safely. Infinite
interruptions are not a completion guarantee.

Six is sufficient for this profile, not necessary for every layout. Some
three/four/five-record layouts are safe under the finite model but are rejected
by this conservative classifier. Other five-record layouts demonstrably block.

## Writer boundary and other parent directories

The argument excludes every other creator, unlinker, renamer and directory
layout mutator in the protected parent, including privileged writers and package
operations. A future implementation must enforce that boundary from admission
through intent retirement, across owner death and remount. Checking once then
allowing an unrelated writer to consume a record invalidates the proof.
Filesystem exclusion for one operation does not establish a durable fence.

The accounts private staging directory is a separate namespace. Its creation,
stage and intent/pending names, metadata, aliases and capacity also require a
reviewed admission argument. The current finite model holds that directory
fixed, and the actual fixtures have ample capacity there. This `/etc` result is
not a complete account/package namespace admission contract. Package inventory,
grant and transaction parents require their own proof and fencing.

Before production integration, bind these validations to the same durable grant
and mount epoch as remaining block/inode accounting. Reconstruct before exposing
a writable installed root or permitting competing writers. No new default
restriction on roots without package metadata is introduced by this experiment.

## Verification

```sh
python3 scripts/test_pkg_namespace_admission.py
python3 scripts/test_pkg_namespace_admission_ext4.py .codex-remote-attachments/pkg-account-namespace-20261021-132851
```

The finite model explores all forward and restart edges for three block sizes,
three placements of free records, six canonical orders and 3/4/5/6 eligible
records: **216 profiles, 1,033,089 summed state instances**. Every reachable state
of all 54 admitted six-record profiles satisfies the classifier; no forward step
blocks and terminal states are reachable. This models namespace operations and
logical journal boundaries, not every C planner or journal sector fault.

The 162 smaller profiles are conservatively rejected. Among them, 36 three-record,
18 four-record and 18 five-record profiles have blocked states. Seven negative
controls reject malformed/duplicate/wrong-sized roles, unsupported geometry and
a bin stolen by an excluded foreign writer. Input ownership and concurrency are
assumptions of the model, not implemented security enforcement.

Pure-model evidence: `.codex-remote-attachments/pkg-namespace-admission-20261021-134420/`.
Retained source hashes compare unchanged. C sources there are references, not
executed by this Python model.

The actual-code gate derives from the accepted prior EXT4 campaign. It only
merges six adjacent 12-byte free records into three additional 24-byte free
records in private copies, preserving all inode/data identities and the actual
1 MiB directory limit. Linux validates the resulting six-record seed. The
three-record input is rejected without mutation. Actual EXT4/JBD2/redo executes
the same two durable empty-create interruptions, followed by successful recovery
and another fresh recovery. The structural classifier checks before and after
every invocation. Synthetic 8 KiB account validation and host locks/device I/O
remain adapters; this is not production account-service or firmware acceptance.

Actual-code evidence:
`.codex-remote-attachments/pkg-namespace-admission-ext4-20261021-134539/`.
**24/24 terminal states PASS** under ASan/UBSan and Linux fsck/debugfs, across
1/2/4 KiB blocks x 512/4096-byte sectors. Each pair covers two durable empty-create
interruptions, successful completion and another fresh successful recovery.
The candidate accepts all 48 before/after directory observations. All three
six-record seeds pass e2fsck; the original three-record seeds are rejected by
the classifier and retain their original hashes. Canonical/staging bytes,
canonical ownership/modes and unrelated sentinel bytes/mode are checked; completed
states additionally have no publication temporaries or pending intent. Partial
temporary/pending validity is exercised by the actual subsequent redo loader,
not a new independent corruption validator in this gate.

The gate verifies the copied prior source inventory before compilation and
records current gate-source hashes, which compare unchanged after the run.
It does not claim the entire shared checkout or shared media stayed byte-equal.
There were no failed/partial campaigns in this continuation; prior namespace
fixture/observer failures remain retained under their original qualifications.
`git diff --check` and both opt-in target dry-runs pass.

Next gate: private staging-parent capacity and role/alias validation, followed
by a concrete durable writer-fence contract and remaining-resource refinement.
The structural predicate must not be connected to a writable production mount
until those dependencies and failure routing are reviewed. Installation,
production trust provisioning and grants remain disabled. No release build,
physical write, push or fresh Ring 3 boot acceptance accompanies these host tests.
