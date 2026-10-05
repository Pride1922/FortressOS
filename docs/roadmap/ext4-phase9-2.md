# EXT4 Phase 9.2 — Mounted transaction crash evidence

2026-10-05; verification in progress. See the
[execution specification](../plans/EXT4_PHASE9_2.md). Production journaled RW
remains disabled. No guest, USB or physical acceptance is claimed here.

## Implementation

The recovery worker uses the actual VFS, EXT4 and JBD2 implementation under
ASan/UBSan, with ranked host lock adapters and an explicit disposable device.
Each distinct stable image gets a fresh mount and fresh cache, descriptor and
allocator state. The independent Python persistence model and journal parser
select a single expected durable prefix; mount rejection passes only with a
specific corruption witness and zero writes/flushes/publication.

The campaign found a torn primary group-descriptor checkpoint that prevented
bootstrap before the intact committed journal could be examined. Recovery-only
bootstrap now borrows a checksummed group-1 descriptor backup in memory, after
matching immutable bitmap/inode-table addresses. It does not repair primary
media. The existing authoritative replay preview must still validate metadata
and ownership before recovery I/O or mount publication. Ordinary RO/E4-A
admission retains strict primary-descriptor checks.

The cache, taint, transaction ownership, durability barriers, production mount
gates, default ext2 path, DMA quarantine and synchronization contracts are
preserved. Kernel compilation and linking passed after the change.

## Primary campaign

Retained evidence: `.codex-remote-attachments/ext4-phase9/campaign-19ycgaya`;
immutable inventory: `foundation-5aprl4_q`. Final process exit was 0, with an
empty manifest error list. Runtime: 3,301.48 seconds.

| Measurement | Result |
| --- | ---: |
| Complete operation/geometry/placement traces | 156 |
| Declared schedules checked | 333,424 |
| Recoverable schedule outcomes | 331,796 |
| Independently witnessed corruption rejections | 1,628 |
| Distinct stable inputs receiving actual fresh mounts | 32,213 |
| Distinct recovered home images independently audited by Linux | 3,226 |
| Input reconstruction hash samples | 6,554 |

Ledger verification checked every declared identity in order, including
byte-identical schedules, every retained Linux audit delta, duplicate result
agreement and exact expected coverage. This is not 333,424 distinct mounts or
333,424 separate Linux audits. Linux audit reuse requires exact home-sector
equivalence after successful clean/empty-journal assertions in the worker.

## Controls and additional verification

- Sparse/full persistence-model equivalence: 552 schedules, six profiles and
  both sector sizes, PASS.
- Recovery-oracle prefix/corruption controls: 48, PASS (`oracle-nkuxz5uf`).
- Descriptor-backup identity/checksum/authoritative-replay controls: 5, PASS
  (`bootstrap-iue0th5g`); four rejection variants issue zero writes/flushes.
  A valid backup alone cannot admit a torn primary without journal authority.
- Existing Linux stale-byte and allocation-disagreement negative controls,
  plus 24 empty-file/new-directory inode audits: PASS
  (`foundation-5aprl4_q/oracle-negative-ev65yh23`).
- Mounted staging/credit injections, shared offsets and independent append:
  12/12 geometries, 6,712 injections, PASS
  (`ext4-phase8-6/staging-v8fvfe13`).
- Fragmented fixture construction: six actual mounted fixtures, three
  interleaved extents, exact bytes and Linux ownership/fsck, PASS
  (`fragment-6b8lwd9f`). Their separate 156-trace inventory passed
  reconstruction, barrier checks and Linux audits (`foundation-4j8k_43_`).
  Independent extent inspection confirms all 12 extending-write baselines
  promote the inode root to an external leaf containing five extents.

The fragmented extending-write campaign passed all 12 geometry/placement
combinations (`campaign-jhtf4w3a`): 22,244 schedules, 22,168 recoverable outcomes,
76 witnessed corruption rejections, 4,119 distinct fresh-mount inputs and 1,488
Linux audits. Runtime: 790.55 seconds. Its ledger audit checked all schedules,
478 deterministic input reconstruction hashes and all 1,488 retained Linux
audit deltas. It retained the actual recovery worker and a source snapshot.

Together the campaigns cover 355,668 schedules, 36,332 distinct stable recovery
inputs and 4,714 distinct Linux home-image audits. Keep the two manifests and
their counts separate when reproducing them.

Mounted latest-VFS/cache/lifetime/admission/freeze/concurrency smoke passed
12/12 (`ext4-phase8-5/latest-vfs-b95hmu7o`), including immediate directory mapping
refresh after create and rename growth and eight write/freeze races per profile.

Both regression batches finished with exit 0: transaction foundation, journal
file mutation, E4-A write/read/format, ext2, BOT and USB mount policy, JBD2
recovery/writer, namespace, orphan/deep cleanup and allocation. Namespace passed
12/12 with 324 Linux copies; orphan coverage passed 12/12 with 288 Linux copies.
The full depth-2 cleanup check and maximum 4096-extent allocation checks passed.
The separate retained policy log is `policy-pve24ns6/host.log`.

This is a checkpoint, not Phase 9.2 acceptance. The additional direct mounted
depth-1-to-depth-2 promotion fixture/campaign has not run. Its fixture setup and
the final evidence consolidation remain outstanding. No test is currently
running at this checkpoint. Phase 9.3 has been authorized but not started.

## Reproduction

Run in WSL Ubuntu-24.04 from `/mnt/c/Sources/FortressOS`, using only these explicit
disposable fixtures:

```sh
make test-ext4-crash-host EXT4_CRASH_INVENTORY=.codex-remote-attachments/ext4-phase9/foundation-5aprl4_q
make test-ext4-crash-controls-host EXT4_CRASH_INVENTORY=.codex-remote-attachments/ext4-phase9/foundation-5aprl4_q
python3 scripts/verify_ext4_crash_ledger.py .codex-remote-attachments/ext4-phase9/campaign-19ycgaya
make test-ext4-crash-fragment-host EXT4_INTEGRATION_FIXTURES=.codex-remote-attachments/ext4-phase8-5/host-frz8r6nu
python3 scripts/test_ext4_crash_recovery.py .codex-remote-attachments/ext4-phase9/foundation-4j8k_43_ --operation write
make test-ext4-integration-staging-host EXT4_INTEGRATION_FIXTURES=.codex-remote-attachments/ext4-phase8-5/host-frz8r6nu
make bin/fortress.elf
```

Earlier exploratory failures remain retained, including the discovered torn
descriptor and an overly narrow test expectation that allowed EIO but omitted
the implementation's supported EOPNOTSUPP corruption rejection. These are not
counted as accepted complete campaigns.

## Limits

“Exhaustive” means the declared finite schedules. The model persists latest
accepted sector versions, not arbitrary superseded historical versions. Visible
file-data overwrites are not promised atomic. Uncertain and torn I/O can produce
detectable durable corruption and is distinguished from recoverable input.
Focused legacy extension fault schedules remain separate from the six-profile
matrix. Recovery interruption, interoperability, guest crash, USB and physical
acceptance gates remain 9.3–9.6. This work enables no production journaled RW.
