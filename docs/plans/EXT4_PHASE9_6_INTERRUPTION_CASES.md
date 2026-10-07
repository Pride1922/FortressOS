# Phase 9.6 — Physical interruption cases

Each case needs separate approval, a hashed artifact and verified instrumentation
on the identified Dell 5590 / disposable 4 GB Generic Flash Disk. Production
journaled RW remains disabled. Only case 1 is currently implemented and tested.
Cases 2 and 3 specify required work; they are not ready for physical execution.

## Common procedure

Recheck target identity, capacity, PARTUUID, GPT and durability. Disconnect other
removable disks; exclude internal NVMe. Preserve baseline bytes/allocation sets.
Only at the exact marker, the proposed action is holding the power button until
off, then unplugging the stick to remove standby USB power. No random timing,
active-transfer unplug, sync, normal shutdown or reboot at the pause.

Before ANY recovery boot, capture read-only from Administrator PowerShell:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File C:\Sources\FortressOS\scripts\capture_ext4_physical.ps1
```

Preserve/hash the original. Linux replay/repair operates only on a copy. Record
commands, exit codes, dumps and ownership checks. Then boot the original stick
with its case-specific recovery entry, without reflashing or Linux repair.
Check the expected result before further mutation, sync/shutdown, capture again
and audit bytes, namespace, allocation, clean state, journal and orphan chain.
Missing marker, failed barrier, taint or transport error stops the case.

## 1. Durable commit before checkpoint — implemented, physical approval pending

See [the executable commit-cut procedure](EXT4_PHASE9_6_COMMIT_CUT.md).
COMMIT PAUSE creates `/mnt/cut-commit.txt`; the hook terminally pauses after
successful commit/barrier and before the first checkpoint write.

Marker: `EXT4 TEST PAUSED: COMMIT DURABLE; CHECKPOINT NOT STARTED`.
Home metadata must lack the file; the journal must contain its committed change.
Linux replay on a copy and FortressOS recovery on the unrepaired stick must each
produce one regular empty file and preserve existing downloads/namespace.
Pre-recovery audit: `python3 scripts/audit_ext4_commit_cut.py <capture>` in WSL.
Exact-artifact BIOS/UEFI tests passed; no physical interruption has occurred.

## 2. Interrupted recovery — hook, artifact and audit pending

Prepare a valid pending journal containing at least two distinct unrevoked home
images with recorded old/final hashes and expected namespace/bytes. Required
test-only hook: write the first selected replay home image, successfully flush
it, then terminally pause BEFORE the second selected home write and BEFORE
clearing the journal. No mount publication, fixture rewrite or outstanding DMA.
A failed write/flush must never emit the marker.

Marker to implement:
`EXT4 TEST PAUSED: RECOVERY PARTIAL DURABLE; JOURNAL RETAINED`.
Use an explicit RECOVERY PAUSE entry. Normal recovery disables the hook.
The operator cuts at the marker, not while trying to catch a short recovery.

Capture must show first selected home image final, second old, journal retained.
Linux replay on a copy and fresh FortressOS recovery must converge to the full
committed result: exact bytes, no missing/duplicate namespace or allocation
conflict. A second clean recovery boot must preserve that result. Record the
added test barrier: this exercises a durable partial-replay boundary, not all
in-flight tears. Implement the capture audit and publish exact commands/hashes;
verify BIOS/UEFI pause, restart and idempotence before requesting approval.

## 3. Durable open-unlink — hook, artifact and audit pending

Create/checkpoint `/mnt/cut-open-unlink.bin` with deterministic bounded data.
Record inode, payload SHA, exclusively owned blocks and free-count baseline.
Retain an open reference, unlink the pathname and durably checkpoint the
unlink/orphan transaction. Pause BEFORE closing the reference or reclaim/reuse.

Marker to implement:
`EXT4 TEST PAUSED: OPEN UNLINK DURABLE; REFERENCE RETAINED`.
Emit only after pathname absence, zero links, durable traditional orphan-chain
membership and successful reading through the retained reference are verified.
All barriers must succeed. The pause retains the reference, performs no further
I/O or scheduling and has no outstanding DMA.

Capture must show absent pathname, zero-link orphan and still-owned payload
blocks. On restart no live reference survives: Linux recovery on a copy and
FortressOS orphan recovery must reclaim inode/owned blocks exactly once, empty
the orphan chain, leave the name absent and preserve unrelated files. Compare
allocation sets/counts with the baseline allowing only recorded fixture changes.
Bounded reuse and a second recovery boot must show no double free, leaked orphan
or stale replay into reused blocks. Implement the audit, publish exact commands
and artifact hashes, and verify BIOS/UEFI recovery/reuse before asking approval.
