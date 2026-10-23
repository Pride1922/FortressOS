# Private authenticated retirement integration acceptance

This closes one bounded private milestone: durable authenticated retirement evidence,
read-only mount preview and actual account-service recovery, followed by combined
interruption/refusal acceptance. The results below come from completed immutable campaign manifests.
The three-part private milestone is closed (2026-10-23).

## Implemented profile

The copied-source adapter uses actual EXT4/JBD2, VFS and account-service/redo code.
Production sources remain unchanged. Preallocated root-owned certificate, enrollment,
ledger and independent artifact-evidence files are pinned by caller-provided fixture
policy. Public deterministic test signing keys establish only this private fixture's
authority; no production trust is provisioned.

The read-only journal preview verifies enrollment, catalog and retirement signatures,
pending state, trust, account lineage, real inode generation/ownership/link counts,
allocation maps, bitmaps and retained content before replay. The reopened engine is
checked again before its dirty mark. Typed mutations recheck authority under the
existing filesystem exclusion. Their JBD2 transactions publish the grant, ownership
table, retirement ledger, account-generation record and origin slot together with
actual namespace removal and physical frees.

Two actual open-node pins retain the unlinked archive; the first close does no I/O.
Final close and constructor recovery reclaim its data and inode in separate commits.
Grant/account sequences advance together, credential generation stays fixed, and
repeated recovery grants no extra credit. Actual account CHECK/SNAPSHOT remain
available; BEGIN fails closed without I/O while this frozen lineage is retained.
An active account transaction is outside the supported profile and refuses admission.

The selected victim is one inline, one-block archive. Other executable/state/intent
roles (17-19) and remaining escrow stay charged and retained. This milestone does
not implement their final retirement, escrow release, IDLE transition or general
package installation. It closes the agreed private integration profile, not PKG-1
as a whole.

## Findings fixed

- The account-orphan dispatcher initially refused a legitimate authenticated
  package victim. A scoped typed route now admits only the signed inode and parent.
- Account-service preparation can fail its final durability flush after filesystem
  construction succeeds. The harness now requires disabled authentication and
  zero-I/O refusal after this failure, then checks two fresh-process recoveries.
- At exact reserved capacity, ordinary inode creation could steal the account
  reserve. Planned superblock admission now enforces both block and inode floors
  F-G >= R. Package-orphan occupancy is restored when calculating R, so package
  charged ownership cannot reduce the account reserve. Real competing inode and
  block allocations now fail before I/O while authenticated cleanup succeeds.
- The initial clean runner overescaped its sequence-report expression. Its FAIL is
  retained; the final campaigns use the corrected measurement.

The pressure fixture sets G=F-R in an independently re-signed native grant rather
than physically filling a filesystem. Account reserve is 80/35/17 blocks for
1/2/4 KiB filesystems and eight inodes. It is unchanged across retirement.

## Evidence and limits

Final acceptance: **PASS**, strict GCC warnings and ASan/UBSan. Six geometries
use selected first/middle/last fault positions; the additional 4 KiB/4 KiB run
is exhaustive at every observed activation/unlink/close/recovery I/O event.

| Block/sector bytes | Faults | Fresh-process checks | Admitted | Unpublished refusals | Max images |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1024/512 | 96 | 192 | 160 | 32 | 18 |
| 1024/4096 | 96 | 192 | 164 | 28 | 18 |
| 2048/512 | 96 | 192 | 160 | 32 | 14 |
| 2048/4096 | 96 | 192 | 164 | 28 | 14 |
| 4096/512 | 96 | 192 | 160 | 32 | 12 |
| 4096/4096 | 96 | 192 | 164 | 28 | 12 |
| 4096/4096 exhaustive | 1616 | 3232 | 2992 | 240 | 12 |

Totals: 2,192 faults and 4,384 fresh-process checks: 3,964 authenticated
admission/recovery checks and 420 correct zero-write unpublished-state refusals.
The second check preserves the admission decision; admitted repeat recovery makes
zero semantic commits. These are checks, not 4,384 successful recoveries.

There are another 385 explicit zero-I/O adversarial refusals (55 per geometry/run),
35 full Linux/record audits and seven pressure-fixture fsck-only audits. Full audits
verify hashes, record generations, inode generation/ownership/link counts, maps,
bitmaps, charged ownership and native free counters. All e2fsck runs pass.

Baseline I/O-event counts (unlink / final-close / recovery / activation):

| Block/sector bytes | I/O events |
| --- | --- |
| 1024/512 | 87 / 164 / 185 / 87 |
| 1024/4096 | 46 / 87 / 101 / 46 |
| 2048/512 | 137 / 254 / 289 / 121 |
| 2048/4096 | 38 / 71 / 85 / 34 |
| 4096/512 | 237 / 434 / 497 / 189 |
| 4096/4096 | 34 / 63 / 77 / 28 |

Exhaustive 4 KiB/4 KiB cuts: 272 unlink, 504 final-close, 616 recovery and
224 activation. Maximum transaction images are 18/14/12 for 1/2/4 KiB
filesystems (existing limit 64). Grant/account sequence progresses 1 to 4;
credential generation remains 1. The selected archive refunds one block and
one inode: G 12/4 to 13/5, Q 4/4 to 3/3, conserved T.

Final immutable evidence directories:

- .codex-remote-attachments/pkg-retirement-integrated-20261023-211517-311862/
- .codex-remote-attachments/pkg-retirement-integrated-20261023-211527-569149/
- [Aggregated counts, hashes and preservation](../../.codex-remote-attachments/pkg-retirement-acceptance-summary-20261023/manifest.json).

Both final manifests report no changed repository sources, compiled copies or
guarded files. Their source hashes were independently rechecked after completion.
AGENTS.md, README.md and existing shared ELF/initramfs/ISO/raw image bytes are
preserved. This is preservation evidence, not a boot or media acceptance run.


Each interruption is injected before/after write or flush boundaries using eight
RAM-disk persistence profiles. Activation cuts may either authenticate a complete
publication or refuse an unpublished state without any writes and with identical
bytes. A read-only journal-visibility oracle prevents a complete visible certificate
from being mislabeled unpublished. Other published-state cuts must recover, then
recover again without additional commits. Successful temporary images are deleted
only after both checks; failed images and logs are retained.

Linux audits apply to named retained outputs, not every interruption image. The
sector-atomic RAM model does not cover torn sectors, controller firmware or physical
power loss. Host sanitizers and fresh-process recovery do not establish boot, Ring 3,
SMP, scheduler/signal teardown or physical acceptance. Source and shared-media
preservation are separate checks and do not establish boot acceptance.

Raw failed attempts remain in .codex-remote-attachments:

- pkg-retirement-integrated-20261023-203316-460804: initial compile failure and
  incremental diagnostic subdirectories, including the dispatcher refusal, final
  account flush harness failure and reserve-stealing reproduction. Its workspace
  was reused for debugging; historical source copies there are not immutable final
  acceptance evidence. Raw logs and native fixtures remain retained.
- pkg-retirement-integrated-20261023-205819-118400: immutable failed clean campaign,
  caused by the report-expression bug after functional/adversarial checks.
- pkg-retirement-floor-old-context-20261023: deliberate copied-source variant with
  only the planned reserve-floor check removed, preserving a reproducible defect
  witness. Its first mistaken no-environment invocation is retained separately.

Reproduce the six-geometry campaign with:

    make -f tests/pkg_grant_candidate.mk test-pkg-retirement-acceptance

The explicit exhaustive geometry command is:

    python3 scripts/test_pkg_retirement_integrated.py --block 4096 --sector 4096 --exhaustive --jobs 4

Production integration, installation, production trust provisioning, release-media
builds, pushes and physical-device writes remain disabled. Changes are uncommitted.
