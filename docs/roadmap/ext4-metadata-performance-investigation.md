# ext4 metadata and installation performance investigation — 2026-11-03

The slow paths are reproducible. The strongest installation finding is excessive
NVMe identity-command traffic, amplified by single-sector adapters. The strongest
boot finding is filesystem-wide metadata validation whose cost grows with volume
capacity, plus inefficient recovery-preview reads. Checksum CPU cost and weak
cache coverage add latency during ordinary lookups.

This is an investigation, not a production optimization. No filesystem format,
validation, journal ordering, identity checks, cache-coherence rules or durability
barriers were changed in the production tree. No laptop storage was written.

## Evidence and limits

- Source checkpoint: `9747416`, in an isolated investigation worktree, excluding
  parallel ACPI/battery edits.
- Earlier authenticated Dell capture: `build/dell-dmesg-audit/full.log`. Its kernel
  predates the boot-self-test separation. Its counters establish accumulated work,
  not a timestamped total boot duration.
- Host probe includes the actual VFS/ext4/JBD2 implementation and existing mount
  host adapters, with ordinary file-backed sector/run callbacks and pthread lock
  adapters. A stage hook captures counters/timing without changing algorithms.
  It mounts independent regular-file copies of the published factory root.
- Geometry control: same files, with the actual installer expansion code extending
  the 256 MiB seed to 4 GiB, 32 GiB and the Dell root's 499982991 sectors
  (255991291392 bytes, approximately 238.41 GiB).
- Experimental cache/CRC variants exist only in the private worktree. Each uses
  fresh copies. Timings below are three-run medians on the host, not Dell timings.
  The host's file cache and filesystem determine backend latency.
- Instrumented QEMU/KVM BIOS installation: one disposable 512 MiB NVMe target and
  a regular-file USB source, followed by installed BIOS SMP=4 boot with the USB
  absent. Login, application-directory listing, shutdown and Linux `e2fsck -fn`
  pass. Default kernel optimization and validation policies are retained.
- All scaled/variant host images also pass Linux filesystem checks after clean
  freeze. This does not constitute crash-cut acceptance of any proposed change.

Evidence, generated probe sources, private instrumentation diff, JSON counters,
compiler failures and successful runs are retained in
`build/ext4-metadata-investigation/`. The private checkout and installer serial
capture are under `.codex-remote-attachments/ext4-perf-investigation/build/perf/`.
The published installer SHA-256 remains
`6296e1610d3405c156d8341e506a67cde2a51043e7607a5f0d0ebe86cb4a81e9`.

## 1. Installation: identity validation overwhelms the command stream

[`install_runtime.c`](../../src/kernel/install_runtime.c) calls `revalidate()`
before each raw read, write and flush. The backend identity path reaches
[`nvme_installer_identity()`](../../src/drivers/nvme.c), which submits three NVMe
IDENTIFY commands: controller, active namespace list and namespace geometry.
These protect target identity and must not simply be deleted.

[`install_payload.c`](../../src/lib/install_payload.c) deploys ESP + root + BIOS
(64 + 256 + 1 MiB) in 4 KiB chunks. Every chunk is written and immediately read
back. That means six IDENTIFY commands for every 4 KiB of payload copied, before
counting the actual data I/O. The source callback copies already loaded boot
modules from RAM; USB file reads are not on this kernel deployment loop.
Firmware loading of those modules happens earlier and was not timed here.

Observed QEMU stage deltas:

| Stage | Elapsed seconds | IDENTIFY submissions | Data reads | Data writes |
| --- | ---: | ---: | ---: | ---: |
| Payload deployment | 77.171 | 493065 | 82177 | 82177 |
| Root provisioning | 12.600 | 82404 | 21280 | 5884 |
| Final immutable verification | 8.940 | 50148 | 16711 | 2 |

The measured admit-to-readiness-stage interval is 98.770 seconds; payload
installation occupies approximately 78% of that interval. This excludes preflight,
user interaction and the final post-readiness log update. Stage timing uses the
boot-calibrated TSC, not wall-clock guesses from progress percentages.

At entry to the readiness stage, the counter is 626057 IDENTIFY submissions.
The last later snapshot, before final log freeze, is 650738; this is a lower
bound on the complete installation's total. The three IDENTIFY response buffers
also move 12 KiB of administrative data for each revalidation. Payload deployment
alone therefore receives roughly 1.88 GiB of IDENTIFY data in addition to the
payload write/readback traffic.

This reproduces a slow installation without needing a failing disk or a large
root partition. QEMU command latency is not the Dell's latency, so the 77 seconds
must not be presented as a physical measurement or a promised speedup.

## 2. Single-sector adapters multiply metadata I/O

The installer's private `block_dev_t` in `install_runtime.c` exposes `part_read`
and `part_write` but no run callbacks. [`block_read_sectors()`](../../src/drivers/block.h)
therefore falls back to eight 512-byte calls for one 4 KiB metadata block. Each
of those installer calls repeats identity validation. A 4 KiB metadata read can
consequently require 24 IDENTIFY commands plus eight data commands.

The journal recovery-preview device in
[`ext4_mount_journal.inc`](../../src/fs/ext4_mount_journal.inc) also exposes only a
sector callback. It loses bulk-read support even when the real partition has it.
On the Dell-sized host fixture, the two mount phases read exactly the same
33283072 bytes each, but produce different backend request counts:

| Phase | Backend requests | Average request behavior |
| --- | ---: | --- |
| Recovery preview | 65006 | 512-byte sector requests |
| Reopened underlying device | 8130 | Mostly 4 KiB runs |

With only sector callbacks throughout, a complete mount issues 130170 backend
read requests. With underlying-device runs, it issues 73160, because the preview
still falls back to sectors. These counts are not the logical `requests` field
from the kernel profile: that field counts the ext4 block-wrapper requests and
can conceal downstream sector splitting.

A preview optimization must preserve pending journal overlays. Blindly forwarding
reads to the underlying disk would violate recovery-preview correctness.

## 3. Boot: validation scales with capacity and is repeated

The mounted journal path analyzes the journal, validates a previewed filesystem,
replays it, then reopens and validates the resulting filesystem. This is deliberate
before/after validation, not permission to skip recovery or corruption checks.

[`e4_open_journal_files_owned()`](../../src/fs/ext4_transaction.inc) verifies
allocation bitmaps and free counts. [`e4_orphan_validate()`](../../src/fs/ext4_orphan.inc)
constructs ownership/reference state, examines allocated inodes/directories and
compares block ownership against group bitmaps. Work therefore grows with groups
and capacity even when very few files exist. Each clean mount performs both the
preview and reopened ownership checks.

Same factory files, native host probe, underlying runs enabled:

| Filesystem size | Groups | Backend read requests per mount | Read MiB | CRC MiB processed |
| --- | ---: | ---: | ---: | ---: |
| 256 MiB | 2 | 4332 | 3.77 | 5.93 |
| 4 GiB | 32 | 5476 | 4.76 | 6.41 |
| 32 GiB | 256 | 13556 | 11.78 | 9.94 |
| Dell root, 238.41 GiB | 1908 | 73160 | 63.56 | 36.03 |

The installation expands to nearly the entire target disk. Its eager group
initialization writes and reads back block/inode bitmaps and eight inode-table
blocks for newly added groups. The Dell-sized expansion alone issued 19302
4 KiB writes (79060992 bytes) and 19309 reads in the host adapter. Those callbacks
would each revalidate the target on the actual installation path.

The instrumented installer performs three detached root mounts: provisioning,
persisted host-key verification, and final installation-log update. Each repeats
the preview/reopened validation. The independently required persistence checks
must be retained while evaluating whether their scope can be narrower.

The earlier Dell's first captured profile reports:

- 18557 logical bitmap reads; 15414 ext4 wrapper requests.
- 14135948065 bitmap-read cycles, approximately **7.45 accumulated seconds** at
  its reported TSC frequency of 1897227878 Hz.
- 41097384 checksum bytes and 2877689228 checksum cycles, approximately
  **1.52 accumulated seconds**.
- No reported read failures or timing anomalies in that snapshot.

These are accumulated instrumented durations, not a complete boot-stage breakdown.
The host fixture differs from the user's personalized root and does not reproduce
all service activity. It does independently reproduce the scaling and mechanisms.

## 4. CRC implementation and compiler settings are expensive

[`e4_crc_impl()`](../../src/fs/ext4.c) computes reflected CRC32C one bit at a time:
eight iterations per byte. Cached metadata is still checksum-validated on each
use. The default kernel CFLAGS in the [Makefile](../../Makefile) select no
optimization level, so GCC defaults to `-O0`; several specific modules override
this individually. ext4 itself does not.

The workload below performs 100 repeated path lookups and metadata queries,
rotating through the five installed `/usr/bin` applications. It reads no app data.
All variants retain checksum validation and process the same 2652400 checksum
bytes. The table variant uses a portable 256-entry CRC32C table with the same
polynomial/state conventions; it does not depend on SIMD or CPU extensions.

| Host probe configuration | Median lookup workload ms | Backend read requests |
| --- | ---: | ---: |
| `-O0`, bitwise CRC, 8 cache slots | 44.573 | 480 |
| `-O0`, table CRC, 8 slots | 10.438 | 480 |
| `-O2`, bitwise CRC, 8 slots | 25.921 | 480 |
| `-O2`, bitwise CRC, 64 slots | 23.216 | 300 |
| `-O2`, table CRC, 8 slots | 6.153 | 480 |
| `-O2`, table CRC, 64 slots | 6.049 | 300 |

At `-O0`, table CRC reduces this workload by about 4.3× with unchanged I/O and
validation coverage. On the large host fixture it reduces median complete mount
from 2334 ms to 1839 ms. This is a proof of opportunity, not a production CRC
acceptance campaign. A global kernel optimization change needs a separate ABI,
stack, exception-recovery and SMP regression campaign.

## 5. Cache coverage is small and excludes directory blocks

The clean metadata cache has eight 4 KiB entries (32 KiB at this geometry), FIFO
replacement, and covers superblocks, descriptors, bitmaps and inode-table blocks.
Directory blocks and ordinary data bypass it. Lookups refresh parent/child inode
metadata and scan/checksum directories before reusing a cached VFS node.

In the repeated lookup workload, increasing slots to 64 eliminates 180 backend
metadata reads but leaves 300 directory reads: 1.23 MB remains after warmup.
It does not reduce checksum bytes. Increasing this cache alone is therefore an
incomplete fix. Caching directory data or validated metadata must preserve the
existing invalidate-before-write and publish-only-after-durability contract,
including permission changes, replacement, inode reuse and failure/taint paths.

## Recommended implementation order

1. **Restore bounded bulk callbacks in installer partition I/O and an overlay-aware
   journal-preview read path.** Preserve whole-run bounds, identity validation,
   failure propagation, no write retries and quarantine. This removes avoidable
   amplification within the existing design.
2. **Replace bitwise CRC32C with a verified portable implementation.** Differential
   vectors, Linux filesystem oracles and corruption/cut tests must prove identical
   acceptance/rejection. Measure selective storage compiler optimization separately.
3. **Redesign the cost of per-operation identity checks and small payload chunks.**
   The actual command counts warrant a focused installer safety/design review.
   Keep exclusive target binding, namespace replacement detection, readback and
   readiness/durability ordering; do not simply delete checks or enlarge DMA runs.
4. **Reduce repeated whole-filesystem work without weakening admission.** Separate
   mount proof cost from targeted host-key/log verification, improve bitmap/range
   processing and evaluate lazy-group handling only with complete allocator and
   recovery evidence. Keep both replay correctness and disk ownership guarantees.
5. **Tune caching after the above measurements.** Evaluate directory and validated
   metadata caching with a bounded memory budget and complete coherence tests.

Future acceptance should time preflight, payload copy, expansion, root admission,
provisioning, final verification and log finalization independently, and count
IDENTIFY/data commands and actual transfer sizes. Include BIOS/UEFI × SMP=1/4,
small and Dell-sized sparse roots, interrupted installs, namespace identity changes,
Linux `e2fsck`/file hashes and physical Dell cold boot/install measurements.

## Investigation failures retained

Initial host-probe builds exposed missing harness includes/adapters; these were
fixed before counting successful runs. Two instrumented-media attempts were
rejected before target writes: first for mismatched boot/payload kernel banks,
then for primary/backup FAT FSInfo divergence after host mtools edits. The final
image used matching padded banks, recalculated payload hashes and the producer's
existing FSInfo-copy procedure. Validation was not bypassed. All cases and logs
remain available; the final virtual installation and installed-root audit pass.

Disposable images were removed after successful checks. Probe sources, patch,
receipts and logs remain; the seed can be extracted again from the unchanged
published installer. No new installation image was published by this investigation.
