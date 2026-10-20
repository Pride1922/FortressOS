# Descriptor synchronization and latency review

2026-10-20. Design review only: no syscall allocation, barrier removal, writeback
thread, device-ownership change or lock redesign is implemented here.

The [initial EH-5 baseline](../roadmap/ext4-latency.md) now measures staging
separately from final sync: 640 writes pay 3,840 flushes; final sync adds one.
This confirms that descriptor scope alone does not address repeated write
barriers. Production optimization and a new syscall remain unimplemented.

The [bounded staging batch proposal](EXT4_STAGING_BATCH_DESIGN.md) reviews an
explicit synchronous batch as the first prototype, with userspace buffering,
geometry/credit caps, unchanged ordinary writes, and separate fd-scoped sync.
It also explains why retaining the shared journal transaction across calls is
not an acceptable shortcut. A release-excluded
[prototype](../roadmap/ext4-stage-prototype.md) now implements the batch and
selected-mount sync adapters; this is not a delivered production ABI or a
responsiveness acceptance claim.

## What the current path actually does

Successful journaled EXT4 writes already commit and checkpoint synchronously.
SYS_WRITE accepts at most 16 KiB; the backend accepts at most 32 KiB. A 10 MiB
staging file written through the syscall therefore needs at least 640 writes,
each using the existing transaction/barrier sequence. SYS_SYNC is an additional
global drain/barrier, not the operation that first writes all 10 MiB of dirty
inode pages. There is no deferred per-inode dirty-page queue today.

`storage_sync_all()` attempts installed root and external storage separately.
`ext4_sync()` holds the existing rank-1 IRQ-save filesystem lock through orphan
drain and polling device I/O. Other filesystem callers wait for that exclusion;
the servicing CPU also has IRQs disabled. This can cause latency. Contention
alone does not prove deadlock: current backends must not sleep, enable IRQs,
reenter VFS, acquire another rank-1 lock, or await work requiring that exclusion.
Those contracts remain binding. Software polling deadlines also do not establish
a measured user-visible responsiveness bound for every supported device.

The block interface exposes a device flush, not an inode-specific persistence
command. Shared journal, inode-table, bitmap and directory blocks further couple
metadata durability. An fd-scoped filesystem operation cannot honestly promise
that the device persists only that inode or that it never stalls another writer.

## Proposed SYS_FSYNC contract to review before implementation

A future additive `SYS_FSYNC(fd)` should select the retained opened object and
its filesystem, avoiding unrelated mount drains. Descriptor validation, retained
file-reference lifetime, unsupported streams/backends, eligible directory
descriptors, error precedence and concurrent close must be specified and tested.
Existing SYS_SYNC semantics and stat/mount-info layouts must stay unchanged.

For a regular file, define durability of its accepted data, length and inode
metadata. Name publication is a directory operation: synchronizing the file is
not a substitute for durably creating/renaming its directory entry. Define parent
directory synchronization and cross-directory rename ordering explicitly. The
current synchronous namespace operations already checkpoint those edits.

Initially an fd-scoped operation may still require the selected filesystem's
shared journal/device barrier. State that cost explicitly. Do not implement a
global SYS_SYNC wrapper and describe it as isolated per-inode flushing. A new
syscall alone does not remove the repeated barriers in the staging-write path.
Likewise, merely dropping the filesystem lock around a flush is unsafe without
retained mount/device ownership, transaction sequencing, cache-publication fences,
concurrent-writer ordering, freeze exclusion and late-error ownership.

## Measurement and implementation gates before PKG-2 mutation

Measure staging writes and the final barrier separately on disposable NVMe and
USB-emulated fixtures. Record bytes, write/flush counts, journal/checkpoint time,
lock wait/hold distributions and foreground/SSH/logging latency. Include a slow
or failed flush fixture; distinguish contention, IRQ-disabled stalls, timeouts
and an actual wait dependency cycle. QEMU results do not establish physical
controller cache quality or physical latency acceptance.

Choose the latency budget and change one bounded mechanism at a time. A scoped
fd API can address mount selection/error attribution; reducing repeated barriers
or moving blocking work outside IRQ-save exclusion is a separate protected
design decision. Preserve the old/new replacement and recovery gates. Any chosen
implementation needs fd/type/error/close races, concurrent append and directory
publication tests, actual late I/O failures, BIOS/UEFI and independent Linux
audits. No claim that SYS_SYNC is intrinsically crash-unsafe, or that SYS_FSYNC
alone makes package updates safe, follows from this review.
