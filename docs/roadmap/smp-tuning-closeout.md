# SMP performance tuning closeout — 2026-10-09

Performance tuning is paused at the user's request. This does not assert that
SMP performance is solved. Retain the verified VMM/lifecycle/context-handoff
correctness fixes, stack mapping batching and fast ELF initialization. The
default reschedule policy services urgent waiter/signal requests; fresh-work
hints wake idle CPUs without forcing a busy user task to yield. Its latency
improvement is established; it is not a universal throughput win.

The final Dell one-boot writer-batching comparison rejects 4 KiB batching:
plain pooled medians 13.747 ms (1 KiB) versus 17.7855 ms (4 KiB), a 29.4%
regression. The established 1 KiB default remains. All eight pipe exports and
the spawn check validated. The preceding handoff export has an unidentified
second set in B1 and no confirmed A2; do not treat it as a complete ABBA trial.

`benchrun` was an experiment-only capture tool and is removed from source,
build targets and initramfs. Its dedicated guest/host runners are removed;
historical benchmark procedure documents remain as evidence, not current
commands. The standalone smpbench and reusable core correctness tests remain.

The user explicitly requested deletion of the entire build folder and all
generated comparison images. Host execution policy blocked both bulk deletion
and individual generated-file deletion; cleanup requires a user-side action.
Previously tracked physical evidence notes are relocated to
docs/roadmap/evidence before cleanup. Conclusions in reports are retained;
after cleanup, do not claim independent revalidation of deleted raw evidence.
Initramfs packaging explicitly excludes stale staged benchrun binaries.

The normal image builder now defaults to journaled EXT4, including a clean
checksummed JBD2 journal. Fresh images go to bin/fortress.img. Legacy ext2
requires an explicit --filesystem ext2 selection; no-journal EXT4 workbench
fixtures require explicit ext4-nojournal. The normal build uses neither.

Earlier Dell SMP captures were collected with ext2 images. Their comparisons
describe those conditions, not journaled EXT4 performance. The effect of the
filesystem difference on benchmark output and overall timing was not measured;
do not extrapolate those numbers to the new EXT4 default.
No physical disk is flashed by the agent. Memory work is a subsequent task.

Fresh default-image acceptance: BIOS and UEFI USB boot at SMP=8, create/sync/
shutdown/reboot/read/unlink, with independent Linux e2fsck after each boot and
an unchanged shipped-image hash. Host pipe/VMM/stack/ELF/wait/reschedule gates
also pass. This verifies the packaging and persistence path, not Dell speed.
