# EXT4 Phase 9.3 — Recovery, reuse and interoperability

2026-10-06; declared host campaign verified. Production journaled RW remains disabled.
See [execution specification](../plans/EXT4_PHASE9_3.md).

Input preparation reconstructs sixty committed dirty images from the verified
Phase-9.2 primary ledger: write, truncate, open-unlink, last-close and reuse
across twelve geometry/placement combinations. Every selected input matches
its recorded sector-delta hash and verified source-ledger hash.

Actual mounted recovery event inventory under ASan/UBSan passed 60/60, with
exact VFS bytes/namespace, clean freeze, independent Linux dumps/ownership and
unmounted fsck. Events retain every write payload and flush, with publication
state observed at the callback. This baseline measures recovery plus final
freeze; it does not establish recovery-interruption coverage.

Retained evidence under `.codex-remote-attachments/ext4-phase9`:

- `recovery-inputs-ufx64e7r`: sixty compressed inputs and exact source cases.
- `recovery-inventory-rfkdjp5e`: sixty recovery traces, payloads, independent
  Linux logs, compressed clean images and manifest with empty error list.

## Recovery interruption campaign

The native fault worker mounts and freezes actual EXT4/VFS/JBD2, interrupting
every baseline write/flush before and after across cached, write-through,
early-low/high and odd/even persistence. Its resulting stable sector delta must
exactly equal the independent persistence model. Recovery failures before
publication must return EIO without a mount; failed published freeze must taint
the mount. Fresh successful retries check exact committed bytes and namespace,
orphan drainage and clean freeze, then Linux checks equivalent home media.

Each label has a declared second-interruption seed: the middle prepublication
flush, interrupted before persistence under the cached profile. The fresh
retry's complete actual trace is inventoried; every write/flush in that second
trace is interrupted before and after across all six profiles. This is one
seed per label, not the Cartesian product of all first/second interruptions.

Focused `1024-normal-512-write` passed 1,152 actual cuts: 624 first interruptions
and 528 repeated interruptions, 40 distinct successful fresh recovery inputs
and one independent Linux home-image audit. Its independent ledger verifier
reconstructed all 1,152 interrupted inputs and verified complete declared
coverage (`recovery-cuts-4k0pgy_u`). The full sixty-label run passed all 147,912
cuts, with zero errors (`recovery-cuts-f74zd5b0`). Independent ledger verification
reconstructed every interrupted state and verified all sixty Linux audit deltas.
The worker is compiled with ASan/UBSan, strict warnings,
`-O1 -g -no-pie -pthread` and the established host include adapters.

Reproduce with the explicit disposable inventory:

```sh
python3 scripts/test_ext4_recovery_cuts.py .codex-remote-attachments/ext4-phase9/recovery-inventory-rfkdjp5e
python3 scripts/verify_ext4_recovery_cuts.py .codex-remote-attachments/ext4-phase9/recovery-cuts-4k0pgy_u
```

## Multi-orphan, reuse and interoperability evidence

The writer/replay/orphan regression batch completed with exit 0 for the reuse,
revocation, sequence/ring wrap, linked-shrink and interoperability evidence:
writer `build/jbd2-write/run-4mwxu1gi`, replay
`build/jbd2-replay/run-y1crw6cb`, orphan
`build/ext4-phase8-4/run-a7txzk0q`. Each passed 12 profiles; the orphan runner
audited 288 independent Linux copies. Replay retains the Phase-6 raw debugfs
producer and documented zero-tag-UUID normalization boundary. Writer journals
are independently replayed on Linux without that normalization. Recovery and
writer regressions include targeted sector tears, separate from the mounted
atomic six-profile campaign.
Separate real minimum-journal tests use e2fsprogs-created 1/2/4 MiB journals
for 1/2/4 KiB filesystem blocks: exactly 1,024 mapped journal blocks each.
The actual writer/crash/restart and independent Linux replay checks passed on
these fixtures, separately from the existing synthetic four-block ENOSPC map.
The geometry probe retains all nine producer attempts in
`small-journal-probe-i5vf8jye`; smaller requests rejected by mke2fs are not
counted as filesystem fixtures or successful admission tests.

Minimum journals passed 12/12 in `minimum-journal-2xw2pjch`, including wrap,
maximum bounded credits, actual writer interruptions and 36 Linux replay
copies. The extra multi-orphan campaign passed 12/12 and 47,440 first-cut
histories (`multi-orphan-k5y6gw6t`): two simultaneous deleted-file intents and
one linked shrink intent, committed by the actual planner. Every baseline
recovery write/flush is interrupted before/after under four native persistence
profiles. Each history then attempts a second interruption at recovery event
zero; the manifest separately counts cases where that second cut was reached.
Final retries verify the complete allocation census, deleted names, retained
linked file size and zero-write idempotent cleanup. Pending and complete images
receive independent Linux replay, exact retained sparse bytes and clean fsck.
These workbench histories are separate from the mounted six-profile campaign.

The real minimum journal has 1,023 usable log blocks. The current exclusive
writer's bounded transaction occupies substantially fewer, so synthetic
four-block ENOSPC coverage must not be described as exhaustion of a valid
Linux minimum journal. No unsupported profile is enabled to force exhaustion.

## Review and limits

The declared host gate passes: 147,912 mounted interruption schedules with every
stable input independently reconstructed, 47,440 additional multi-orphan
histories, minimum-journal and writer/replay/orphan regressions. Manifest counts
and hashes were checked in `phase9-3-review.json`. Evidence was copied and
hash-verified into `verification-20z0p9k6`, including compressed-image
reconstruction, raw logs, source snapshots, binaries and versions.

This is finite declared coverage, not every pair of interruptions or persistence
history. Mounted second-cut coverage uses one declared seed per label; the
multi-orphan workbench uses event zero and counts actual reached cuts. Sector
tears and reverse Linux interoperability remain the separately rerun bounded
workbench profiles. Real minimum-journal testing does not create an artificial
production journal-full condition. No guest, USB or physical acceptance is
claimed. Phase 9.4 is authorized and its isolated build has passed; guest crash
verification is the next gate. No production mount gate is changed.
