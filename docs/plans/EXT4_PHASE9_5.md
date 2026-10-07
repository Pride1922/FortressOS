# EXT4 Phase 9.5 — Journaled USB disposable fixtures

Execution plan, 2026-10-06. Prerequisite: the declared Phase-9.4 guest gate
passed 66/66 cases and evidence review; see [results](../roadmap/ext4-phase9-4.md).
The declared automated gate passed on 2026-10-07: 144 main cases, ten separate
admission controls, relevant regressions and evidence review. See the
[implementation and limits](../roadmap/ext4-phase9-5.md). Physical acceptance
and production activation remain separate.

## Goal and boundaries

Verify bounded E4-B journal mutation and recovery through the actual xHCI/BOT
USB block adapter. Keep production journaled RW disabled, default ext2 and
accepted non-journaled E4-A intact. Preserve metadata-cache coherence, taint,
durability barriers, lock ranks, bounded polled I/O and DMA quarantine.

Use QEMU regular-file fixtures only. Dell 5590/5530 and the two physical sticks
belong to Phase 9.6, after a separate procedure identifies the sacrificial
target. No physical preparation, flashing, formatting or power cuts belong
to this implementation.

## 9.5a — Audit and fixture admission

Read PROTECTED.md, AGENTS.md §§4/7.3–7.5/7.7/9, block.h, ext4.h, jbd2.h,
usb_mount.h, xhci_bot.h and the USB/storage annexes before editing. Trace
selection, GPT validation, durability classification, block write/flush,
SYS_SYNC and shutdown calls, including the existing journal-fixture fallback.

Add a separate explicit QEMU-only journaled USB fixture path. Reuse
`ext4_mount_journal_fixture` with its existing admission contract after unique
target selection, primary-consistent GPT and successful durability admission.
Do not add journal activation to `usb_mount_production_storage`.
Prevent simultaneous E4-A, NVMe journal and USB journal fixture admission;
reject ambiguity before filesystem writes or VFS publication.

Record fixture PARTUUID, filesystem/journal UUID, exact feature masks, geometry,
transport, selected durability tier and mounted mode. Unknown/ineligible
durability cannot be overridden by the fixture flag. RO fixtures remain
immutable and never replay. Labels alone do not authorize writes.

Create owned GPT images from explicit E4-B recipes, separately labelled from
E4-A and ext2. Prepare an isolated source build and matching immutable ISO/ELF
copies. Every final QEMU argv must permit exactly one USB data image, xHCI,
BOT storage and paired OVMF exceptions; reject NVMe, extra backends, physical
paths and implicit snapshots. Boot from the ISO, not the data image.

Gate: eligible fixture recovery reaches the existing journal mount API;
wrong/duplicate target, RO, degraded/conflicting GPT and failed admission
publish no RW mount and make zero filesystem writes.

## 9.5b — Normal persistence and synchronization

Start with BIOS, 1 KiB blocks, SMP=1. Complete the vertical case before matrix
expansion. Initial matrix: BIOS/UEFI × 1/2/4 KiB × SMP=1/4, 12 configurations.
Use native 512-byte logical sectors; retain 4096-sector adapter coverage as
host evidence unless a separately validated QEMU USB geometry is available.

Three boots per configuration, 36 boots total:

1. Recover the seeded journal/orphan, then create/write/append, grow, truncate,
   rename, unlink with live descriptors and reclaim/reuse. Sync mid-session,
   perform more mutations, sync again and freeze on shutdown.
2. Verify exact persisted bytes/namespace before rewriting. At SMP=4, execute
   shared and independent append on actual AP workers; check their CPU identity,
   all 200 distinct records and exact 3,200-byte files. Mutate again and reboot.
3. Verify the second boot's persisted results, clean up and shut down cleanly.

After every stopped guest, audit an extracted partition independently with
`e2fsck -fn`, exact bytes, namespace, inode links, allocation ownership,
orphan state and journal/clean-state checks. Successful sync must allow later
writes; only successful freeze/drain/barriers may produce a clean-state claim.
Include normal and wrapped journal placement in the declared fixture ledger.

## 9.5c — Native guest interruption and recovery

Reuse Phase-9.4 breakpoint techniques with the USB transport and fresh owned
images. Initial finite budget: 60 crash cases:

| Cases | Coverage |
| --- | --- |
| 36 | All 12 configurations × durable commit before checkpoint, checkpoint after one home write, and replay after one home write before publication |
| 12 | Six SMP=4 firmware/geometry configurations × shared/independent AP append |
| 12 | All configurations × durable open-unlink with live descriptors |

At each milestone retain writer/replay state, sequence, counts, stop packet,
CPU identity where applicable and the exact interrupted image. Linux
journal-only replay on a copy establishes the expected committed result,
followed by read-only fsck and semantic audits. A fresh FortressOS guest must
recover that result before any fixture rewrite can conceal it, then freeze
normally and pass independent integrity checks. Compare reclaimed allocation
bitmaps/counters against Linux for orphan cases.

Missed milestones, timeouts, unexplained rejection and fsck findings are
failures. QEMU termination retains the host page cache: it is a guest restart
test, not physical power loss. Host cache-loss/reorder/tear evidence stays
separate.

## 9.5d — Failure, removal and ownership

First validate the fault mechanism on a disposable vertical case. Prefer
backend errors and QMP removal that exercise real USB callbacks. If a case
needs a bounded test adapter, label that boundary and keep it behind explicit
fixture admission; do not describe an adapter failure as a BOT wire failure.
Freeze exact event targets and expected outcomes before launching the matrix.

Proposed budget: six profiles × 12 configurations, 72 failure cases:
admission flush failure, ordered-data write failure, journal write failure,
checkpoint write failure, mid-session sync flush failure and active-transfer
USB removal. Removal means safe terminal failure; reconnection and hot-plug
recovery are outside existing USB support.

Assert bounded completion, returned errors, preserved healthy prepublication
state where required, taint after uncertain I/O, no further mutation on a
failed device and no false clean shutdown. Inspect the existing DMA ownership
and quarantine state: uncertain buffers remain owned, never freed/reused.
Read failure diagnostics only after filesystem/transfer locks are released.
Do not introduce sleeps, IRQ enables, scheduling or new lock nesting under
the filesystem lock.

Keep durability evidence distinct: actual synchronous flush success,
verified write-through, accepted ASSUMED_WRITE_THROUGH policy, and deliberately
false flush success are different outcomes. The false-success/cache-loss
negative control belongs to the calibrated host model; it cannot establish
physical USB flush behavior. Admission failures and RO/ambiguous target
controls must hash unchanged filesystem images.

## 9.5e — Regressions and evidence review

Run existing E4-A USB persistence, ext2 USB persistence, USB mount-policy and
BOT sanitizer gates. If shared VFS/sync routing changes, also run ext2/storage,
SMP append and shutdown regressions appropriate to the affected paths.
If transaction code changes, rerun its relevant staging/credit, journal and
recovery gates; a test-only runner change does not justify repeating every
previous exhaustive host campaign.

Keep evidence under `.codex-remote-attachments/ext4-phase9`, outside shared
build outputs: exact commands, source/binary/tool hashes, fixture provenance,
argv, stop/transport diagnostics, audit outputs and failed images. Provide
a simple progress manifest with expected/completed/failed case counts.
Initial budget is 12 normal cases plus 60 crash and 72 failure cases; admission
controls and existing regression counts are separate. Freeze any necessary
budget revision before its campaign, with the reason and remaining gap.
Bound each wait and reap every owned process. Run one case first and measure
its duration before giving a matrix ETA; avoid an open-ended campaign.

Archive images with verified reconstruction hashes and verify unique case
coverage, actual AP execution, unchanged RO/rejected media and all independent
results. Publish `docs/roadmap/ext4-phase9-5.md` with passes, failures, explicit
adapter boundaries and remaining limits. Gate completion requires all declared
cases and relevant regressions to pass; missing coverage stays pending.

## Handoff

Phase 9.6 reviews the complete evidence and prepares separate physical
acceptance on deliberately identified media. A 9.5 pass does not enable
production journaling or authorize a physical power-loss experiment.
