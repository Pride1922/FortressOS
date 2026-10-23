# Private retirement ledger: actual JBD2 coupling

This continues [the pure ledger/accounting gate](PKG2_RETIREMENT_LEDGER_CODEC.md).
The separate host-only adapter stages grant, ownership table, retirement ledger,
account-generation v4 and empty origin slot in the actual EXT4 semantic
transaction. It changes no production source, mount-admission parser or release
target. Installation, production integration and trust provisioning remain disabled.

## Execution and allocation provenance

The copied-source runner wraps the existing private charged-orphan lifecycle
workbench. The real VFS namespace planner and real EXT4 orphan reclaimer determine
inode links, allocated sectors, allocation bitmaps and free counters. The existing
whole-filesystem orphan/ownership audit checks allocation uniqueness. Fixed record
objects require retained inode generations, root ownership, exact modes and sizes,
single links, dense initialized inline extent maps and matching sector counts.

The adapter validates the old complete five-record generation, constructs the next
complete generation, and calls the pure transition validator with actual current
and planned superblock free counters. All five record images join the original
planner's bounded transaction before the real JBD2 writer issues any writes.
Unlink advances records without refund. Actual data reclamation refunds blocks;
the separate final inode reclamation refunds the inode. T remains fixed and
increases in F, decreases in Q and increases in G must agree exactly.

Constructor validation runs **after journal replay and before orphan cleanup** in
a private copied-source hook. It is not pre-replay validation or mount admission.
The constructor receives an explicit unpublished engine; no mounted/trusted flags
are fabricated. The test retains trusted fixture bindings and inode locators in
the host process across actual filesystem teardown and constructor restart.
This is not fresh-process reconstruction of authenticated policy.

A held VFS file and duplicate reference pin the unlinked inode. Reading retained
data and closing the first reference must not refund resources. Final close and
constructor recovery use the real reclaimer. Every tested interruption is followed
by two constructor restarts and record/allocation validation. Repeated orphan
recovery must issue no I/O and grant no extra credit.

## Boundaries

The fixture is one root-owned, singly linked, inline, one-block charged payload,
one install/RETIRING grant, preallocated fixed records and structurally valid
private account records. Account database hashes, capability and authorization
pins are synthetic fixture policy; signatures and real account-service coherence
are not established. General inventory, multiple victims, deep/fragmented extents,
credential mutation, escrow release, retirement-record clearing, IDLE, competing
writers and production mount integration remain outside this gate.

The fault disk models sector-atomic write/flush interruptions with four persistence
modes and faults before/after I/O. It does not establish torn-sector handling,
hardware durability or controller behavior. Linux e2fsck and independent debugfs
record/hash checks audit retained outputs. Host tests establish no boot, Ring 3,
SMP, scheduler/signal teardown or physical acceptance. Source and shared-media
preservation are separate checks and do not imply boot acceptance.


## Results (2026-10-23)

- Six-geometry ASan/UBSan PASS: `pkg-retirement-journal-20261023-183620-405556`. 1/2/4 KiB blocks
  x 512/4096-byte sectors; first/middle/last I/O boundaries for unlink,
  final close and constructor recovery, four persistence modes and faults
  before/after I/O: **432 interruptions**, 864 constructor restarts, 12 Linux audits.
- Exhaustive ASan/UBSan PASS: `pkg-retirement-journal-20261023-183641-171405`. All write/flush boundaries
  for those three operations with 4 KiB blocks and 4096-byte sectors:
  **1384 interruptions**, two restarts each and two Linux audits.
  Other geometries were not exhaustively swept.
- Total: **1816 interruptions**, **3632 constructor restarts**, **14
  retained-output Linux audits**. All cuts receive complete-record and allocation
  validation in process. Linux audits cover the normal-refunded and final-recovered
  images per geometry, not every cut.
- Grant sequences 4/5/6 and account sequences 51/52/53 represent unlink, block
  refund and inode refund. Q: 1/1 -> 1/1 -> 0/1 -> 0/0. G: 7/0 -> 7/0 ->
  8/0 -> 8/1. T stays 8/1; F changes 0/0, then +1/0, then +0/1. Credential
  generation stays 9. Second constructor restart must preserve account sequence.
- Maximum staged images: 18/14/12 for 1/2/4 KiB blocks, below the 64-image bound.
  Independent debugfs checks verify all five checksums, full hash bindings,
  sequence correspondence, zero selected Q, tombstone class and absent payload.
  All retained-output e2fsck -fn runs exit zero.
- Final manifests report unchanged compiled copies, repository sources and
  guarded files. AGENTS.md, README.md and shared ELF/initramfs/ISO/raw-image
  hashes are preserved separately. This establishes no boot acceptance.
  Summary: `.codex-remote-attachments/pkg-retirement-journal-summary-20261023/manifest.json`.

Bounded matrix: `make -f tests/pkg_grant_candidate.mk test-pkg-retirement-journal`.
Exhaustive recorded geometry: `python3 scripts/test_pkg_retirement_journal.py --block 4096 --sector 4096`.
The CLI without geometry/cut options requests exhaustive sweeps across all six
geometries; that broader campaign was not run here.

## Retained failures

- `pkg-retirement-journal-20261023-183243-804974`: compile FAIL; nonexistent
  VFS node inode field, corrected to the actual EXT4 private inode identity.
- `pkg-retirement-journal-20261023-183340-797629`: 144 selected 4 KiB cuts
  passed at runtime, but overall preservation FAIL because the adapter changed
  during the run. Raw FAIL and changed-source list retained; excluded from counts.
- `pkg-retirement-journal-20261023-183507-574533`: compile FAIL; a generated
  newline substitution missed the second-restart assertion, leaving unused
  variable/function. Fixed the substitution and asserted its generated presence.
- Initial Python generation failed syntax validation before creating a campaign;
  tool output remains in the conversation. No success claimed for that invocation.

Next: authenticated retirement provenance and binding this format to the private
pending/trust and account-service profile, with refusal tests, before mount
integration. Synthetic capability/account pins do not establish that authority.
All changes remain uncommitted. Production integration, installation, trust
provisioning, release-media builds, pushes and physical-device writes remain disabled.


Continuation: [authenticated private retirement provenance](PKG2_RETIREMENT_PROVENANCE.md) now verifies the signed certificate and retained enrollment/catalog/artifact/intent against independently supplied policy. It remains a separate pure gate; this JBD2 adapter still needs durable certificate/evidence storage and authenticated filesystem binding before mount integration.
