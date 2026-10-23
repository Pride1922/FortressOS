# Private retirement completion through IDLE

This continues checkpoint add8a21. Production integration, installation, production
trust provisioning, release-media builds, pushes and physical-device writes stay
disabled. All hooks are exact overlays on copied host workbench sources. Production
kernel/filesystem/account sources are unchanged.

The bounded installed demo has four charged roles. Archive role 1 is unlinked and
reclaimed; installed executable role 17 is transferred to baseline ownership without
freeing or modifying it. Temporary next-state role 18 and retired-intent role 19 are
unlinked and reclaimed. With no charged allocations or orphans left, one JBD2
transaction clears the ownership entries and retirement ledger, releases unused
escrow, publishes the canonical IDLE grant and clears account transaction bindings.
The enrolled transaction directory remains present and empty.

A separate completion-role fixture certificate authorizes exact hashes of each
five-record state. It is persisted in a root-owned, preallocated fixed-map file;
its hash, locator and fixture public keys come from independent caller policy.
The archive-only certificate remains unchanged and cannot authorize later steps.
Public deterministic fixture keys are not production trust provisioning.

Actual inode generations, root ownership, link counts, physical maps, bitmaps,
content hashes, native free counts and account database consistency are checked
before replay. Each planned semantic operation checks the signed next record set
under filesystem exclusion. Unlink refunds nothing; block and inode frees refund
only observed physical changes in their respective dimensions. Executable transfer
decreases charged/total accounting without increasing native free space. Final
release decreases escrow/total accounting without inventing a physical refund.

The fixed account-database profile permits repeated real account BEGIN/FINISH
writer lifetimes after IDLE. Only the paired sequence, operation generation and
open/closed state normalize to the signed terminal state; credential generation,
account hashes, package fields and all other bytes remain bound. Account database
updates are outside this completion fixture. This is not production account-policy
or arbitrary package installation support.

## Wire and transition review

The new certificate is 1728 bytes: a 1536-byte signed body and two sorted
96-byte Ed25519 key-ID/signature records. Independent fixture policy extends the
previous 1568 bytes with its SHA-256 and a 128-byte native locator. Neither
certificate authority nor its locator is adopted from mutable package records.

The body binds enrollment identity/epoch, original retirement certificate and
enrollment hashes, installed-state/trust/artifact/executable hashes, twelve
lifecycle record-set commitments and two reference account BEGIN/FINISH snapshots,
original ownership rows, starting native free counters and account operation
generation. The gate validates every signature, threshold, reserved area and
fixed-map identity. Account BEGIN/FINISH normalization uses the signed terminal
commitment; it does not authorize changes to account contents or credentials.

The eleven lifecycle commits are archive unlink, data free and inode free;
executable baseline transfer; temporary-state unlink, data free and inode free;
retired-intent unlink, data free and inode free; then atomic record clearing and
escrow release. Sequence progresses 1 to 12. Temporary files each use one inline
data block in this bounded profile. Credential generation stays 1. At normal
capacity, G/Q start at 12/4 and 4/4; after physical cleanup and executable transfer
G is 15/7 and Q is zero, then IDLE releases G and sets T/G/Q to zero. The executable
and canonical installed state remain unchanged. Fixed certificates, enrollment,
trust and evidence remain retained as baseline control/audit files.

Before IDLE, the fixture freezes ordinary allocation as well as the account
writer. At exact F-G=R, real empty-file creation isolates the inode floor and
append to an existing file isolates the block floor. Both must return ENOSPC
without writes/flushes or taint; cleanup remains possible. Pressure is independently
re-signed escrow pressure, not a physically full filesystem. R remains
80/35/17 blocks for 1/2/4 KiB filesystems and eight inodes throughout cleanup.

After IDLE, ordinary inode/block allocation succeeds and reuses a retired inode
with a newer generation. The original implementation incorrectly required retired
inode numbers to stay unused forever. It now distinguishes a dead old identity
from legitimate later generations; a live old generation still refuses. Linux
fsck and fresh constructors audit the successful reused inode.

## Retained findings and attempts

All directories below remain under .codex-remote-attachments; failed manifests,
raw logs and immutable copied sources have not been overwritten.

- pkg-retirement-idle-20261023-213419-921947: compile failure from declaration
  order, a nonexistent ACCOUNT_ABORT constant and strict warnings. Corrected to
  the actual ACCOUNT_FINISH interface and explicit declarations/formatting.
- pkg-retirement-idle-20261023-213657-052017: fixture setup refusal because the
  completion gate ran before enrollment. The gate now preserves the existing
  explicitly pre-enrollment construction path while inactive.
- pkg-retirement-idle-20261023-213942-941335: focused PASS, superseded by deeper
  allocation/refusal/owner-lifetime acceptance.
- pkg-retirement-idle-20261023-214358-742503: false Linux directory-audit failure.
  debugfs displays old names in zero-inode deleted directory slots. Only live
  entries count toward namespace presence; raw output is retained.
- pkg-retirement-idle-20261023-214641-982354: genuine post-IDLE inode reuse
  refusal, reproduced by real allocation and fixed with generation-aware checks.
- pkg-retirement-idle-20261023-214925-068590: focused selected-interruption PASS,
  superseded by final immutable geometry/exhaustive campaigns.

## Acceptance limits

This is the existing one-demo, install-only, single-block temporary-file profile.
It does not establish arbitrary upgrades/removals, concurrent CPU execution,
account database UPDATE, general future package grants or full-filesystem rollback
protection. Contending owners are invoked through actual account-service entry
points in a host process; process death invokes the actual process-exit hook.
No real kernel scheduler or signal campaign is inferred. ASan host stack usage is
not kernel stack acceptance. All large retained snapshots are private workbench
state, not production footprint or ABI commitments.

The fault model covers sector-atomic persistence at every observed write/flush
boundary in the named exhaustive geometry, with four persistence modes and
before/after cuts. It does not cover torn sectors or physical/controller power
loss. Mount construction, semantic completion and final account-service flush
are within the measured finish range; subsequent freeze/clean teardown is not.

Each cut must recover to the independent signed IDLE five-record set twice,
without any extra record-generation advance on the repeat. Temporary passing
images are removed only after both checks. Failed images and logs are retained.
Linux audits apply to named retained outputs, not every cut. Raw corruption
refusals may be caught by filesystem checksums; a separate five-case oracle
recomputes valid ext4 inode CRC32C, requires clean Linux fsck, and then requires
zero-I/O admission refusal for executable owner/group/mode/generation changes and
a live resurrected retired generation. That separates physical identity/ownership
validation from simple corruption detection.

## Acceptance

**PASS (2026-10-23). The agreed private retirement-to-IDLE milestone is closed.**

The tested preceding checkpoint was committed locally as add8a21. This continuation
remains uncommitted. No push was performed.

Strict GCC warnings and ASan/UBSan pass. The six-geometry campaign samples the
first/middle/last finish I/O events in eight persistence/before-after profiles. The
additional 4 KiB/4 KiB campaign exhausts all 353 observed finish I/O events.

| Block/sector bytes | Finish events | Faults | Fresh processes | Zero-I/O refusals | Retirement images |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1024/512 | 908 | 24 | 48 | 84 | 18 |
| 1024/4096 | 485 | 24 | 48 | 84 | 18 |
| 2048/512 | 1402 | 24 | 48 | 84 | 14 |
| 2048/4096 | 397 | 24 | 48 | 84 | 14 |
| 4096/512 | 2390 | 24 | 48 | 84 | 12 |
| 4096/4096 | 353 | 24 | 48 | 84 | 12 |
| 4096/4096 exhaustive | 353 | 2824 | 5648 | 84 | 12 |

Totals: **2,968 interruptions, 5,936 fresh-process checks and 588 explicit
zero-I/O refusals**. Every cut reaches the independent signed IDLE five-record
set on both restarts. The record bytes, including grant/account sequence and
account operation/credential generations, stay identical on the second restart.
There is no extra refund or implicit record-generation advance.

There are **70 retained-output Linux/record audits**, plus **five separate
Linux-clean, checksum-valid native refusals**. Positive audits check exact five
record bytes against independent signed vectors, debugfs executable bytes, live
transaction-directory entries and e2fsck. The supplemental refusals independently
recompute inode CRC32C before fsck and test executable UID/GID/mode/generation and
live old-generation resurrection. All five reject before writes or flushes.

Each geometry also passes: committed-uncheckpointed IDLE preview/replay; three
real account BEGIN/FINISH owner lifetimes with competing/non-owner refusal; actual
process-exit-hook fencing and recovery; ordinary allocation and retired-inode
reuse with a newer generation; exact account-reserve competing inode/block
refusal followed by completion and repeat recovery. The pressure fixture remains
synthetic signed escrow pressure, not physical space exhaustion.

The 84 explicit refusals per run comprise 60 mixed old/IDLE five-record
selections, seven completion-file corruptions, one forged but independently
re-pinned signature, seven policy changes, one wrong-policy committed-IDLE journal
preview, six executable inode-field corruptions, one inode-bitmap corruption and
one retired-generation corruption. Raw corruptions can fail checksum validation;
only the five supplemental cases claim coherent native-inode isolation.

Maximum observed retirement transaction images are 18/14/12 for 1/2/4 KiB
filesystems, below the existing limit of 64. These counters measure the retirement
transactions, not certificate activation or clean teardown. All injected cuts
start from fully activated fixtures; new certificate enrollment publication is
not a fault-sweep claim in this campaign.

Evidence:

- .codex-remote-attachments/pkg-retirement-idle-20261023-215338-866725/
- .codex-remote-attachments/pkg-retirement-idle-20261023-215351-114164/
- .codex-remote-attachments/pkg-retirement-idle-coherent-native-20261023/
- [Aggregate exact counts, hashes and attempt index](../../.codex-remote-attachments/pkg-retirement-idle-summary-20261023/manifest.json).
- [Independent coherent-native oracle source](../../.codex-remote-attachments/pkg-retirement-idle-coherent-native.py).

Both final campaigns report no changed compiled copies, repository input sources
or guarded files. Independent reconciliation rechecked those hashes after both
completed. AGENTS.md, README.md, the SSL checksum and existing shared ELF,
initramfs, ISO and raw image are unchanged. Source/shared-media preservation is
separate from acceptance: no boot or new media acceptance is claimed.

Reproduce from WSL:

    make -f tests/pkg_grant_candidate.mk test-pkg-retirement-idle
    python3 scripts/test_pkg_retirement_idle.py --block 4096 --sector 4096 --exhaustive --jobs 4

Final review: terminal publication is atomic with all five record changes;
physical frees alone create refunds; executable transfer and escrow release
create no physical credit; generation-aware reuse and account reserve survive
IDLE; fresh-process admission validates durable records before journal replay.
The bounded private lifecycle is complete. Production integration, installation,
trust provisioning, release-media builds, pushes and physical-device writes
remain disabled.
Host recovery and source/media preservation remain separate from boot acceptance.
No new Ring 3, BIOS/UEFI, SMP, scheduler/signal teardown or physical acceptance is
claimed. Failed attempts and raw evidence are retained under
.codex-remote-attachments/pkg-retirement-idle-*.
