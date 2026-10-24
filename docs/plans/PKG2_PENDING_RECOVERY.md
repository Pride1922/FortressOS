# Private pending recovery and durable reservations

2026-10-24. Gate 1 continuation. This remains a copied-kernel TEST profile with
an unpublished, detached filesystem. Production EXT4, syscall dispatch, boot
selection, account authentication and public package mutation stay disabled.
The original production integration plan is preserved.

## Implemented boundary

`tests/pkg_pending_candidate.c` authenticates the immutable selector/baseline,
the signed single-file artifact, canonical target inventory, current grant,
ownership table, intent and account bindings. Native observations refine every
charged row with inode/generation, mode, owner, link count, size, content hash,
dense inline extents and actual filesystem capacity/free counts. Unknown names,
unowned transaction files, overlapping/protected maps and mismatched charges
refuse. The permanent artifact is a retained baseline proof, not refundable
transaction storage.

The fixed install-recovery plan admits four charged roles: archive, activation,
next inventory and intent. Its authorized budget is derived from artifact and
executable sizes plus fixed bounded metadata credits; it is not adopted from a
disk-supplied total. Ordinary allocation must retain remaining package credits
and the separate 256 KiB/16-inode account reserve. This is not a generalized
package allocator or an installation API.

RESERVED aborts before publication. PENDING completes either staged activation,
already-published activation or inventory publication. RETIRING removes the
known transient roles. Activation rename and inventory publication share a
journal transaction. Every cleanup transaction binds actual block/inode frees
to sealed ownership, grant and account updates. Final executable ownership
transfers to baseline without freeing its blocks; releasing unused credits also
does not free storage. Synthesizing the next inventory from authenticated
baseline/artifact proof permits deletion of its temporary copy.

`tests/pkg_pending_fs.inc` wraps the existing semantic JBD2 commit in the copied
translation unit. It checks actual planned free-count deltas and exact released
maps/inode before stamping records into the same transaction. A bounded private
non-orphan removal planner shares existing bitmap, extent, inode, directory and
checksum helpers. It does not toggle production orphan policy. Ordinary plans
are fenced from protected content, inode identities and allocated bitmap bits.

The retained raw engine is explicitly detached and uses existing filesystem
rank-1 exclusion; no root or account service is published. Native boot verifies
the journal's projected state before replay, then reopens and revalidates home
state. Private Ring 3 recover rechecks credentials and owner cancellation
between durable steps outside filesystem exclusion. The operation reference
remains pinned until the callback returns. A failed copyout cannot rerun it.

## Evidence

All directories below are beneath `.codex-remote-attachments/`. Manifests retain
every compiler/runtime command, copied input hashes, overlays and per-case logs.
Boot acceptance and repository/build-output preservation are separate results.

| Evidence | Runtime acceptance | Preservation |
| --- | --- | --- |
| `pkg-pending-20261024-182509-217854` | ASan/UBSan: 89 pure vectors (5 accepted, 84 refusals), 5 actual EXT4 recovery/reopen/cut/repeat cases, Linux audits; 128 owner lifetimes plus between-step credential/exec/close/exit/fresh-reuse checks; freestanding compile PASS | Full guard FAIL: only `scripts/test_pkg_pending_limits.py` changed while the campaign ran; compiled copies unchanged |
| `pkg-pending-limits-20261024-182603-007708` | Actual physical block/inode exhaustion at `remaining + 64 blocks / 16 inodes` for RESERVED/PENDING/RETIRING; ordinary allocation and reserved-content writes refused before I/O; real duplicated handle pins deny recovery before lifecycle writes; final release restores ordinary allocation; Linux checks PASS | PASS |
| `pkg-pending-cuts-20261024-182828-553692` | 165 cuts before/after every observed flush plus selected 256-byte sector tears with preceding writes persisted, across RESERVED/PENDING/RETIRING; fresh projected authentication, recovery, repeat, byte-identical whole disk excluding journal storage, and Linux fsck PASS | Full guard FAIL: concurrent iwlwifi/shared outputs and two evolving boot harness scripts; compiled copies unchanged |
| `pkg-boot-admission-20261024-182933-306648` / `recovery-manifest.json` | Final pin/taint implementation: 40 BIOS/UEFI × SMP=1/4 boots; five phases/windows, recovery plus subsequent boot; actual phase/sequence UART values checked, native whole-disk oracle and Linux checks PASS | Full guard FAIL: concurrent `iwlwifi.c`, shared outputs, and the explicit reuse wrapper change; copied inputs unchanged |
| `pkg-pending-boundaries-20261024-183557-308664` | 17 boots: nine zero-write admission refusals plus authenticated pending-state projected journal replay/recovery/reboot across BIOS/UEFI × SMP=1/4 PASS | PASS |
| `pkg-current-20261024-183546-919654` | 43 existing baseline/current regression vectors, native readback, independent Linux checks PASS | PASS |

The nine native refusals cover forged charges, forged authorized totals,
premature IDLE with live transaction objects, unknown transaction names,
activation generation/hard-link mismatch, wrong next inventory, actual block
capacity below the retained floor, and an invalid projected pending journal.
Every refusal preserves the whole image with zero writes/flushes. Valid pending
journal replay is tested against the independent native complete-disk oracle,
not mistaken for package-level completion alone.

`pending-fs-frames.json` in the final boot directory audits 21 private filesystem
functions, including the commit wrapper: maximum 448 bytes, all <=1024 bytes.
The ordinary linked verifier frame audit retains the previously approved
1280-byte ceiling for `mono_crypto_eddsa_check_equation`; no new exception.
The linked-symbol audit rejects signing/Argon2/entropy glue. No release image
target was used. Private ISO construction and NVMe targets are regular files.

Exact top-level Linux commands, run through WSL Ubuntu-24.04 from the repository:

```sh
python3 scripts/test_pkg_pending.py
python3 scripts/test_pkg_pending_limits.py /mnt/c/Sources/FortressOS/.codex-remote-attachments/pkg-pending-20261024-154306-490657
python3 scripts/test_pkg_pending_cuts.py /mnt/c/Sources/FortressOS/.codex-remote-attachments/pkg-pending-20261024-154306-490657 --jobs 2
python3 scripts/test_pkg_pending_boot.py --native /mnt/c/Sources/FortressOS/.codex-remote-attachments/pkg-pending-20261024-182509-217854/pending-native --jobs 4
python3 scripts/test_pkg_pending_boot.py --native /mnt/c/Sources/FortressOS/.codex-remote-attachments/pkg-pending-20261024-182509-217854/pending-native --jobs 4 --reuse-build /mnt/c/Sources/FortressOS/.codex-remote-attachments/pkg-boot-admission-20261024-182933-306648
python3 scripts/test_pkg_pending_boundaries.py /mnt/c/Sources/FortressOS/.codex-remote-attachments/pkg-boot-admission-20261024-182933-306648 --native /mnt/c/Sources/FortressOS/.codex-remote-attachments/pkg-pending-20261024-182509-217854/pending-native
python3 scripts/test_pkg_current.py
git diff --check
```

The first final boot invocation completed compilation/measurement/frame checks
but stopped before runtime on the preservation assertion. Explicit reuse checks
every saved copied input and all module measurements, rejects a failed build,
and retains the original build manifest. The reused campaign then completed all
40 boots; its final nonzero exit is solely the retained preservation assertion.
Host and cut commands with full preservation FAIL likewise return nonzero even
when runtime PASS. None of these failures is relabeled PASS.

`preservation-reconciliation.json` in the final host directory independently
confirms unchanged package/EXT4/process/credential sources, protected contracts,
original plan and saved copied inputs. It does not claim shared build outputs
were preserved. The original plan SHA-256 remains
`f1e8a769becaac55509a82a1633fda9c6057167588becab8c9cba91521fb1017`.
Concurrent Wi-Fi work and its shared builds were not staged or reverted.

`phase-audit.json` in the boundary directory records actual UART phase/sequence
values; the inherited runner's original `sequence=1` result placeholder is
retained in its old files. The helper now records actual values directly;
offline parsing of all 17 saved UART/results and import of that reporting-only
change PASS. The final commit-wrapper frame filter was also broadened after
the independently retained frame audit. These reporting-only changes do not
alter the tested C/kernel implementation.

Retained development failures beneath `.codex-remote-attachments/`:

- `pkg-pending-20261024-153025-813901`: strict compile refusal for an unused
  helper. The helper now enforces canonical unused extent slots. Preservation
  also failed during concurrent shared builds; no runtime acceptance occurred.
- `pkg-pending-20261024-153259-389654`: RESERVED fixture incorrectly retained
  genesis next-state hash/generation and was correctly refused. The original
  runtime/preservation failures remain recorded.
- `pkg-pending-20261024-153729-760494`: runtime failure in the model harness,
  which retained the executable in charged observations after final transfer.
  The filesystem path independently passed that transition; the model now
  explicitly moves the executable to its baseline observation. Preservation PASS.
- `pkg-pending-limits-20261024-154606-232616`: runtime FAIL because the detached
  clean test fixture had not entered the journal writable lifecycle before
  exercising ordinary allocation. The corrected harness authenticates first,
  journals the lifecycle transition, then measures zero writes for denied
  ordinary operations. Preservation PASS; the original failure remains.
- `pkg-boot-admission-20261024-153752-886470`: earlier 10-boot BIOS smoke PASS/PASS.
- `pkg-boot-admission-20261024-154324-855442`: earlier 40-boot PASS/PASS before the
  final explicit pin/taint refusals and phase-reporting changes. Superseded by
  the final 40-boot runtime above, not used to relabel its preservation result.
- `pkg-pending-20261024-154306-490657`: earlier combined host PASS/PASS, retained
  as the signed fixture corpus used by later limits/cut suites. Those suites
  copy the current recovery implementation explicitly instead of silently
  compiling the older implementation from this corpus.

## Limits and remaining gates

One `demo` executable, one build/catalog and immutable accounts/trust only;
artifact and executable each at most 8192 bytes, dense depth-zero maps with at
most four extents, four charged roles and grant sequence at most 64. No account
UPDATE, documentation members, upgrade/remove, trust rotation or general package
history is supported. The initial external floors authorize the bounded recovery
lineage; this does not implement independent monotonic floor advancement or
rollback protection against replacing the entire disk with an older admitted
snapshot. The immutable baseline/catalog must still be acceptable.

Host power-loss models and QEMU cannot establish physical durability. New
multi-step recovery still needs integrated real signal/reaper, credential-change
and copyout-fault campaigns; host owner checks do not establish those scheduler
outcomes. No new AP-placement or physical-device acceptance is claimed. Broader
root/account-service publication, incompatible-kernel exclusion, complete typed
install/remove plans and all production trust/operational decisions remain.
Detached close does not mark the filesystem clean. Read-only `e2fsck -fn` may
report that it is skipping journal recovery; PASS here means structural checks
and oracle comparisons succeed, not that the clean bit or a production root
publication was accepted.

Installation, production trust provisioning, release-media builds, pushes and
physical-device writes remain disabled.
