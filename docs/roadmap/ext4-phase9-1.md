# EXT4 Phase 9.1 — Transaction fault-model foundation

Implemented and host-verified on 2026-10-04. This completes the calibrated model,
mounted event inventory and initial campaign freeze in
[the 9.1 specification](../plans/EXT4_PHASE9_1.md). It does not complete Phase 9
or E4-B acceptance. Production journaled RW remains disabled.

## Implementation

- Independent Python stable/volatile sector model: cached and verified
  write-through behavior, deterministic early persistence/reordering, before/after
  write/flush failure, selected partial flush, prefix tears and disconnect.
- False flush success and omitted barriers are negative controls, never admitted
  durability profiles. Failure acknowledgements do not imply rollback.
- Actual VFS/EXT4/JBD2 mounted-operation inventory with staged home-slice auditing,
  exact payloads, writer phase/sequence/image counts and sector RMW boundaries.
  A library guard reuses the existing integration harness without changing its
  normal executable behavior. No production filesystem or driver was modified.
- Independent trace reconstruction and stable-byte barrier checks. Unchanged
  neighboring old commit records in a 4096-byte RMW sector are distinguished from
  newly submitted records. Removing each real commit flush must fail the checker.
- Read-only Linux complete reachable namespace/type/link and data-block ownership
  observations, exact expected file bytes and unmounted fsck. New empty regular
  files and directory sizes receive a separate independent audit.
- Six deterministic persistence profiles and trace-bound cut counts exported for
  the next campaign. Counts describe proposed work, not successful crash cases.

## Verification and retained evidence

Accepted inventory:
`.codex-remote-attachments/ext4-phase9/foundation-5aprl4_q`.

| Gate | Actual result |
| --- | --- |
| Model calibration | Nine test groups PASS; all 4,606 nonempty proper prefixes across 512/4096-byte sectors, atomic before/after outcomes, partial failed flush, disconnect, bounds, stale/aliased/mixed snapshot and false-flush controls |
| Mounted baseline inventory | 12 geometry/placement configurations × 13 operations = 156; 9,736 events, including 8,536 writes and 1,200 flushes; ASan/UBSan and strict warnings PASS |
| Exact reconstruction | All 156 operation-end stable images reproduced from preimages and recorded payloads; complete images gzip-retained and decompression hash-checked |
| Barrier negative controls | 228 omitted real commit flushes detected; every metadata-mutating baseline has a detected control |
| Linux baseline integrity | 156 unmounted `e2fsck -fn` exits 0, exact known file bytes, complete reachable names/types, links and ownership observations PASS |
| New inode audit | 24/24 created empty regular-file/directory size and identity observations PASS |
| Independent oracle controls | Modified file data passes fsck but fails exact byte oracle; clearing an allocated bitmap bit produces uncorrected bitmap/count/checksum errors, fsck exit 4, with the image unchanged |
| Existing integration regression | Shared non-append offsets and independent append: 1 KiB/4096-sector and 4 KiB/512-sector ASan/UBSan PASS; four independent Linux record/clean-state/fsck audits PASS |
| Tooling checks | New Python modules compile; scoped whitespace checks PASS |

Negative-control evidence:
`foundation-5aprl4_q/oracle-negative-0oakrw25`.
Regression evidence:
`.codex-remote-attachments/ext4-phase9/regression-my74_3on`.
The accepted inventory manifest records source and binary hashes, input hashes,
events, observed snapshots, compressed-image hashes and command logs. Supplementary
verification preserves versions and the full relevant source snapshot.

Exact primary invocations:

```sh
make test-ext4-crash-model-host \
  EXT4_INTEGRATION_FIXTURES=.codex-remote-attachments/ext4-phase8-5/host-frz8r6nu
python3 scripts/test_ext4_crash_campaign.py \
  .codex-remote-attachments/ext4-phase9/foundation-5aprl4_q
python3 scripts/test_ext4_crash_oracle.py \
  .codex-remote-attachments/ext4-phase9/foundation-5aprl4_q
```

Early exploratory inventory and two failed harness runs remain retained and are
excluded from accepted totals. One parser initially included empty/deleted
debugfs directory slots; another record classifier included unchanged historical
commit neighbors in sector RMW. Both harness errors were corrected before the
complete accepted run. No filesystem implementation fix was required.

## Next campaign and limits

The frozen initial 9.2 plan contains **116,832 atomic cuts**, **187,792 targeted
write tears** and **28,800 partial-flush schedules**: **333,424 planned cases**.
None is claimed executed by this milestone. Budgets and exact profile definitions
are in the specification and retained `coverage-plan.json`.

This gate inventories successful mounted operations and tests the independent
model/checkers. It does not yet feed every new fault-model image through actual
FortressOS recovery. Same-sector superseded-version persistence is excluded;
targeted campaign tears do not cover every possible field alignment. Fragmented
trees, maximum credits, recovery cuts and valid multi-step orphan boundaries must
extend the initial matrix in 9.2/9.3. The exported data-block map is not a complete
independent raw metadata/extent validator; Linux fsck supplies that integrity check.

No QEMU power-cut, journaled USB, physical crash or production-rollout claim.
Dell 5590/5530 and two USB sticks are available by user report; designated media,
durability characterization and the separate physical procedure remain later work.
E4-A acceptance, default ext2, metadata cache, barriers, DMA quarantine and
protected synchronization are preserved. Next implementation unit: **9.2**.
