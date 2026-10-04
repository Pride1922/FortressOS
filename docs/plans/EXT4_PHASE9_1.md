# Phase 9.1 — Fault model, oracle and campaign freeze

Implementation scope: host model calibration and inventories of actual mounted
operations. Exhaustive fault injection into mounted operations starts in 9.2.
The new model does not replace Phase 6–8 fault adapters or alter production code.

## Device semantics

`scripts/ext4_crash_model.py` separates stable media from volatile sector images.
Reads see the latest accepted volatile image. Successful working flush persists
all dirty sectors. Restart discards volatile state and outstanding schedules.
Write/flush failure disconnects the device; later callbacks cannot progress.
Failed acknowledgement may follow persistence and never implies rollback.

| Fault | Defined effect | Expected classification |
| --- | --- | --- |
| Before write | No acceptance of the selected write; disconnect | Recovery according to earlier stable transactions |
| After write | Accept to volatile cache; selected profile may also persist; disconnect | Uncertain acknowledgement; no assumed rollback |
| Before flush, no persistence | Lose pending volatile writes | Recover earlier durable prefix |
| Failed partial flush | Persist explicitly selected dirty sectors, then disconnect | Recover valid prefix or detect durable corruption, according to specific bytes |
| After flush | Persist all accepted dirty sectors, then fail acknowledgement | Committed dependencies survive; caller still sees failure |
| Sector tear | Replace an explicit prefix in stable media, retain old suffix; disconnect | Recover or reject detectable corruption; no arbitrary reconstruction promise |
| False flush success | Return success without persistence | Negative control only; cannot support a durability claim |

Selected early persistence always uses the latest accepted image of a sector.
It can reorder persistence of distinct sectors; it does not model obsolete
versions overwriting newer versions of the same sector. Flush tears are modeled
by sector selection; prefix tears apply to a selected write. Whole-sector atomic
before/after outcomes remain separate. These are declared model limits.

## Independent checks and event inventory

`tests/ext4_crash_inventory_host.c` reuses the actual mounted VFS/EXT4/JBD2
integration harness and ranked pthread adapters. It records all operation writes
and flushes, writer sequence/state/staged image usage and filesystem-block slices.
Changed home slices remain subject to the existing staged-image coverage audit.
In 4096-byte sector RMW, unchanged neighbors do not identify newly submitted
journal records. Each event points to retained exact sector payload bytes.

Record a stable pre-operation image, operation-end stable image and separate
orderly-clean image. Independently replaying the event payloads through the Python
cached model must reproduce the exact operation-end image. This calibration
proves trace reconstruction, not crash recovery. Preparation, mount activation
and the later close/freeze are outside each operation trace; 9.3 owns exhaustive
recovery and mount lifecycle cuts.

The barrier checker uses stable byte equality: ordered data and prior log slices
must be durable before the new commit record; a complete commit must be durable
before metadata-home writes; checkpoint metadata must be durable before journal
retirement. Remove each observed commit flush as a negative control: the checker
must fail before premature checkpoint publication. Writer state alone is not
accepted as durability evidence.

Linux audits are independent of FortressOS in-memory validators. On each completed
clean baseline, unmounted `e2fsck -fn` must exit 0 without repair. `debugfs` checks
exact bytes, complete reachable names/types, links, inode identities and allocated
data/directory block ownership; duplicate ownership is rejected. Traditional
orphans must be empty after drain. Fsck also checks allocation/tree/metadata
integrity beyond the explicitly exported data-block map. This is not a standalone
independent raw extent/checksum implementation.

Allowed snapshots are complete states, not fieldwise unions of old/new states.
Negative controls cover mixed namespaces, stale bytes, ownership aliases, bad
links and residual orphans. Disposable Linux controls corrupt file bytes (which
can pass fsck but must fail the byte oracle) and bitmap ownership (which must fail
read-only fsck). Actual interrupted orphan cleanup can admit valid intermediate
states: 9.2/9.3 must enumerate them before running those cases, and must assert
durable commit outcomes rather than allowing old/new indiscriminately.

## Frozen initial matrix for 9.2

Thirteen baseline operations: create, mkdir, write, append, rename, truncate,
unlink, open-unlink, last close, rmdir, reuse, sync and freeze. Geometry:
1/2/4 KiB blocks × 512/4096-byte sectors × normal/wrapped journal placement.
This gives 156 recorded operation baselines. Fragmentation, depth transitions,
maximum credits and recovery/orphan histories extend this initial matrix in
9.2/9.3; they are not covered by the simple baseline inventory.

`scripts/ext4_crash_campaign.py` freezes schema 1 schedules:

| Profile | Early persistence before successful flush |
| --- | --- |
| cached | None |
| write-through | Every accepted sector |
| early-low | Lowest dirty LBA every fourth accepted write |
| early-high | Highest dirty LBA every fourth accepted write |
| odd-writes | Current sector on odd numbered accepted writes |
| even-writes | Current sector on even numbered accepted writes |

Schedules use event indices and LBA ordering, with no random seed or dependency
on a host PRNG. Every write/flush gets before/after cuts under all six profiles.
For each write, test prefix lengths 1, 2, 4, 8, 12, 16, 24, 32, sector/2,
sector-4 and sector-1 under cached and write-through histories. These lengths
sample field and sector boundaries; they are not every possible filesystem-field
tear. Calibration separately checks every prefix of both supported sector sizes.
For failed flushes, each profile gets no dirty sectors, lowest dirty sector,
highest dirty sector and all dirty sectors in descending order persisted before
failed acknowledgement. Some schedules coincide for short pending sets; retain
their identities rather than claiming distinct resulting images.

`coverage-plan.json` binds counts to trace hashes. Planned counts are never test
passes. Future runners must report attempted/completed/rejected cases, reject
missing/duplicate cuts, and bind each expected result to the durable operation
boundary. Recoverable cases must recover; generic mount rejection cannot pass.
Preflight durable-corruption rejection must write nothing and publish no RW mount.
Failures after admitted recovery writes retain the declared taint/error semantics.

## Budgets, artifacts and physical handoff

Initial 9.2 batches: at most 2,000 cases, 2 GiB working images and 8 GiB retained
evidence. Check capacity before launching; stop and retain failures on exhaustion.
Benchmark the first batch to set a bounded execution deadline before expansion;
no runtime estimate is an acceptance claim. Use immutable compressed preimages
and exact event/sector deltas, verifying reconstructed hashes. Retain full failing
images and logs. Do not delete evidence to turn a failed run into a successful one.

The 9.1 runner uses fresh directories under
`.codex-remote-attachments/ext4-phase9`, 180-second subprocess limits and only
explicit regular-file fixture inputs beneath retained disposable evidence.
It compresses and verifies complete baseline images after audits. No physical
disk, QEMU, general build, mount, fsck repair or image-format conversion is invoked.

Available for later manual acceptance: Dell 5590 or 5530, one slower USB 2.0 stick
and one faster USB 3.0 stick (user report, 2026-10-04). Device availability is not
physical acceptance evidence or permission to select/erase an unidentified disk.
Prepare the designated-media procedure at 9.6. Production journaled RW stays off;
E4-A, default ext2, barriers, cache, quarantine and protected locking stay intact.

## Invocation

```sh
make test-ext4-crash-model-host \
  EXT4_INTEGRATION_FIXTURES=.codex-remote-attachments/ext4-phase8-5/host-frz8r6nu
python3 scripts/test_ext4_crash_campaign.py <completed-9.1-evidence-directory>
python3 scripts/test_ext4_crash_oracle.py <completed-9.1-evidence-directory>
```

The first target builds only the isolated host inventory with ASan/UBSan and strict
warnings, runs model calibration and performs the Linux baseline audits. The
second calibrates schedules and exports proposed cut counts. The third checks
independent Linux negative controls on new disposable copies. Results and exact
retained run identities belong in the Phase-9.1 roadmap record.
