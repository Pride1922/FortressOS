# PKG-2 private account capacity and owner lifetime

2026-10-21. Private candidates only. No production account, process, descriptor,
allocator, boot or installation integration. Complements the
[durable grant design](PKG2_DURABLE_GRANT_DESIGN.md) and
[filesystem workbench](PKG2_GRANT_LIFECYCLE_ADAPTER.md).

## Account capacity bound

The earlier one-pass gross-allocation formula is not a restart bound. The new
candidate instead reserves the unoccupied capacity of the entire fixed role set.
Let D=ceil(8192/block_size). A newly created role is written once in one existing
account backend call. At most D initialized data extents need a canonical tree;
T=1 if D>4, otherwise zero, bounds its external extent blocks on 1/2/4 KiB
geometries. The nine database roles each have capacity D+T blocks and one inode.
The two 160-byte intent roles each have capacity one block and one inode. Thus
C=(9*(D+T)+2 blocks,11 inodes): (83,11), (38,11), (20,11).

Define A as actual allocated blocks (data plus tree) and allocated inodes in the
linked account role set. At a quiescent callback boundary, R_acc=C-A, computed
from actual inode/maps rather than file-size sums. Empty durable creates count
one inode and zero blocks. Existing role maps must be dense and initialized;
over-capacity or unsupported maps refuse. Account namespace certificates provide
the preallocated name slots, fixed role names, modes, ownership and no-alias
checks. No directory-growth budget is added: growth is outside this profile. Baseline
account directories are already prepared before escrow is set; initial
`accounts_prepare` directory creation is not covered by this bound.
Namespace geometry is rechecked after every successful mutating callback.

Admission requires a healthy idle engine, the actual fenced namespace, no
orphans, no open account-role files and F-G>=R_acc in each dimension. This profile
excludes retained canonical readers; it does not make them disappear or reclaim
their storage. The private test owns the account operation exclusively and
permits no new readers/intervening ordinary operations. A future implementation
must enforce that ownership, not assume it from UID or filenames.

Every account-only semantic plan moves physical F and remaining capacity together:
R_next=R_before+(F_after-F_before), using the actual staged superblock. The private
wrapper checks F_after-G>=R_next and commits R_next only on acknowledged success.
Unlinked account orphans remain counted as occupied capacity by this tracker
until physical reclaim. The callback must finish draining them before the
quiescent role snapshot can agree. Between callbacks the certificate independently
recomputes C-A and requires exact agreement. Rename between equal-capacity roles
does not change either quantity. Delete/refund, empty create and bulk write have
the corresponding inode/block deltas. No future frees fund a plan's allocation.

Under these restrictions F-G-R_acc is a conserved potential. Restart reconstructs
R_acc from the actual recovered role set, rather than discarding the grant or
charging a second constant gross bound. This is a source-derived conditional
bound, with actual-planner refinement tests; it is not a general theorem about
arbitrary account layouts, mixed object growth, pins or parallel writers.
The constructor already performs the global ownership audit. Repeating that
quadratic audit at every callback is unnecessary in this exclusive private test;
the role/maps and namespace are still re-read and checked each time.

The test starts at exact effective capacity, refuses +1 escrow in either dimension
before writes, refuses a retained account reader, and runs the actual EXT4 account
backend and redo engine with maximum-byte records. It also interrupts at a durable
empty group-publication create, then recovers in two fresh host processes. Linux
audits final bytes, modes/structure through e2fsck, actual i_blocks, absent staging/
pending roles and unchanged F-R_acc across the input/output. Byte-profile semantic
validation is deliberately synthetic; the full account service parser/BEGIN/
UPDATE authorization is not exercised. G is a volatile test escrow chosen from
actual F-R_acc, not an enrolled package ledger. Its conservation is independently
checked, but persistent grant/account integration remains open.

Uncertain I/O invalidates the volatile candidate tracker; it must be reconstructed
after recovery. This campaign injects a callback-level durable empty create, not
all journal write/flush/sector cuts. Existing grant and account cut campaigns
remain separate evidence. No grant-aware pre-replay hook is added. A resource
bound does not reserve heap, cache/orphan slots, journal credits or device liveness.

## Private process-owned value objects

The actual process registry exports values and credential snapshots, not a
general retained process/TCB handle. `tests/pkg_writer_candidate.{c,h}` therefore
owns a separate bounded value arena. A trusted birth adapter binds a registry
PID to a unique, nonwrapping owner generation. Session and operation generations
are independent. A numeric PID alone cannot use the candidate; keys are internal
trusted-call-site values, not an exposed user ABI or a security capability.

Reference ownership is one for the live owner, one for its session, and one per
admitted operation. Begin requires a caller-supplied trusted durable-IDLE result,
root plus CAP_SYS_ADMIN from an actual registry credential snapshot, and no
existing session. Every operation rechecks credentials, live owner identity and
the exact session. A later credential drop refuses subsequent operations;
already-admitted work retains its original credential-snapshot authorization.
Close needs identity, not restored privileges, so cleanup survives a drop.

Exit drops the live reference and cancels new operations. Exec cancels the old
session while retaining the live owner object. Existing operations keep the
session/owner alive until their last completion. Only then can its slot be reused.
Duplicate/stale completion cannot drop a new reference. A different process key
cannot use an old session. Cancellation and reference retirement never release
disk escrow: even after all references retire, a new begin still requires an
independent durable-IDLE decision.

The owner guard is separate from filesystem and process exclusion. It never calls
the process registry while held and does no filesystem I/O. Credential snapshots
are obtained outside it; identities are rechecked under it. Tests use actual
registry birth, credentials, exit/forget, and pthread exclusion/barriers to end a
process while an operation is retained, followed by credential drop, exec and
slot/PID reuse checks. They do not run the scheduler or retain a real TCB.

Trusted birth/exit/exec notifications are host adapters. Birth registration and
notification delivery are serialized by that adapter; the current kernel has no
atomic registry publication-plus-owner-registration API. Production birth/abort/
exit/reaper and descriptor ownership still require integration. There is no FD
dup/spawn/exec ABI implementation, capability transfer proof, TSan/IRQ/SMP claim
or actual journal operation in the owner test. Durable IDLE is a mock predicate;
the previously tested actual grant reservation API is not called here. Those
boundaries must not be reported as completed production process-owned admission.

Run the standalone `test-pkg-account-capacity` and `test-pkg-writer-lifetime`
targets in `tests/pkg_grant_candidate.mk`. Evidence, failures and source
preservation qualifications are recorded in the PKG-1 roadmap. All installation,
production trust provisioning, release-media builds, pushes and physical writes
remain disabled.


The subsequent [private integrated service workbench](PKG2_PRIVATE_SERVICE_INTEGRATION.md)
connects these candidates to actual account-service admission/recovery, durable
grant records, copied registry call sites and extracted kernel FD helpers.
It remains outside production builds and retains the documented host/concurrency,
trusted enrollment and pre-replay admission limitations.
