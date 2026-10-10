# Memory investigation: bounded correctness gate

2026-10-09. Starting commit: `26cd041f71bbac7c9833376c7cbb00070bfbd3d0`.
Plan: [memory investigation](../plans/MEMORY_INVESTIGATION_PLAN.md).

## Demonstrated defects and changes

- Actual `heap.c` failed `krealloc(SIZE_MAX)` rejection: unchecked alignment
  wrapped the request and returned success. Requests now reject before resize
  arithmetic/mutation, preserving the allocation and payload.
- Actual heap shrinking failed live-block count equality. The synthetic split
  remainder passed through normal free without first being counted. Shrink
  now counts that temporary block; byte accounting already included it.
- Heap integrity checks now independently count allocated blocks. A deliberate
  counter fault is rejected even when byte totals agree.
- Actual PMM audit accepted a deliberately wrong allocation counter. It now
  holds rank 4 while checking backing bounds, reserved-frame membership and
  bitmap/counter equality; diagnostics run after releasing the lock. Host
  cases cover used/free corruption, null-frame and other reservation corruption,
  and a partial final bitmap byte.
- The PMM boot host runner again compiles the shared test source. Missing
  architecture/kernel/filesystem include paths and process declarations were
  restored. Function-section garbage collection excludes unused lifecycle
  code; no host process/scheduler execution is claimed.
- The boot QEMU runner recognizes the current shell prompt. Heap diagnostic
  snapshots now coherently report bytes, blocks and largest available payload.

Before-fix assertions are retained in
`build/memory-investigation/repro-shrink.log`, `repro-overflow.log`, and
`repro-pmm-audit-assert.log`. Earlier fixture compilation mistakes are retained
separately; they do not constitute defect reproductions.

## Pressure observations, separate from performance

The deterministic host heap fixture executes 10,000 mixed-size allocate/free
operations with payload checks and an integrity audit after every operation.
At the sample with most free blocks, it has 41 free blocks, 182,672 free bytes
including metadata, and a largest free payload of 18,768 bytes. After releasing
all allocations it coalesces to one 458,752-byte free block, with 112 committed
data pages and one synthetic retained table. Committed pages remain in PMM:
freeing heap objects enables reuse, not physical reclamation.

A 2 MiB payload request fails the existing 512-page expansion limit because
metadata requires another page, despite available synthetic frames. A separate
frame-budget exhaustion case consumes the existing free block first; failed
expansion restores its frame set and preserves heap state.

The actual PMM fixture exhausts its synthetic map, releases alternating frames,
and has 513 free pages with largest run one. Two-page allocation fails while
single-page allocation succeeds. Exact allocation-set equality returns after
cleanup. This demonstrates external fragmentation independently of total free
capacity. PMM bitmap used counts include address holes/reservations; they are
not payload usage or the separate managed-RAM total used by SYS_SYSINFO.

None of these observations establish allocator latency, throughput, SMP
scaling, a production failure rate, or a need for allocator redesign.

## Verification and evidence

The complete campaign is `wsl -d Ubuntu-24.04 -- make test-memory`.
Final retained directory: `build/memory-20261009T171204718445Z`.
Its `result.json` is the authoritative status/command/hash record.
All five host targets, the normal build, six VMM cases, four low-RAM boot cases
and both journaled EXT4 firmware cases passed.

| Coverage | Evidence boundary |
| --- | --- |
| Heap ASan/UBSan | Real heap; single-threaded checked lock and synthetic PMM/VMM. Resize, coalescing, counter faults, pressure, ten five-page expansion cuts and retained-table boundary. |
| PMM audit/boot ASan/UBSan | Real PMM; synthetic memmaps, host locks/readiness; accounting, reservations, fragmented/exhausted OOM, capped allocation, unlock policy and exact cleanup. |
| VMM ASan/UBSan | Actual VMM with pthread serialization; lifetime, atomic walks, OOM, mapper competition, batch map/unmap and exact reclamation. |
| PMM concurrent host | Existing real-PMM pthread stress, ceiling/transition, multipage/OOM and telemetry. This target is not sanitizer-instrumented. |
| VMM QEMU | BIOS/UEFI x 1/4/8 CPUs, 2 GiB, disposable ISO/no data disk; 100 process cycles, exact bitmap, table, stack and deferred reclamation. |
| Boot QEMU | BIOS/UEFI x 1/4 CPUs, 256 MiB, disposable ISO/no data disk; readiness/ceiling and shell recovery. No high-memory threshold claim from these low-RAM cases. |
| Journaled EXT4 QEMU | BIOS/UEFI x four CPUs; disposable production-image USB copies, paired OVMF, no NVMe. Ten warmed VFS memory cycles, Ring 3 persistence, reboot, independent Linux exact bytes and fsck. |

An additional tightened host boundary assertion compares the exact synthetic
frame and leaf sets, allowing only the expected newly retained table. Its pass
is in `build/memory-investigation/final-boundary-host.log`.

The first full campaign's EXT4 runner stopped after a successful kernel memory
audit because its case-sensitive log check expected `EXT4`; production reports
`[USB E4-B] Selected filesystem: ext4 journaled`. The corrected runner checks
that exact marker and independently requires has_journal/extent before boot.
The failed campaign remains in `build/memory-20261009T170814579752Z`; the
separate completion is in `build/memory-ext4-completion`. The earlier campaign
with a missing fixture include path remains in `build/memory-20261009T170749158679Z`.

## Journaled EXT4 scope

The opt-in `opt/fortress/memory_storage_test` hook executes after production
mount, before shell startup. Admission requires the QEMU xHCI identity and no
NVMe. It reuses existing static PMM snapshot storage. Warm-up creates one
reusable node and exercises write/truncate/close/sync before the baseline.
Ten synchronous write/read/truncate/close/sync cycles require exact payload
readback, PMM bitmap equality, coherent heap metric equality, unchanged table
and deferred counts, and unchanged diagnostic kernel mapping fingerprint.
The probe then unlinks its file. Intentional node/cache retention is included
in the baseline; there is no unmount or reclamation-policy change.

The runner separately creates a file through Ring 3 shell commands, syncs and
shuts down, verifies exact bytes via Linux debugfs and clean e2fsck, reboots,
reads/removes the file and repeats the offline checks. Both disposable images
retain journals; the shipped source image hash must remain unchanged during
testing. Kernel VFS cycles and Ring 3 persistence are separate claims.
Process lifecycle is tested separately by the VMM matrix, not attributed to
the storage hook. Fingerprint equality is diagnostic, not a mapping proof;
bitmap equality is allocation state, not a proof of ownership.

## Preserved behavior and follow-up

VMM lifetime/atomic walks/deferred teardown and scheduler context-handoff code
were not edited. No per-CPU caches, fit-policy change, heap trimming, bootloader
reclamation, or EXT4 durability change was introduced. Normal `make` rebuilds
`bin/fortress.img` as journaled EXT4; no legacy ext2 image is silently produced.
The permissions-plan SHA-256 remains
`79376ef18093ceea4cb7dfd6c6b399277f56db788d67f2ab728b26216957b323`.

The next focused investigation should quantify retained heap backing under
system-wide frame pressure and long-lived EXT4 node/cache growth. The bounded
reused-node test establishes stability after warming, not unlimited namespace
churn. Determine representative allocation sizes and failure causes before
changing the expansion cap or adding physical reclamation. Keep any latency
campaign separate, and measure journaled EXT4 directly. No Dell test is needed
for this completed automated gate; no physical or speedup claim is made.

## Retention investigation: first evidence

Run `make test-ext4-memory-churn-host` for the bounded namespace test. It
extracts partition 2 from the normal image into a new evidence directory,
requires has_journal/extent, and runs actual journaled EXT4/VFS/JBD2 under
ASan/UBSan. No image formatting, ext2 fallback, or production policy change.
Commands, logs, source/permissions hashes and status are retained in
`build/memory-churn-20261009T172313121762Z/result.json`.

Observed correctness/availability limitation:

- Starting with only the mount root cached, 1,023 create/close/unlink cycles
  exhaust all 1,024 mount-lifetime node slots. Each removed node remains held;
  measured node size is 584 bytes, or 597,432 bytes of retained node payload.
  This excludes allocator metadata and mount-wide allocations.
- The file is absent after every unlink, no orphan remains, and the on-disk
  free inode and block counters return exactly to baseline. Thus this failure
  is cached-node capacity exhaustion, not disk space exhaustion.
- Ten further creates return EFBIG, with unchanged live allocation count and
  zero disk events. The filesystem is not tainted. Both exhausted and remounted
  clean snapshots pass Linux `e2fsck -fn` and independent absent-file checks.
- Host teardown/remount restores creation and releases all fixture allocations.
  This is not a claim of a supported runtime unmount API or kernel reboot test.

The heap ASan/UBSan gate now also verifies that all 458,752 committed bytes
are free after mixed-size churn, yet 112 data frames plus one synthetic table
frame remain allocated. With a zero synthetic PMM allocation budget, existing
free heap capacity is reusable, expansion fails, and freeing does not return
frames. This is expected grow-only behavior, not a demonstrated ownership leak.
The adapter does not establish actual system-wide PMM pressure or scheduling.

First follow-up: reproduce the EXT4 namespace capacity limit in disposable
QEMU USB images, with saved logs and offline checks, then trace all VFS node
consumers and open/unlink lifetimes before discussing reclamation. A separate
bounded QEMU low-memory experiment should quantify how retained heap frames
affect other page consumers. No hardware evidence is needed yet. No latency,
performance, ext2-to-EXT4 comparison or allocator redesign claim is made.

## QEMU reproduction and lifetime review

`wsl -d Ubuntu-24.04 -- make test-memory-churn` PASS at
`build/memory-churn-qemu-20261009T172819422298Z/result.json`:
BIOS and UEFI, four configured CPUs, two boots per firmware. The first boot
uses the separate `opt/fortress/memory_storage_churn` diagnostic with existing
QEMU xHCI/no-NVMe admission; production mount must report journaled EXT4.
The synchronous BSP probe runs before the shell, not concurrently across APs.

Both first boots complete 1,023 create/close/unlink cycles, then reject with
EFBIG. Exactly 1,023 heap allocations remain; heap-accounted used bytes grow
638,352 bytes (624 per node including alignment/header/footer). BIOS committed
bytes grow 675,840 -> 1,404,928; UEFI 741,376 -> 1,396,736. These are within-boot
snapshots, not comparable performance results or proof that all growth is leak.
Ten further rejections preserve used/block/committed totals; heap integrity,
PMM audit and sync pass. Shell startup, sync and clean shutdown still work.

The second boot has no churn opt-in. Ring 3 creates and reads a new file;
Linux debugfs checks its exact bytes after shutdown. All four shutdown copies
pass `e2fsck -fn`, retain journal/extent features, and contain no churn file.
Exact QEMU argv, UART/stderr and offline logs are retained. The rebuilt normal
image is SHA-256
`4cd777f8279c872b3b8450e84afcd10379d9c0144acd90f759d78c29c3c9247a`;
its hash and the permissions-plan hash remain unchanged during the campaign.
PMM boot and VMM host sanitizer regressions also pass after adding the hook.

The extended host gate passes at
`build/memory-churn-20261009T172927661854Z/result.json`. It additionally proves
that an uncached existing `/README.txt` is present in the directory on disk,
but VFS open reports ENOENT at full cache capacity. After remount, open works.
This misleading absence is host-reproduced; the QEMU campaign specifically
checks creation exhaustion and recovery, not that existing-file symptom.

### Why immediate freeing is unsafe

- `vfs_lookup` (`src/fs/vfs.c:233`) returns raw nodes without a transient pin;
  directory traversal uses parent/child pointers after callback unlock.
- `vfs_open_ext` pins via `node->open` only after lookup and writable checks
  (`src/fs/vfs.c:659`). EXT4's opens count covers independent file handles,
  not lookup users, syscall stat, traversal or in-flight namespace callbacks.
- `vfs_unlink` reads `target->owns_nodes` after the filesystem unlink callback
  (`src/fs/vfs.c:485`). Freeing inside that callback would invalidate this
  access even without concurrency. Rename has a similar post-callback access.
- `sys_stat` uses an unpinned lookup result (`src/kernel/syscall.c:959`).
  `e4_jclose` and orphan pin scans depend on retained nodes/opens to preserve
  open-unlinked file contents until final close. Detached parents and cached
  arrays are further references that reclamation must account for.

The existing mount-lifetime contract deliberately prevents these use-after-
free hazards. No current use-after-free is claimed. Raising 1,024 or freeing
zero-open tombstones is not an evidence-backed lifetime fix: the former only
delays exhaustion and the latter breaks pointer safety. First discuss explicit
transient node references/callback ownership and reliable lookup errors before
implementing cache reclamation. A bounded retained-backing/PMM pressure probe
is an independent next investigation. No Dell tests or speedup claims needed.

## Approved bounded node reclamation

The user approved implementation after the QEMU findings. This supersedes the
earlier mount-lifetime-only limit for normal journaled EXT4 VFS operations.
The 1024-slot cache bound, allocator policy, journal ordering and storage
admission remain unchanged. No VMM or scheduler implementation was edited.

`vfs_lookup_ref` and `vfs_create_ref` return owned references paired with
`vfs_node_put`; filesystem callbacks acquire them under existing exclusion.
Path traversal holds the parent until acquiring the child. Normal open keeps
one node reference for each independent file_t until final close, including
dup/shared-descriptor cases; failure paths unwind it. Journaled namespace
callbacks own tree changes without VFS dereferencing a freed target afterward.
Stat/chdir use owned lookup results and preserve filesystem errors.

EXT4 tracks transient references separately from file opens. Unpinned deleted
nodes can be freed; open-unlinked inode cleanup still follows the existing
durable orphan machinery. Cached children, including detached open children,
retain parent addresses. At capacity, only an unpinned, unopened, non-legacy
leaf may be evicted. Root is permanent; removing a child can collect a removed
parent iteratively. Fully pinned capacity rejects with EFBIG before writes.
Lookup allocation/I/O errors remain ENOMEM/EIO, not apparent ENOENT; O_CREAT
only attempts creation after actual ENOENT.

Compatibility is explicit: old raw lookup/create callbacks mark returned
journaled nodes as mount-lifetime stable. They must not be used for a churn
workload that expects reclamation. E4-A/ext2 keep their existing node policy.
Owned references protect addresses, not an atomic snapshot of every node
field, and do not add general legacy VFS tree synchronization or unmount.

Verification:

- Host ASan/UBSan full churn/pin gate:
  `build/memory-churn-20261009T174824528245Z/result.json`. 1152 unpinned cycles
  return exactly to one cached root and the live-allocation/disk-counter
  baseline. 1023 deliberately pinned tombstones force EFBIG for creation and
  uncached existing-file open, with zero disk events. Releasing one pin restores
  lookup; idle-leaf eviction restores creation. All pins release without leaks.
- Pthread lookup/unlink ordering, detached-parent lifetime, independent/dup
  open-unlink reads, inode reuse and legacy-pointer compatibility pass. Focused
  rename, parent transfer, OOM/I/O/type/reference-overflow cleanup passes in
  `build/memory-churn-20261009T175518319658Z/result.json` on final VFS code.
- BIOS/UEFI four-CPU QEMU:
  `build/memory-churn-qemu-20261009T174633413688Z/result.json`. Each first boot
  completes 1152 cycles with zero retained blocks/used-byte growth, unchanged
  741376 committed bytes and exact PMM bitmap equality. The formerly uncached
  existing file reads successfully. Shell sync/shutdown, reboot creation,
  exact Linux bytes and all four offline fsck checks pass. BSP synchronous
  churn is not a cross-core namespace-race acceptance claim.
- Existing mounted-journal lifecycle/admission/recovery/directory-cache smoke
  and eight pthread write/freeze races pass, with Linux audits:
  `build/memory-node-mount-20261009T175535988990Z/result.json`. Production
  geometry is 4096-byte blocks/512-byte sectors; this is not the 12-geometry
  full cut/restart matrix. Non-journaled E4-A write host regressions pass six
  block/sector geometries with fsck and exact 16MiB byte audits at
  `build/ext4-phase4/host-pxz48dyw`. Pipe/SIGPIPE host tests pass.
- The memory campaign passes all five host targets, VMM BIOS/UEFI 1/4/8 CPUs,
  256MiB boot tests 1/4 CPUs and warmed journaled EXT4 persistence:
  `build/memory-20261009T175106384221Z/result.json`. The final additional rename
  non-directory pin-release check was added afterward and tested separately.

Verification gaps and retained failures:

- The first reclamation host run completed its C assertions and Linux checks,
  but correctly failed the source-image hash gate because a concurrent normal
  build replaced the image. Retained at `build/memory-churn-20261009T174518227814Z`;
  the subsequent immutable-source run above passes. `test-memory-nodes` builds
  before running its checks sequentially.
- `make test-ext2` stops at `tests/ext2_host.c:193`, an allocation-fault assertion
  expecting write failure. The same assertion fails using HEAD's pre-change VFS
  source with the unchanged ext2 implementation/current public headers;
  reproduction sources and log are in `build/ext2-baseline*` and
  `build/memory-baseline-vfs.c`. This is a separate existing verification gap,
  not an ext2 regression attributed to node reclamation. The full legacy ext2
  suite is not claimed passing; no ext2 implementation changes were made.
- The stock mount-smoke target requires a completed fixture directory and had
  none configured. The production-copy smoke runner supplies explicit fixtures
  instead; it does not claim the missing historical geometry matrix was run.

Next memory investigation remains bounded actual PMM pressure versus retained
heap backing. No allocator redesign, Dell test or performance improvement is
claimed here. All tests use explicit journaled EXT4 production copies; only
the named legacy ext2 regression runner formats explicit ext2 fixtures.

## Legacy ext2 test gap resolved

`wsl -d Ubuntu-24.04 -- make test-ext2` now passes all eight ASan/UBSan cases:
1024/2048/4096-byte blocks with 128-byte inodes, plus 4096-byte blocks with
256-byte inodes, each using 512/4096-byte sector adapters. Output is retained
at `build/ext2-test-gap.log`. This supersedes the earlier failing-suite note.

Two assertions expected heap allocation during a cached direct-block write.
RW mount now supplies the bitmap buffer, so neither injected allocation budget
was reaching an allocation. The first case now disables allocations entirely
and requires a successful write with zero allocation attempts/failures, exact
readback, one consumed block, clean cache and equal pending/durable bytes.
The second targets the actual truncate prevalidation workspace: exactly one
failed allocation, ENOMEM, unchanged live allocations, writes/flushes, file
size/free blocks and an untainted mount. Existing populated-file truncate OOM
and I/O failure coverage remains intact.

Only host test assertions/instrumentation changed; ext2/VFS kernel behavior,
normal journaled EXT4 image and permissions-plan edits were not modified.
No QEMU or hardware claim follows from this host-only test repair.

## Real PMM pressure versus retained heap backing (2026-10-09)

Added `make test-memory-pressure`: build first, then disposable ISO-only QEMU
TCG, one CPU, BIOS/UEFI at 256/512 MiB and an unmodified BIOS 256 MiB control.
The diagnostic runs after normal heap self-tests and before AP/thread startup.
It takes two baselines: existing free heap capacity, then a touched eight-by-
512 KiB allocation burst after every allocation is freed.

At each baseline it holds every free PMM frame using an intrusive ownership
chain in those frames (no auxiliary heap allocation), checks single-page and
contiguous PMM OOM, fills existing heap capacity successfully, and requires
another heap allocation to fail because expansion cannot acquire a frame.
It returns every held frame and checks exact PMM bitmap equality, all heap
snapshot fields, table counts, rejected-free accounting and independent audits.
Monotonic above-address allocation avoids repeated first-fit rescans; this
is deliberately not a latency or fragmentation benchmark.

Evidence: `build/memory-pressure-20261009T181543770946Z/result.json`, argv,
UART and stderr per case. The pressure cases each retain 1,032 additional
heap data pages and two table pages after the burst: 1,034 fewer frames
(about 4.04 MiB) available to direct page consumers. Free committed heap
capacity rises from 36,864 to 4,263,936 bytes, coalesced into one free block.
Both exhaustion rounds restore their respective exact baseline and subsequent
normal startup reaches the Ring 3 shell. Four expected allocation failures
are counted per round. This confirms a capacity limitation of grow-only heap
backing, not a cleanup leak: free heap capacity remains usable under PMM OOM.

The control boot has no pressure marker. The campaign verifies unchanged
source image, ELF and permissions-plan hashes during testing; no data disks
are attached. PMM boot, heap memory and VMM host regression targets also pass.
Allocator policy, VMM ownership and scheduler handoff code are unchanged.
The normal build remains journaled EXT4; this ISO-only test makes no ext2 or
EXT4 workload/performance claim and requires no Dell testing.

Unsuccessful attempts retained: `memory-pressure-20261009T180921545027Z`
ran the probe before the normal heap tests, invalidating their fixed 16 KiB
expansion assumption; the hook was moved after those tests. Its interrupted
QEMU was explicitly terminated. `memory-pressure-20261009T181204818171Z`
passed BIOS 128 MiB but timed out in UEFI 128 MiB before any kernel diagnostic;
the runner reaped QEMU and retained a FAIL manifest. This is an uninvestigated
low-RAM boot boundary, not evidence of memory-probe failure or a general
128 MiB UEFI support claim. The final supported test matrix uses 256/512 MiB.

Next focused investigation: measure live/free/committed heap, free PMM frames
and process/table cleanup across a representative process/allocation burst,
then test recovery under bounded pressure. Separate meaningful page-consumer
demand from synthetic exhaustion before proposing tail trimming or changing
retention policy. No allocator redesign follows from this test alone.

## Representative process bursts and bounded PMM recovery (2026-10-09)

Added `make test-memory-burst`: builds first, then runs disposable ISO-only
QEMU TCG, one CPU, BIOS/UEFI at 256/512 MiB and an unmodified BIOS 256 MiB control.
Evidence: `build/memory-burst-20261009T183501817513Z/result.json`, argv, UART
and stderr per case (5/5 PASS).

The diagnostic runs after AP bringing-up and scheduler startup, before input/shell
initiation. It executes four sequential stages:

1. **Warmed Baseline**: Four warmup process spawn/exit cycles establish reused
   stack-slot mappings and TCB sizes. After draining via `sched_reap_dead()` and
   `vmm_drain_deferred_destructions()`, baseline snapshots record live heap
   (29,920 bytes used, 45,056 bytes committed, 15,136 bytes free, 15,104 largest
   payload), table frames (279 for 256 MiB, 407 for 512 MiB), and exact PMM bitmap.
2. **Representative Process Burst**: 32 real process spawn/exit cycles across
   CPUs using `embedded_init_elf` mode 9, interleaved with kernel heap allocation
   churn (varying sizes 256–704 bytes). After full drainage through supported
   mechanisms (`process_wait`, `sched_reap_dead`, `vmm_drain_deferred_destructions`),
   post-burst snapshots show:
   - Live heap used bytes returned exactly to 29,920.
   - Retained heap delta: 0 (committed total remained 45,056 bytes).
   - Page-table delta: 0 (table frames remained 279 / 407).
   - Free PMM frames returned exactly to baseline (60,421 BIOS / 58,825 UEFI at 256 MiB).
   - Zero deferred destructions and zero stack slot leaks.
3. **Bounded PMM Pressure & Real Page Consumer Demand**: Free PMM frames are held
   using an intrusive ownership chain (no heap memory) down to controlled bounds:
   - *Headroom 64 frames*: Real page consumer test spawns a user process (~10–12
     physical frames needed). Spawn, Ring 3 execution, exit code 42 and reaping
     succeed cleanly under bounded pressure.
   - *Existing heap reuse*: A 4,096-byte `kmalloc` from retained free capacity
     succeeds without allocating any PMM frames.
   - *Headroom 2 frames*: Tightly constrained below process requirement. Real process
     spawn fails cleanly returning `NULL` / ENOMEM with zero kernel panic, crash or
     leak. Heap expansion beyond capacity fails cleanly returning `NULL`.
   - *Release*: All held frames (60,419 in BIOS 256M) are verified for signature
     integrity and returned to PMM.
4. **Post-Pressure Recovery**: Reaping dead tasks and draining deferred destructions
   restores exact heap metrics, exact table-frame counts, zero deferred destructions,
   and exact PMM bitmap equality (`memcmp(before, after) == 0`). Full integrity
   audits (`heap_verify_integrity() && pmm_audit()`) pass.

Host test coverage in `tests/vmm_space_host.c` (`test_burst_memory_accounting`)
verifies address space lifecycle bursts, deferred destruction queueing, and clean
OOM rollback under ASan/UBSan.

### Synthesis and Next Focused Decision

1. **Grow-only capacity is bounded for representative workloads**: Unlike synthetic
   unbounded `kmalloc` bursts, representative process workloads do not steadily
   expand kernel heap backing. Reused TCB allocations and kernel stack slots fit
   entirely within warmed baseline capacity (delta = 0).
2. **Reclamation mechanisms are complete**: Supported mechanisms (`process_wait`,
   `sched_reap_dead`, `vmm_drain_deferred_destructions`) return 100% of physical data
   frames, user page tables, and stack frames.
3. **Real page consumers degrade gracefully**: Under bounded PMM pressure, process
   spawning succeeds when headroom exists, and cleanly fails with `NULL`/ENOMEM when
   exhausted, without corrupting state or leaking frames.
4. **No allocator redesign or heap trimming is indicated**: The capacity lost in
   the earlier probe (4.04 MiB) was purely a direct consequence of an intentional
   4 MiB heap burst. Because normal process workloads do not cause heap expansion
   churn, physical heap trimming would add complexity and synchronization overhead
   without addressing a real workload deficit.

## Concurrent Process Peaks and Fragmented PMM Headroom

Date: 2026-10-09.
Target: `make test-memory-cohort`.
Evidence: `build/memory-cohort-20261009T190329723217Z/result.json`.
Logs: `bios-256M-cohort.log`, `uefi-256M-cohort.log`, `bios-512M-cohort.log`, `uefi-512M-cohort.log`, `bios-256M-control.log`.
Status: **5/5 PASS** (BIOS/UEFI across 256 MiB and 512 MiB, plus BIOS control).

Tracked artifact integrity verified before and after test execution:
- `bin/fortress.img`: `7b0aa628ef67196a66b2d042b261dbc003e50dacc71922e681f0940a693d5f2d`
- `bin/fortress.elf`: `d99fea6d266550804cc98632e79c1f18c91997ca352dea9b97d6ade1bc31156e`
- `docs/plans/PERMISSIONS_PLAN.md`: `79376ef18093ceea4cb7dfd6c6b399277f56db788d67f2ab728b26216957b323`

### Methodology and Execution

The test exercises multi-task concurrency and physical memory fragmentation without modifying allocator policy:

1. **Warmed Baseline Snapshot**:
   - Live heap used: 29,920 bytes; heap committed: 45,056 bytes; free payload: 15,104 bytes.
   - PMM free pages: 60,414 (BIOS 256M), 58,818 (UEFI 256M), 125,820 (BIOS 512M), 124,224 (UEFI 512M).
   - Page-table frames: 279 (256 MiB), 407 (512 MiB).
   - Zero deferred destructions. PMM bitmap snapshot taken.

2. **Concurrent Cohorts across Sizes 2, 4, 8 with 2 Passes Each**:
   - `sched_disable_preemption()` during cohort creation ensures all $N$ processes are staged simultaneously before any process executes.
   - Active stack slots bitmask verifies simultaneous coexistence: `count_bits64(active_mask ^ initial_mask) == N`.
   - Simultaneous peak heap live usage:
     - Cohort 2: peak 34,656 bytes (+4,736 bytes for 2 TCBs + stacks). Committed: 45,056 (`heap_delta = 0`).
     - Cohort 4: peak 39,392 bytes (+9,472 bytes for 4 TCBs + stacks). Committed: 45,056 (`heap_delta = 0`).
     - Cohort 8: peak 48,864 bytes (+18,944 bytes for 8 TCBs + stacks). Committed expanded by one 4 KiB chunk to 49,152 bytes (`heap_delta = 4096`).
   - Mixed-order reap: Processes are waited on in permuted order (size 2: {1, 0}; size 4: {3, 0, 2, 1}; size 8: {7, 2, 5, 0, 6, 1, 4, 3}). Each process terminates with exit code 42.
   - Post-reap quiescence restores live heap used bytes to exactly 29,920 and table frames to 279 / 407 with zero stack slot leaks and zero deferred destructions.
   - Pass 1 repeat: For each cohort size, repeating the exact cohort verifies `repeat_stable=PASS`. Heap committed remained identical (45,056 for sizes 2 and 4; 49,152 for size 8), proving that peak heap growth is a one-time capacity expansion, not continuing retention or leak.

3. **Controlled Scattered Free Frames & Fragmented PMM Headroom**:
   - Intrusive linked list exhausts all physical memory down to 128 frames.
   - The remaining 128 frames are allocated into an array `frag_pages[0..127]`, leaving zero free pages in the machine.
   - Even indices `frag_pages[0, 2, 4, ...]` are freed (64 frames), leaving every free frame flanked on both sides by an allocated frame (`frag_pages[1, 3, 5, ...]` and intrusive list frames).
   - Contiguous allocations fail: `pmm_alloc_pages(2) == 0` and `pmm_alloc_pages(4) == 0` (`contig2_fail: PASS`, `contig4_fail: PASS`). Total free memory (256 KiB) is completely fragmented with maximum run length 1.
   - Single-page allocation succeeds: `pmm_alloc_page()` succeeds (`single_page: PASS`).
   - Real Ring 3 user process spawn succeeds: `process_spawn_on_cpu` succeeds (`proc_spawn: PASS`). Virtual memory maps physically disjoint frames into contiguous virtual user space. Process executes and exits with code 42.
   - Tight exhaustion test: Free memory constrained to $\le 2$ frames. Process spawn cleanly fails (`NULL`/ENOMEM) with zero crash or leak (`oom_spawn_fail: PASS`). Heap expansion beyond capacity cleanly fails (`NULL`).
   - Restoration: All held frames released. Contiguous allocation restored: `pmm_alloc_pages(4)` succeeds (`contig_restore: PASS`). Post-pressure process spawn succeeds and executes cleanly (`post_spawn: PASS`).

4. **Post-Pressure Recovery**:
   - Quiescence drain verifies:
     - Exact post-cohort heap used (29,920 bytes), blocks, total (49,152 bytes), and free bytes restored.
     - Exact table frames (279 / 407) restored.
     - Zero deferred destructions.
     - Exact PMM free pages restored.
     - Full heap integrity audit and PMM bitmap audit pass.
     - Exact bitmap comparison matches post-cohort baseline.

### Evidence Boundary Qualifications

1. **Overlapping Process Ownership vs. Simultaneous Multicore Execution**:
   The cohort test verifies simultaneous co-existence in memory — all processes hold
   dedicated stack slots in `sched_get_active_stack_slots_mask()`, allocated TCBs,
   and independent user PML4 address spaces concurrently. However, on single-core
   QEMU runs (`-smp 1`), execution is scheduled sequentially via timeslicing and
   reaped in mixed order, not executing simultaneously across physical hardware cores.
2. **Alternating Allocation Indices vs. Physical Frame Isolation**:
   In the fragmented PMM test, allocating into an array and freeing alternating even
   indices establishes non-adjacent array indices, but does not by itself prove physical
   address non-contiguity without comparing physical addresses. However, the subsequent
   rejection of multi-page requests (`pmm_alloc_pages(2) == 0` and `pmm_alloc_pages(4) == 0`)
   definitively demonstrates the complete absence of any 2-frame contiguous run in the
   allocator's free pool, proving that every free frame was physically isolated.

## Process-Launch Allocation-Failure Rollback

Date: 2026-10-09.
Target: `make test-memory-rollback`.
Evidence: `build/memory-rollback-20261009T194049474942Z/result.json`.
Logs: `bios-256M-rollback.log`, `uefi-256M-rollback.log`, `bios-512M-rollback.log`, `uefi-512M-rollback.log`, `bios-256M-control.log`.
Status: **5/5 PASS** (BIOS/UEFI across 256 MiB and 512 MiB, plus BIOS control).

Tracked artifact integrity verified before and after test execution:
- `bin/fortress.img`: `33b7079609c251c656f0141bf07080d179292903bc70ed2b185070b8b836b6d6`
- `bin/fortress.elf`: `bec1cedbdd3833846ad299e79ecf57c94435f485b7cbeab2193641c4ba92ed41`
- `docs/plans/PERMISSIONS_PLAN.md`: `79376ef18093ceea4cb7dfd6c6b399277f56db788d67f2ab728b26216957b323`

### Traced Callers and Construction Stages

Real callers of `process_spawn_on_cpu` and `process_spawn_with_actions` include kernel
initialization self-tests in `main.c`, shell program execution, and the `sys_spawn`
system call in `syscall.c`. Every fallible construction stage was audited and
instrumented across `process_spawn_internal`, `load_elf_into_space`, and `kstack_alloc`:

1. `SPAWN_FAULT_VMM_USER_PML4`: Early OOM at `vmm_create_user_pml4()`. Returns `ELF_ERR_NOMEM`.
2. `SPAWN_FAULT_ELF_SEGMENT_PMM_PAGE0`: PMM frame allocation fails on the first ELF segment page (early OOM).
3. `SPAWN_FAULT_ELF_SEGMENT_PMM_PAGE1`: Partial segment construction: page 0 is successfully allocated and mapped into the user PML4; page 1 PMM allocation fails. Verifies `vmm_destroy_pml4(pml4, true)` walks and cleanly reclaims previously mapped page 0 and intermediate user tables.
4. `SPAWN_FAULT_ELF_SEGMENT_MAP_PAGE0`: Page 0 frame allocated, VMM mapping fails. Frame freed before destroying PML4.
5. `SPAWN_FAULT_ELF_SEGMENT_MAP_PAGE1`: Partial segment construction: page 0 mapped; page 1 frame allocated; mapping of page 1 fails. Explicitly frees unmapped page 1 frame, then destroys PML4 reclaiming page 0 and tables.
6. `SPAWN_FAULT_SIGRESTORER_PMM`: Frame allocation fails for signal restorer stub page after segments are mapped. PML4 destroyed.
7. `SPAWN_FAULT_SIGRESTORER_MAP`: `vmm_map_page()` fails for signal restorer page. Unmapped frame freed, PML4 destroyed.
8. `SPAWN_FAULT_USER_STACK_PMM`: Frame allocation fails for user stack page after segments and restorer are mapped. PML4 destroyed.
9. `SPAWN_FAULT_USER_STACK_MAP`: `vmm_map_page()` fails for user stack page. Unmapped frame freed, PML4 destroyed.
10. `SPAWN_FAULT_KSTACK_PMM_PARTIAL`: Partial kernel stack allocation: allocates 2 of 4 stack frames mid-loop (`allocated == 2`), then PMM fails. Verifies `kstack_alloc` rolls back both allocated frames and clears slot bitmap.
11. `SPAWN_FAULT_KSTACK_MAP_BATCH`: Kernel stack batch mapping failure: all 4 stack frames allocated, `vmm_map_pages` fails. Verifies `kstack_alloc` frees all 4 frames and clears slot bitmap.
12. `SPAWN_FAULT_KSTACK_SLOT_EXHAUST`: Kernel stack slot allocation failure (bitmap exhausted).
13. `SPAWN_FAULT_TCB_KMALLOC`: `kmalloc(sizeof(tcb_t))` fails after kstack is allocated. Kernel stack freed (`kstack_free`), user PML4 destroyed.
14. `SPAWN_FAULT_FD_INIT`: `fd_init_std()` fails after TCB and kstack allocated. `fail_actions` invokes `fd_close_all()`, frees TCB (`kfree`), frees kstack, and destroys user PML4.
15. `SPAWN_FAULT_SPAWN_ACTIONS_PARTIAL`: Spawn action failure with partial progress: action 0 opens `/etc/motd` to fd 3; action 1 fails with ENOENT. `fail_actions` unwinds via `fd_close_all()`, cleanly closing fd 3 and stdio, releasing all VFS node references, TCB, kstack, and user PML4.
16. `SPAWN_FAULT_SCHED_REF`: `vmm_space_add_sched_ref()` fails right before runqueue insertion / publication. `fail_actions` rolls back all held descriptors, TCB, kstack, and user PML4.

In all 16 cuts, `process_spawn_on_cpu` catches `!p` and calls `process_record_abort(pid)`,
clearing the reserved PID record from `g_process_table` and freeing any associated group resources.

### Coverage Audit and Evidence Boundaries

1. **Exact Hit Verification (`spawn_get_fault_hits() == 1`)**:
   Every injected fault is tracked via `g_spawn_fault_hits` on the production execution path.
   The test asserts `spawn_get_fault_hits() == 1` at each cut, proving the failure point was
   actively encountered on the production path and eliminating false passes.
2. **Synthetic Stage Rejection vs. Internal Partial-Allocation Rollback**:
   Fault cuts distinguish simple upfront rejection from true partial construction unwinding:
   - Cuts 3 and 5 verify that when multi-page ELF segments fail midway (page 1), earlier mapped pages (page 0) and intermediate page tables are completely reclaimed by `vmm_destroy_pml4`.
   - Cut 10 verifies that when `kstack_alloc` fails mid-loop after allocating 2 frames, the partial frames are freed and the slot bit is cleared.
   - Cut 11 verifies batch map failure rollback in `kstack_alloc`.
   - Cut 15 exercises `process_spawn_with_actions` where action 0 succeeds (opening `/etc/motd` to fd 3) and action 1 fails (ENOENT). Unwinding via `fd_close_all` cleanly releases fd 3 and stdio references, leaving zero leaked VFS node references or heap allocations.
3. **Host vs. QEMU Verification Boundaries**:
   `tests/vmm_space_host.c` does NOT compile or execute `src/kernel/thread.c` or `src/kernel/elf.c`.
   It compiles only `vmm.c` with stub adapters in `tests/host/thread.h`. Cuts 1–8 in the host test
   *model* VMM page mapping and unmapping sequences under ASan/UBSan; they are not exhaustive
   production launch coverage. Full production process creation and rollback is verified in the
   disposable QEMU integration suite (`make test-memory-rollback`).
4. **Aborted PID and Publication Verification**:
   The aborted PID is captured via `spawn_get_last_aborted_pid()` and verified:
   `!process_is_alive(aborted_pid)` and `!process_wait(aborted_pid, NULL)`. No partially
   constructed task is ever inserted into the runnable queue or published to the scheduler.
5. **Per-Cut Accounting and Integrity**:
   At each individual cut, the test verifies:
   - `sched_get_active_stack_slots_mask() == initial_stack_mask` (zero leaked stack slots).
   - `vmm_get_deferred_count() == 0` (zero deferred destructions pending).
   - `cur_heap.used_bytes == baseline_heap.used_bytes` and blocks equal (zero heap leaks).
   - `cur_tables == baseline_tables` and `cur_pmm.free_pages == baseline_pmm.free_pages`.
   - `heap_verify_integrity() && pmm_audit()` pass.
6. **Subsequent Normal Process Execution and Quiescent Recovery**:
   Following all 16 cuts, a normal Ring 3 process is spawned, executes to completion, returns exit code 42 via `process_wait()`, and is reaped cleanly. Full quiescence drain verifies exact PMM bitmap equality (`memcmp(before, after) == 0`). Control case without the diagnostic hook confirms that test instrumentation is strictly opt-in and does not run by default.

## Read-Only Memory Subsystem Observability (`sysinfo -m`)

Date: 2026-10-09.
Target: `make test-memory-observability`.
Evidence: `build/memory-observability-20261009T200055706676Z/result.json`.
Logs: `bios-256M-observability.log`, `uefi-256M-observability.log`, `bios-512M-observability.log`, `uefi-512M-observability.log`, `bios-256M-control.log`.
Status: **5/5 PASS** (BIOS/UEFI across 256 MiB and 512 MiB, plus BIOS control).

Tracked artifact integrity verified before and after test execution:
- `bin/fortress.img`: `ce49881b772e103a049719cb08d346d19b1103e3d40380096cce97e1cdc0d54f`
- `bin/fortress.elf`: `50e89aa454ff18491662688d32edf6622cc19654508aec9748635ede7d2db775`
- `docs/plans/PERMISSIONS_PLAN.md`: `79376ef18093ceea4cb7dfd6c6b399277f56db788d67f2ab728b26216957b323`

### ABI Architecture & Interface Decision

1. **Non-Breaking Syscall Separation (`SYS_MEMINFO = 56`)**:
   Existing callers of `SYS_SYSINFO` (syscall 37, 72-byte `sysinfo_t`) include `/bin/top`, `/bin/ps`, `/bin/dns`, `/bin/trace_probe`, `/bin/diskbench`, and `/bin/smpbench`. Extending `sysinfo_t` in place would risk buffer overruns in existing binaries. Instead, a dedicated introspection call `SYS_MEMINFO` (number 56) and structure `sysinfo_mem_t` (96 bytes) were introduced:
   - Caller supplies `(sysinfo_mem_t *buf, uint64_t size)`.
   - Length check rejects `size < sizeof(sysinfo_mem_t)` with `-SYSCALL_EINVAL`.
   - Range validation rejects invalid/unmapped memory with `-SYSCALL_EFAULT`.
   - ABI layout is strictly checked by static assertions (`_Static_assert(sizeof(sysinfo_mem_t) == 96)` and explicit field offsets).
2. **User CLI Integration (`/bin/sysinfo -m`)**:
   - Running `/bin/sysinfo` with no arguments produces the exact 5-section system overview expected by existing automated regression harnesses (e.g. `test_s9_sysinfo.py`).
   - Running `/bin/sysinfo -m` (or `--memory`) queries `SYS_MEMINFO` and emits a structured memory subsystem report.
   - Running `/bin/sysinfo -h` (or `--help`) outputs command syntax. Unknown flags return error code 1.
   - User stack budget (512 bytes) is respected by allocating `sysinfo_mem_t` in static BSS storage.

### Verified Metric Semantics & Snapshot Consistency

1. **Physical Memory (PMM)**:
   - `pmm_total_frames`: Managed physical 4 KiB frames (excluding non-RAM firmware reservations).
   - `pmm_used_frames`: Currently allocated physical frames.
   - `pmm_free_frames`: Currently available physical frames (`pmm_used_frames + pmm_free_frames == pmm_total_frames`).
   - `pmm_allocatable_frames`: Allocatable free frames below the 1 GiB boot ceiling.
2. **Kernel Dynamic Heap**:
   - `heap_used_bytes`: Live allocated bytes held by kernel callers, including 32-byte boundary tags (16B header + 16B footer).
   - `heap_free_bytes`: Reusable free bytes residing in the committed virtual heap capacity. These bytes are already backed by physical frames allocated to the heap, and are immediately available for `kmalloc` without demanding new PMM frames.
   - `heap_committed_bytes`: Total committed virtual backing bytes. Invariant holds: `heap_used_bytes + heap_free_bytes == heap_committed_bytes`.
   - `heap_largest_payload`: Largest single contiguous free payload allocatable (excludes 32B boundary tag overhead).
   - `heap_free_blocks`: Total count of coalesced free blocks currently linked in the heap's doubly linked free list.
3. **Virtual Memory Management (VMM)**:
   - `vmm_table_frames`: Active physical frames dedicated exclusively to 4-level paging structures (PML4, PDPT, PD, PT). Scope includes the master kernel PML4 and higher-half tables mapped during boot and runtime heap expansions, plus all active/retiring user process PML4 roots and intermediate user page tables. Data frames are not included.
   - `vmm_deferred_spaces`: Count of `vmm_space_t` structures currently queued on `g_vmm_deferred_list` awaiting asynchronous destruction. Explicitly labeled as **queued address spaces** (not frames or reclaimable bytes).
4. **Snapshot Consistency Contract**:
   - Each subsystem provides an internally coherent snapshot under its own lock: PMM stats under Rank-4 `pmm_lock`, Heap stats under Rank-2 `heap_lock`, and deferred count under Rank-3 `g_vmm_lock`.
   - The overall memory snapshot is **not** globally atomic. Subsystem locks are never nested across ranks solely to synthesize a cross-subsystem point-in-time snapshot. The output visibly discloses this non-nesting consistency model.
   - No speculative additive categories for kernel stacks, user pages, or DMA are added; existing counters are not summed into a misleading "total owned" figure.

### Verification Results

1. **Host Unit & Bounds Tests (`make test-s9-sysinfo-host`)**:
   `tests/sysinfo_host.c` verifies `sysinfo_mem_t` ABI layout, size bounds rejection (`size < sizeof(sysinfo_mem_t)` -> `-EINVAL`), invalid pointer rejection (`-EFAULT`), exact field mapping, and invariant preservation.
2. **QEMU Acceptance Suite (`make test-memory-observability`)**:
   - 5/5 cases passed across BIOS and UEFI at 256 MiB and 512 MiB plus an unmodified BIOS 256 MiB control.
   - Validated standard `/bin/sysinfo` backward compatibility and `/bin/sysinfo -m` structured memory output.
   - Observed expected scaling:
     - 256 MiB: ~65,503 total managed frames; 512 MiB: ~131,039 total managed frames.
     - Table frames: 296 frames (256 MiB) vs 424 frames (512 MiB).
     - Deferred queue: 0 queued address spaces at quiescence across all boots.
   - Process lifecycle: Running `/bin/ps` and re-checking `/bin/sysinfo -m` confirmed consistent, non-corrupted memory metrics.
   - Shell recovery: Shell prompt remained fully responsive post-inspection.




## Consumer Memory Attribution: Accounting Contracts, Limits, and Headroom Semantics

Date: 2026-10-09.
Status: Audit completed; claims qualified against code; diagnostic headroom semantics defined.

### 1. Code Audit and Evidence Boundaries

A review of the memory subsystem code identifies key boundaries that qualify consumer attribution claims:

1. **PMM Allocation vs. Consumer Attribution & Mapping Aliasing**:
   - `pmm_alloc_page()` / `pmm_alloc_pages()` manage a single bit per 4 KiB frame in `bitmap`. Setting a bit prevents duplicate allocation from the free pool, but PMM stores no owner metadata, consumer tags, or subsystem references.
   - The Higher-Half Direct Map (HHDM) in `vmm.c` maps all physical RAM linearly into kernel virtual address space (`HHDM_BASE + phys_addr`).
   - Consequently, physical frames mapped into user page tables or the kernel heap are simultaneously mapped by the HHDM. Virtual mappings must be distinguished from operational ownership: an address mapping does not prove exclusive physical ownership, and PMM cannot prove complete consumer attribution.

2. **Kernel Dynamic Stack Accounting Limits**:
   - In `src/kernel/thread.c` (`kstack_alloc`), the slot bit in `g_stack_slots_bitmap` is set under `g_kstack_lock` *before* physical frames are allocated in the loop. If frame allocation fails or during the allocation loop, the bit is set while fewer than 4 frames exist.
   - In `kstack_free`, physical frames are unmapped and returned to PMM via `pmm_free_page` *before* the slot bit is cleared in `g_stack_slots_bitmap`.
   - Therefore, `popcount64(g_stack_slots_bitmap) * 4` is a coarse estimate, not an exact snapshot invariant: asynchronous observation can overcount during both stack construction and teardown windows.

3. **Reserved Bitmap vs. Physical Address Space Holes**:
   - In `src/mm/pmm.c` (`pmm_init`), `total_pages` spans from physical address 0 to `highest_addr`.
   - The bitmap is initialized to all 1s (reserved/used), and only `LIMINE_MEMMAP_USABLE` entries are cleared to 0.
   - Any physical address range not reported as usable RAM—including hardware MMIO apertures, PCI address space holes, firmware ROMs, and DRAM bank gaps—remains marked 1 and is copied into `reserved_bitmap`.
   - These entries represent non-RAM physical address holes rather than consumed physical RAM. Counting set bits in `reserved_bitmap` spans these holes and cannot be directly subtracted from managed RAM (`g_managed_ram_bytes`) or used to deduce dynamic kernel memory.

4. **Driver-Specific DMA Sizing and Quarantine Behavior**:
   - DMA allocations depend strictly on controller detection, link state, and hardware capability registers:
     * `xhci.c`: Scratchpad allocation depends on the controller's `HCS_PARAMS2` register (`sp_count`). In addition, USB transfer endpoint rings (e.g. for BOT mass storage) are allocated dynamically during device configuration.
     * `nvme.c`: Allocates admin/IO queues and bounce buffers (5 frames) only if an NVMe controller is detected and probed; on machines without NVMe, 0 frames are allocated.
     * `e1000.c`: Allocates descriptor rings and packet buffers (146 frames) only if cable/link bring-up proceeds to ring initialization; an absent NIC or cold cable waiting allocates 0 DMA frames.
   - DMA quarantine is a driver-specific failure/shutdown policy (abandoning reclamation to prevent bus-mastering corruption); it cannot be modeled as a fixed static frame count across all boots.

5. **Snapshot Skew, In-Flight Allocations, and Remainder Underflow**:
   - Subsystem counters are sampled sequentially under separate ranked locks (`pmm_lock` Rank 4, `g_vmm_lock` Rank 3, `heap_lock` Rank 2, `g_kstack_lock` Rank 1).
   - An "unattributed remainder" computed by subtracting dynamic subsystem counts from PMM used frames can fluctuate, become negative, or reflect transient in-flight allocations and mismatched accounting domains, not a memory leak.
   - Unsigned subtraction `pmm_used - (reserved + dynamic)` risks arithmetic underflow. Remainder calculations must account for snapshot skew.

6. **ABI Buffer Negotiation Status**:
   - `SYS_MEMINFO = 56` currently validates `size >= sizeof(sysinfo_mem_t)` (96 bytes) and copies exactly 96 bytes.
   - Larger-buffer length negotiation (`min(size, sizeof(...))`) is a proposed contract pattern for future ABI evolution, not something currently supported in the kernel dispatch.

---

### 2. Qualified Accounting Contracts

| Category | Allocator | Observed Resource | Boundaries & Concurrency |
| :--- | :--- | :--- | :--- |
| **Kernel Heap Committed** | `src/mm/heap.c` | `heap_committed_bytes / 4096` physical frames | Backed by `heap_lock` (Rank 2). Byte usage (`heap_used_bytes`) includes 32B boundary tags and represents sub-allocator payload, not physical frame allocations. |
| **VMM Page Tables** | `src/mm/vmm.c` | `vmm_table_frames` (PML4, PDPT, PD, PT frames) | Tracked by atomic counter `g_vmm_allocated_table_frames`. Covers kernel master root + runtime expansions + user process page tables. Strictly excludes mapped leaf data frames. |
| **Kernel Dynamic Stacks** | `src/kernel/thread.c` | Estimated as `popcount64(g_stack_slots_bitmap) * 4` | Protected by `g_kstack_lock` (Rank 1). Up to 4 frames per active slot. Overcounts transiently during stack allocation and teardown. BSP boot stack is in kernel `.bss` (boot-reserved). |
| **User Data Leaf Frames** | `src/kernel/elf.c`, `thread.c` | Singly-mapped leaf frames mapped with `PTE_USER` | Lower-half user space only ($0 \le \text{VA} < \text{0x0000800000000000}$). Singly-owned per address space (no COW/shared memory). Freed during teardown (`free_user_frames = true`). |
| **Hardware DMA** | Driver initialization | Hardware-bound DMA buffers | Quarantined on error/shutdown. Sizing depends on hardware presence, controller capability registers, and attached devices. |
| **Boot / Firmware Reserved** | Early boot memmap | Bits set in `reserved_bitmap` | Includes frame 0, PMM bitmap backing, kernel binary, Limine data, ACPI/EFI, and non-RAM physical address holes. |

---

### 3. Diagnostic Headroom Signal Semantics & Test Evidence

In accordance with project policy, no new attribution counters, allocator trimming, or pressure policies are introduced. The diagnostic signal focuses on observable headroom with explicit, well-defined semantics:

1. **Physical Frame Headroom (`pmm_allocatable_frames`)**:
   - **Definition**: The instantaneous count of free physical frames residing below the **current allocation ceiling** (`alloc_limit_pages`), observed under Rank-4 `pmm_lock`.
   - **Ceiling Expansion**:
     * During early boot (before `pmm_unlock_high_memory()` is called), `alloc_limit_pages` is capped at 1 GiB (`PMM_BOOT_ALLOC_LIMIT / PAGE_SIZE = 262,144` frames). Free frames above 1 GiB remain temporarily ineligible, so `allocatable_frames <= free_pages`.
     * After high-memory unlock (`pmm_unlock_high_memory()`), `alloc_limit_pages = total_pages`. The ceiling expands to cover all managed memory, so `allocatable_frames == free_pages`.
   - **Evidence Boundary & Linked Verification**:
     * The 256/512 MiB QEMU matrix cannot distinguish a 1 GiB cap from unlocked high memory because total RAM is entirely below 1 GiB.
     * Actual ceiling enforcement, capped exhaustion, ceiling straddling, unlock gating, and post-unlock allocation are tested in [`tests/pmm_boot_host.c`](../tests/pmm_boot_host.c#L50-L140) (which exercises memory maps with entries spanning the 1 GiB boundary and high RAM at 30 GiB).

2. **Heap Reusable Capacity Headroom (`heap_free_bytes` & `heap_largest_payload`)**:
   - **Definition**: `heap_free_bytes` represents aggregate reusable capacity across all free blocks in committed virtual heap backing. `heap_largest_payload` represents the payload of the **single largest existing free block** currently linked in `g_free_list_head` (`hdr->size - 32` bytes).
   - **Valid Request Fit & Realizable Alignment Boundaries**:
     * For valid, nonzero, overflow-safe requests at the observed instant, a request can be satisfied from existing free capacity if and only if $\text{ALIGN\_UP}(N, 16) \le \text{heap\_largest\_payload}$ (with minimum payload 16 bytes).
     * Under the allocator's 16-byte alignment invariant (`HEAP_ALIGNMENT = 16`, minimum block size 48 bytes), free-block payloads are always multiples of 16 (16, 32, 48, ...). An unaligned request (e.g. 17 bytes) requires a 32-byte payload; if the largest payload is 16, the request cannot be accommodated.
   - **Coalescing Semantics**:
     * Block coalescing in FortressOS is an **immediate and synchronous** property of `kfree()`. It is not a deferred or future background reclaim mechanism.
     * Existing free blocks are already coalesced to their maximal possible extent; separate free blocks will not merge unless intervening live blocks are freed.
   - **Linked Evidence**:
     * Heap reuse from existing capacity without consuming PMM frames under PMM OOM is proven in [`tests/heap_memory_host.c`](../tests/heap_memory_host.c#L224-L231) and [`src/mm/memory_boot_test.c`](../src/mm/memory_boot_test.c#L400-L415).
     * Fragmented PMM capacity (single-page success vs. multi-page contiguous failure) is proven in [`src/mm/memory_boot_test.c`](../src/mm/memory_boot_test.c#L1350-L1370) and [`tests/pmm_audit_host.c`](../tests/pmm_audit_host.c#L100-L140).

3. **Closure and Next Decision**:
   - Diagnostic headroom verification is complete. Existing counters in `SYS_MEMINFO = 56` and `/bin/sysinfo -m` provide well-defined, non-breaking observability.
   - No new allocator feature (e.g. per-CPU page caches or physical heap trimming) is justified by the evidence.
   - The next decision was authorized to resume measured SMP profiling (measurement-only) rather than adding speculative allocator complexity.

---

## Measured SMP profiling: kernel-stack allocation & free decomposition (1, 4, and 8 CPUs)

Campaign directory: `build/smp-memory-profile-20261009T211110375787Z`.
Authoritative record: `build/smp-memory-profile-20261009T211110375787Z/result.json` (6/6 PASS).
Prior baseline directory (pre-start-gate snapshot): `build/smp-memory-profile-20261009T210110431199Z`.

### 1. Workload & Invariants Under Test
- Bounded, repeatable kernel-stack allocation & free workload (`kstack_alloc_tracked` and `kstack_free_tracked`) executing pinned workers across CPU cores.
- Verification matrix:
  - BIOS 1 CPU (profiled + uninstrumented control)
  - BIOS 4 CPUs (profiled + uninstrumented control)
  - BIOS 8 CPUs (profiled + uninstrumented control)
- Workload sizing: 2 pinned workers per CPU, 50 allocate/touch/free iterations each (100 operations per CPU; 100 ops at 1 CPU, 400 ops at 4 CPUs, 800 ops at 8 CPUs).
- Verified invariants:
  - Worker CPU stability: each worker verifies execution on its assigned CPU (`cpu_current()->id == assigned_cpu`).
  - Coordination gates: workers synchronize at a start gate before timed loops and park at a done gate before termination.
  - Zero resource leaks: exact baseline restoration for heap used bytes, allocated blocks, VMM table frames, PMM free pages, drained deferred queue, and byte-for-byte PMM bitmap equality (`pmm_snapshot`).
  - Non-interference: uninstrumented controls pass with clean boot and identical hash preservation for protected plans and default images (`df1843426c85...`).

#### 2. Measured Elapsed TSC Ticks and Subsystem Decomposition

All clock values are ordered elapsed TSC ticks read via `lfence; rdtsc; lfence`. They represent elapsed time on the invariant TSC timebase, not instruction counts or execution cycles.

#### Isolated Workload Delta Accounting (Excluding Worker Setup)

In the authoritative 18-run campaign (`build/smp-memory-profile-20261010T085711159410Z/result.json`, 18/18 PASS spanning 3 repetitions of profiled and matching untracked control runs across 1, 4, and 8 CPUs), taking baseline snapshots after all worker threads are created and parked at the start gate cleanly isolates the benchmark iterations:
- Exact 2.0 shootdowns per successful stack lifecycle (1 during `vmm_map_pages`, 1 during `vmm_unmap_pages`): exactly 200 calls for 100 ops at 1 CPU, 800 calls for 400 ops at 4 CPUs, and 1,600 calls for 800 ops at 8 CPUs. *(Scope qualification: this invariant applies specifically to this successful kernel-stack allocation/free benchmark workload where `pml4_index >= 256` triggers `needs_invalidation`; it does not apply universally to every stack path, such as OOM rollback before mapping, partial mapping faults, or unmapped slot operations).*
- Exact 8 PMM acquires per stack lifecycle (4 frames allocated on map, 4 frames freed on unmap): exactly 800 acquires at 1 CPU, 3,200 acquires at 4 CPUs, and 6,400 acquires at 8 CPUs.

#### Repeated-Run Distributions and Matching Gated Telemetry Overhead

Comparing matching uninstrumented gated controls (`smp_memory_test=kstack_control` executing the identical 50-iteration alloc/touch/free worker loop) against instrumented profiling across 3 repetitions per configuration demonstrates that overhead is well-bounded on matching gated workload intervals (`overall_elapsed_tsc` between start gate and worker quiescence):

| Geometry | Control Wall (min / med / max) | Profiled Wall (min / med / max) | Wall Overhead (%) | Control Gated TSC (min / med / max) | Profiled Gated TSC (min / med / max) | Gated TSC Overhead (%) | Avg Alloc Ticks / Op (min / med / max) | Avg Free Ticks / Op (min / med / max) |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **1 CPU** | 6.51s / 6.55s / 6.56s | 6.79s / 6.80s / 6.80s | +3.82% | 5.09M / 5.14M / 5.16M | 6.06M / 6.13M / 6.13M | +19.26% | 25,386 / 25,720 / 25,785 | 25,544 / 25,653 / 25,843 |
| **4 CPUs** | 8.39s / 8.41s / 8.41s | 8.65s / 8.73s / 8.88s | +3.80% | 40.15M / 42.62M / 51.92M | 46.94M / 56.83M / 78.94M | +33.34% | 205,480 / 281,708 / 370,906 | 209,463 / 242,901 / 460,247 |
| **8 CPUs** | 8.88s / 8.93s / 9.00s | 9.18s / 9.21s / 9.47s | +3.14% | 130.25M / 132.56M / 190.68M | 139.11M / 141.38M / 170.79M | +6.65% | 826,446 / 929,073 / 1,082,162 | 938,779 / 1,159,940 / 1,316,916 |

*Methodological note: Overhead is evaluated on matching gated workload intervals, resolving earlier reporting gaps that compared whole-run host wall seconds.*

#### Fine-Grained Same-Scope Stack Sub-Interval Decomposition

Measuring same-scope contiguous sub-intervals directly inside `kstack_alloc_tracked`, `kstack_free_tracked`, `vmm_map_pages_tracked`, and `vmm_unmap_pages_tracked` yields exact empirical decomposition (median values across repetitions):

| Subsystem Interval | 1 CPU (100 ops) | 1 CPU % | 4 CPUs (400 ops) | 4 CPUs % | 8 CPUs (800 ops) | 8 CPUs % |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **Stack Alloc: `slot_wait`** | 104,801 | 2.0% | 2,099,517 | 1.9% | 8,981,552 | 1.2% |
| **Stack Alloc: `slot_hold`** | 108,318 | 2.1% | 682,763 | 0.6% | 2,319,010 | 0.3% |
| **Stack Alloc: `alloc_prep`** | 194,870 | 3.8% | 2,370,166 | 2.1% | 11,863,001 | 1.6% |
| **Stack Alloc: `pmm_alloc`** | 563,331 | 11.0% | 9,380,170 | 8.3% | 51,755,564 | 6.9% |
| **Stack Alloc: `map_prep`** | 70,798 | 1.4% | 928,624 | 0.8% | 3,528,835 | 0.5% |
| **Stack Alloc: `vmm_map_wait`** | 63,555 | 1.2% | 4,892,061 | 4.3% | 36,308,264 | 4.9% |
| **Stack Alloc: `vmm_map_hold`** | 436,897 | 8.5% | 6,558,603 | 5.8% | 23,988,948 | 3.2% |
| *(Page-Table work within `map_hold`)* | *316,191* | *72.4%* | *5,292,500* | *80.7%* | *18,998,439* | *79.2%* |
| **Stack Alloc: `vmm_map_misc`** | 295,496 | 5.8% | 24,631,511 | 21.9% | 146,669,771 | 19.7% |
| **Stack Alloc: `map_shootdown`** | 0 | 0.0% | 44,973,565 | 40.1% | 215,606,898 | 29.0% |
| **Stack Alloc: `alloc_tail`** | 86,431 | 1.7% | 1,071,315 | 1.0% | 3,611,224 | 0.5% |
| **Stack Free: `unmap_wait`** | 63,294 | 1.2% | 4,930,761 | 5.0% | 31,081,556 | 3.3% |
| **Stack Free: `unmap_hold`** | 682,900 | 13.3% | 10,720,981 | 10.9% | 27,194,185 | 2.9% |
| *(Page-Table work within `unmap_hold`)* | *562,397* | *82.4%* | *9,552,075* | *89.1%* | *23,229,919* | *85.4%* |
| **Stack Free: `unmap_misc`** | 246,654 | 4.8% | 13,170,059 | 13.4% | 232,019,348 | 25.0% |
| **Stack Free: `unmap_shootdown`** | 0 | 0.0% | 38,413,159 | 39.1% | 207,302,256 | 22.3% |
| **Stack Free: `free_mid`** | 64,048 | 1.2% | 739,057 | 0.8% | 2,401,180 | 0.3% |
| **Stack Free: `pmm_free`** | 629,664 | 12.3% | 14,818,862 | 15.1% | 127,141,826 | 13.7% |
| **Stack Free: `free_tail`** | 40,739 | 0.8% | 527,240 | 0.5% | 1,927,051 | 0.2% |
| **Stack Free: `slot_free_wait`** | 107,139 | 2.1% | 1,616,927 | 1.6% | 6,248,046 | 0.7% |
| **Stack Free: `slot_free_hold`** | 39,070 | 0.8% | 286,832 | 0.3% | 1,182,747 | 0.1% |

#### Two-Layer Reconciliation Model & Mathematical Attribution

To ensure mathematical attribution integrity and eliminate double-counting, timing analysis uses two separate conceptual layers:

- **Layer 1: Continuous Non-Overlapping Lifecycle Partition**:
  $$\text{full\_lifecycle} = \text{kstack\_lock\_wait} + \text{kstack\_lock\_hold} + \text{pmm\_time} + \text{vmm\_lock\_wait} + \text{vmm\_lock\_hold} + \text{vmm\_misc} + \text{shootdown} + \text{wrappers} + \text{residual}$$
  - Full lifecycle is measured continuously around `kstack_alloc_tracked` and `kstack_free_tracked`.
  - In `kstack_alloc`: `slot_wait` $\rightarrow$ `slot_hold` $\rightarrow$ `alloc_prep` $\rightarrow$ `pmm_alloc` $\rightarrow$ `map_prep` $\rightarrow$ `vmm_map_wait` $\rightarrow$ `vmm_map_hold` $\rightarrow$ `vmm_map_misc` $\rightarrow$ `map_shootdown` $\rightarrow$ `alloc_tail`.
  - In `kstack_free`: `unmap_wait` $\rightarrow$ `unmap_hold` $\rightarrow$ `unmap_misc` $\rightarrow$ `unmap_shootdown` $\rightarrow$ `free_mid` $\rightarrow$ `pmm_free` $\rightarrow$ `free_tail` $\rightarrow$ `slot_wait` $\rightarrow$ `slot_hold`.
  - Monotonic containment and non-overlap are strictly verified on every run ($t_{\text{alloc}} \ge \text{alloc\_partition}$, $t_{\text{free}} \ge \text{free\_partition}$, and $\text{residual} \ge 0$).
  - Shootdowns execute strictly *after* releasing `g_vmm_lock` and are included exactly once.
- **Layer 2: Nested Interrupt Overlay (Remote IPI Servicing)**:
  - Servicing of remote TLB shootdown interrupts by peer CPUs is tracked independently via `smp_ipi_tlb_handler` (`g_ipi_tlb_service_tsc`) and snapshotted across the gated workload.
  - **IPI servicing is NEVER summed into the Layer 1 partition**. An IPI interrupt occurring during a timed PMM or VMM interval is already inherently included in that interval's elapsed time; adding remote IPI servicing again as an independent partition component would constitute double-counting.

###### Authoritative Per-Run Reconciliation Table (`build/smp-memory-profile-20261010T100909874884Z/result.json`):

| Run | Full Lifecycle (ticks) | Residual (%) | Shootdown (%) | PMM (%) | VMM Misc (%) | VMM Lock Wait (%) | VMM Lock Hold (%) | Wrappers (%) | KStack Lock (%) | Remote IPI Overlay (%) |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **smp1-r1** | 5,038,724 | 25.40% | 0.00% | 25.10% | 8.81% | 3.25% | 19.98% | 10.40% | 7.07% | 0.00% (0 IPIs) |
| **smp1-r2** | 5,078,518 | 27.02% | 0.00% | 25.10% | 8.46% | 3.32% | 20.37% | 8.40% | 7.33% | 0.00% (0 IPIs) |
| **smp1-r3** | 4,964,285 | 25.68% | 0.00% | 25.67% | 8.24% | 3.86% | 20.91% | 9.99% | 7.64% | 0.00% (0 IPIs) |
| **smp4-r1** | 191,323,252 | 16.04% | 39.62% | 13.07% | 11.79% | 5.19% | 7.58% | 3.84% | 2.89% | 5.58% (1,983 IPIs) |
| **smp4-r2** | 186,533,984 | 15.78% | 52.22% | 8.96% | 6.79% | 3.94% | 4.64% | 2.42% | 5.25% | 5.48% (1,937 IPIs) |
| **smp4-r3** | 286,620,766 | 19.53% | 36.38% | 14.06% | 12.86% | 6.15% | 8.53% | 3.34% | 1.50% | 3.66% (1,909 IPIs) |
| **smp8-r1** | 1,984,666,974 | 35.76% | 25.03% | 13.10% | 17.00% | 4.01% | 2.87% | 1.23% | 1.01% | 3.24% (5,908 IPIs) |
| **smp8-r2** | 1,538,897,284 | 29.08% | 30.45% | 19.94% | 9.64% | 5.10% | 3.12% | 1.50% | 1.17% | 3.98% (5,923 IPIs) |
| **smp8-r3** | 1,591,627,577 | 31.91% | 26.25% | 10.00% | 17.07% | 4.88% | 2.92% | 5.81% | 1.15% | 4.07% (6,055 IPIs) |

##### Distribution Summaries across Geometries (min / median / max):

- **1 CPU (100 ops, 3 repetitions, gated overhead +64.53%, wall overhead +2.90%)**:
  - Full Lifecycle: 4.96M / 5.04M / 5.08M ticks.
  - Unmeasured Residual: 25.40% / 25.68% / 27.02% (1.27M / 1.29M / 1.37M ticks).
  - Outer-vs-Inner Breakdown: Invocation gap = **6.95% / 7.32% / 7.55%**, Internal gap = **17.77% / 18.62% / 19.47%**.
  - Shootdowns: 0.00% (local INVLPG only, 0 broadcast).
  - PMM Time: 25.10% / 25.10% / 25.67%.
  - VMM Lock Wait: 3.25% / 3.32% / 3.86%; Lock Hold: 19.98% / 20.37% / 20.91%.
  - VMM Misc & Setup: 8.24% / 8.46% / 8.81% (pre-lock 1.4%–1.8%, post-prep 3.8%–4.0%, put-op wait 1.9%–2.0%, put-op hold 1.0%).
  - Allocation Wrappers: 8.40% / 9.99% / 10.40%.
  - KStack Slot Lock (Wait+Hold): 7.07% / 7.33% / 7.64%.
  - Remote IPI Servicing Overlay: 0.00% (0 ticks).
- **4 CPUs (400 ops, 3 repetitions, gated overhead -10.56%, wall overhead +5.66%)**:
  - Full Lifecycle: 186.53M / 191.32M / 286.62M ticks.
  - Unmeasured Residual: **15.78% / 16.04% / 19.53%** (29.4M / 30.7M / 56.0M ticks).
  - Outer-vs-Inner Breakdown: Invocation gap = **1.60% / 3.24% / 3.44%**, Internal gap = **12.54% / 12.61% / 17.93%**.
  - TLB Shootdowns: 36.38% / 39.62% / 52.22% (dispatch 27.8%, ack poll 58.7%, service 13.5%).
  - VMM Misc Partition: 6.79% / 11.79% / 12.86% (pre-lock 0.7%–1.6%, post-prep 2.5%–4.6%, put-op wait 3.3%–6.3%, put-op hold 0.3%–0.6%).
  - PMM Time: 8.96% / 13.07% / 14.06%.
  - VMM Lock Wait: 3.94% / 5.19% / 6.15%; Lock Hold: 4.64% / 7.58% / 8.53%.
  - Allocation Wrappers: 2.42% / 3.34% / 3.84%.
  - KStack Slot Lock (Wait+Hold): 1.50% / 2.29% / 5.25%.
  - Remote IPI Servicing Overlay: 3.66% / 5.48% / 5.58% (1,909 / 1,937 / 1,983 IPIs; 10.2M / 10.5M / 10.7M ticks).
- **8 CPUs (800 ops, 3 repetitions, gated overhead +4.46%, wall overhead +2.34%)**:
  - Full Lifecycle: 1.54B / 1.59B / 1.98B ticks.
  - Unmeasured Residual: **29.08% / 31.91% / 35.76%** (447M / 508M / 710M ticks).
  - Outer-vs-Inner Breakdown: Invocation gap = **0.89% / 1.53% / 2.07%**, Internal gap = **27.01% / 30.38% / 34.86%**.
  - TLB Shootdowns: 25.03% / 26.25% / 30.45% (dispatch 24.1%, ack poll 46.9%, service 29.0%).
  - VMM Misc Partition: 9.64% / 17.00% / 17.07% (pre-lock 0.4%–0.6%, post-prep 2.8%–10.9%, put-op wait 5.4%–5.9%, put-op hold 0.27%–0.31%).
  - PMM Time: 10.00% / 13.10% / 19.94%.
  - VMM Lock Wait: 4.01% / 4.88% / 5.10%; Lock Hold: 2.87% / 2.92% / 3.12%.
  - Allocation Wrappers: 1.23% / 1.50% / 5.81%.
  - KStack Slot Lock (Wait+Hold): 1.01% / 1.15% / 1.17%.
  - Remote IPI Servicing Overlay: 3.24% / 3.98% / 4.07% (5,908 / 5,923 / 6,055 IPIs; 61.2M / 64.3M / 64.8M ticks).

#### Decomposition of Shootdown Phases and Page-Table Work

Decomposing the internal phases of shootdown invalidation and VMM lock hold reveals the exact distribution of serialized time:

1. **Page-Table Work inside VMM Lock Hold**:
   - In all geometries, **78–83% of VMM lock hold time is spent executing page-table hierarchy walks and mutating PTEs (`vmm_map_pt` and `unmap_pt`)**. Lock hold does not include shootdowns, zeroing, or PMM allocation.
2. **Shootdown Serialization Phases**:
   - **ACK Polling (`ack_poll`)**: Accounts for **47.6%–60.9%** (4 CPUs, median 58.7%) and **44.9%–48.6%** (8 CPUs, median 46.9%) of shootdown initiator time.
   - **Mailbox Setup & IPI Dispatch (`dispatch`)**: Accounts for **13.6%–29.0%** (4 CPUs, median 27.8%) and **23.8%–25.2%** (8 CPUs, median 24.1%) of shootdown time.
   - **Local Peer Servicing (`map_service` / `unmap_service`)**: Accounts for **10.7%–23.4%** (4 CPUs, median 13.5%) and **26.5%–30.0%** (8 CPUs, median 29.0%) of initiator time.
3. **Partitioning of Secondary VMM Contention (`vmm_put_op_wait`)**:
   - Splitting `vmm_misc` proves that op-reference release lock wait accounts for **5.4%–5.9% of lifecycle time at 4 and 8 CPUs** (and up to 6.3% in some runs), confirming secondary acquisition of `g_vmm_lock` in `vmm_space_put_op` is a measured contention contributor.

### 3. Explicit Hypotheses & Residual Audit

1. **Outer vs. Inner Residual Audit**:
   - Measuring entry/exit timestamps of `kstack_alloc_tracked` and `kstack_free_tracked` (`inner_tsc`) proves that outer invocation/return overhead accounts for only **0.9%–2.1% (median 1.5%) of lifecycle time at 8 CPUs**, and **1.6%–3.4% (median 3.2%) at 4 CPUs**.
   - The remaining residual (median **30.4% at 8 CPUs**, **12.6% at 4 CPUs**) resides *internally* between the tracked sub-intervals (e.g. gaps between PMM allocation, page-table mapping, op-reference release, and slot unlinking), rather than function call setup/teardown.
2. **Hypothesis 1: Measurement Perturbation & Concurrency Variance**:
   - Three repetitions under QEMU MTTCG are insufficient to establish a single precise percentage for diagnostic perturbation. Gated overhead varies from -10% to +15% depending on host scheduling of vCPU threads and lock competition.
3. **Hypothesis 2: Host Scheduling & MTTCG Concurrency Skew**:
   - QEMU Multi-Threaded TCG executes vCPUs on host OS threads. Spin durations in ACK polling (47%–59% of shootdown time) and peer service times are influenced by host thread preemption, meaning TCG measurements reflect host scheduling stretch rather than bare-metal hardware bus latency.
4. **Hypothesis 3: Internal Sub-interval Residual Attribution**:
   - The internal gap between tracked sub-intervals (30.4% at 8 CPUs) is hypothesized to arise from compiler register saving/spilling across C function boundaries, loop overhead inside batch allocators, and asynchronous interrupt handling occurring during uninstrumented code windows between timed blocks. It cannot be assumed that compiler function entry/exit explains the residual.

### 4. Claim Audit & Verdict: "Synchronous TLB Shootdown Waits Dominate Kernel-Stack Lifecycle"

**Verdict: Explicitly SUPERSEDED and Narrowed.**

Earlier working hypotheses are explicitly superseded by empirical full-lifecycle measurements:

1. **"VMM Operations Dominate 80–86% of Lifecycle Time"**: **SUPERSEDED**.
   - VMM lock wait and lock hold combined account for only **~5.5%–5.7% of lifecycle time at 8 CPUs** (wait: 3.3%, hold: 2.3%).
2. **"Synchronous TLB Shootdowns Explain the Majority of Lifecycle Cost"**: **SUPERSEDED**.
   - Shootdowns account for **36.4%–44.4% (median 38.1%) of lifecycle time at 4 CPUs**, and **28.2%–29.0% (median 28.9%) at 8 CPUs**. They represent a substantial component under concurrency, but they do **NOT** constitute a majority (>50%) of kernel-stack lifecycle time.
3. **"Stack-Touch Memory Writes Explain the Unmeasured Residual"**: **SUPERSEDED**.
   - Stack memory touches (`*stack_top = 0xAA`, `*stack_bot = 0x55`) execute strictly *between* $t_{\text{alloc\_end}}$ and $t_{\text{free\_start}}$, outside the measured allocation and free intervals. They cannot mathematically contribute to lifecycle residual.
4. **"Remote IPI Servicing is Part of Residual"**: **SUPERSEDED**.
   - Remote IPI servicing occurring during timed PMM, VMM, or wrapper intervals is already captured within those intervals' elapsed timestamps. Attributing all remote servicing as residual or adding it as an independent additive term is mathematically invalid. It is properly modeled as an independent nested overlay (Layer 2), consuming ~3.3%–3.4% of lifecycle duration at 8 CPUs.

#### Methodological & Concurrency Boundaries:
1. **PMM Wait Scaling**:
   - PMM contention frequency remains moderate (~18%–28% of acquires), but PMM elapsed time scales under concurrency (from 1.17M ticks at 1 CPU to 23.8M ticks at 4 CPUs and 136M ticks at 8 CPUs). PMM operations consume ~7%–12% of total lifecycle time.
2. **QEMU Multi-Threaded TCG Realities**:
   - QEMU 8.2 runs Multi-Threaded TCG (`mttcg`) for x86_64, running vCPUs on separate host OS threads. However, spin-polling loops under host thread scheduling can still exhibit stretch relative to bare-metal hardware.
3. **Mathematical Aggregation Integrity**:
   - Summed CPU intervals represent aggregate core-ticks and must not be divided by the single-timeline wall clock (`overall_elapsed_tsc`). Residuals are strictly non-negative on every individual run, proving absence of double-counting or component overlap.

### 4. Architectural Conclusions & Hardware Next Steps

1. **No Allocator Redesign Justified**:
   - Neither per-CPU PMM page caches (which would address at most ~7–12% of cost while introducing major reclamation complexity) nor heap trimming are justified by the evidence.
2. **Protected Invariants Fully Preserved**:
   - Synchronous invalidation guarantees memory safety and TLB coherence. All VMM lifetime pins, atomic walks, deferred teardowns, and scheduler handoff invariants through commit `26cd041` remain intact.
3. **Specific Hardware Question for Dell Latitude 5590**:
   - With QEMU multi-core overhead, sub-intervals, and two-layer reconciliation fully quantified, testing on physical hardware is deferred until a specific bare-metal latency question arises:
     *On physical Intel Core i5-8350U hardware with dedicated execution cores and hardware interconnects, does hardware IPI delivery reduce ACK polling duration proportionally, and does VMM miscellaneous/table work scale differently than under host MTTCG emulation?*
