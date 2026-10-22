# PKG-2 private service, grant and lifecycle integration

Candidate host integration, 2026-10-21. Builds on the
[capacity and owner candidates](PKG2_ACCOUNT_CAPACITY_OWNER.md). Production
integration, installation and trust provisioning remain disabled.

## Actual code and private overlays

`scripts/test_pkg_service_integration.py` copies the current C/header/include
inputs, records hashes, and makes explicit replacements only in that copy:

- Rename the filesystem semantic commit so private dispatch can use the existing
  tagged-grant or account-capacity wrapper and original JBD2 writer.
- Route actual account-service BEGIN headroom and UPDATE preflight to the capacity
  certificate with G read from the validated durable marker/ledger/table set.
  UPDATE preflight precedes disabling service or incrementing account generation.
- Wrap storage in actual `accounts_prepare` before it runs account redo recovery.
  Backend read/validate/sync and the service's coherent account parser remain real.
- Add reader refusal inside the locked legacy and actor EXT4 open paths while an
  account lease/recovery excludes role readers. Existing readers make admission
  refuse; the candidate does not invalidate their descriptors.
- Add callbacks after registry lock release in commit, abort, accounted exit and
  forget. These invoke the private owner arena without nesting process/owner/FS
  exclusion. Normal spawn calls commit before putting the child on its runqueue;
  staged publication uses the same registry API.

The runner extracts the actual FD initialization, clone, allocation, lookup, close,
dup/dup2 and CLOEXEC helper bodies from `thread.c`, and the actual VFS-to-syscall
error mapper from `syscall.c`. They compile with the real TCB layout. Exact
extracted bytes and hashes are retained; these functions are not hand-written
copies. Shared source files remain untouched. The normal kernel build does not
link this harness or its private hooks.

## Account/grant coupling

The fixture starts with a certified detached root containing coherent root-only
accounts. It creates fixed package records on that detached root using real
backend operations, with no fake trust flags or temporary root publication.
Enrollment is explicitly synthetic and confined to disposable test images;
production provisioning is not implemented. Before account recovery, fixed
record inode generations, modes, ownership, links and maps are checked, followed
by the existing grant ownership validator.

`accounts_prepare`, ACCOUNT_BEGIN, ACCOUNT_UPDATE and ACCOUNT_FINISH execute the
actual service and parser. Capacity escrow comes from the actual durable grant's
remaining budget rather than a mock scalar. Successful account commits rederive
the role potential while preserving package records byte-for-byte. The service
uses valid account text in this campaign, unlike the earlier maximum-byte
synthetic semantic validator. Corrupting the ledger hash makes UPDATE return the
real syscall EIO before writes or generation change. This corruption and repair
are labelled fixture injections through actual metadata commits, not legitimate
protocol transitions or automatic repair.

An existing canonical reader causes BEGIN to return EAGAIN without writes. Once
BEGIN succeeds, new role-reader opens return EAGAIN under filesystem exclusion.
FINISH reopens readers only when the service is enabled and has no writer. A
pending failed update keeps ordinary readers excluded. Namespace fencing remains
the existing explicit opt-in account profile.

## Owner/descriptor coupling

The copied registry commit hook creates generation-checked owner values. A small
separate map reserves a pending birth slot; detach during registration marks it
dead, so a subsequently registered arena object is cancelled rather than leaked.
Map exclusion is released before arena or registry calls. Missing/pool-exhausted
registration denies package admission; it does not authorize a PID-only fallback.
The pending-birth race branch is reviewed, not paused/fault-injected by this
integration campaign.

Package acquisition obtains the real durable IDLE decision, retains an arena
operation, and publishes a real RESERVED grant before returning its exclusive
file descriptor. Allocation/FD failure after durable reservation cancels volatile
ownership but retains disk evidence for recovery; those failure branches are
reviewed, not fault-injected in this campaign. The real dup/dup2 helpers reject
the exclusive descriptor, and real clone omits it. Its real CLOEXEC/close callback
cancels the session without releasing escrow.

The fixture admits another operation, performs actual accounted process exit,
then closes descriptors. Registry exit detaches the live owner while the retained
operation holds its value/session references. That admitted operation subsequently
creates and writes a tagged file through actual planners, then completes. No TCB
pointer is retained. A new writer is refused by the recovered RESERVED ledger
even after the old owner references are gone. Abort and forget hooks are also
exercised. Trusted package cleanup refunds actual allocations, releases to IDLE,
and permits fresh acquisition. CLOEXEC of that fresh lease leaves its durable
reservation intact until explicit cleanup.

## Recovery and limits

Normal and durable-empty-publication interruption modes run for BIOS-independent
host block/sector geometries 1/2/4 KiB × 512/4096. An interrupted real UPDATE
retains its hash-checked account intent and charged package object.
Two fresh host-process runs load independently retained synthetic expectation
context, recover accounts with the real service, retain the package reservation
on the first run, and explicitly clean it on the second. The original generic
constructor still replays before private grant validation; no grant-aware
pre-replay admission or trusted boot selector is implemented here.

2026-10-22 continuation: the opt-in `--pre-replay` copied-source overlay now
validates grants in constructor preview before replay. See
[the preview candidate](PKG2_GRANT_PREREPLAY.md) for its exact profile and remaining
account-capacity/selector boundaries. The original invocation without that flag
retains the historical ordering described above.
The subsequent [account-capacity preview](PKG2_ACCOUNT_PREVIEW.md) also certifies
the account namespace and C-A bound at that boundary, before replay writes.

Host VMM range checks, current-thread selection, mutex/IRQ behavior and lifecycle
invocation are adapters. Registry and extracted FD functions execute, but the
scheduler, CR3/stack destruction, real Ring 3 syscalls and guest exec/spawn do not.
The private coordinator serializes service/acquisition calls; global test lane
selection and trusted cleanup are not a general concurrent kernel dispatch API.
No cross-CPU/IRQ, TSan, arbitrary successive I/O-failure, or all-write/flush cut
claim follows. The interruption is an explicit durable callback boundary.
Account directories are prepared baseline state; their allocating initialization
and arbitrary installed layouts remain outside the profile.

Linux e2fsck and exact canonical-account byte checks audit final images. Final
ledger/table checks require IDLE and zero escrow/charges/ownership entries.
A separate selected 4 KiB/512-byte audit retains the tagged payload across the
interruption and first recovery, checks its bytes, coherent RESERVED G/Q, and
unchanged package record bytes. Source/input guards are reported separately from
runtime and Linux results. No boot or release-media acceptance is claimed.

Run `make -f tests/pkg_grant_candidate.mk test-pkg-service-integration` in WSL.
Exact campaigns, retained failures and remaining gates are in the PKG-1 roadmap.
