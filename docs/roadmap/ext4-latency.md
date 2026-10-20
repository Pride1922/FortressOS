# EH-5 initial latency baseline — 2026-10-20

This campaign measures the current synchronous implementation. Production locking,
barriers, syscall ABI, caches, boot ordering and physical write gates are unchanged.
EH-5 is not complete: installed-root package scale, SSH latency, physical timing,
directory/cache profiling and before/after optimization acceptance remain open.
EH-1B remains separately open.

## Reproduction and measurement boundaries

- `make test-ext4-latency-host`: ASan/UBSan actual mounted VFS/EXT4/JBD2;
  4 KiB blocks with 512/4096-byte sectors. Stage 10 MiB as 640 writes of
  16 KiB, measure final sync separately, then inject 100 ms into every host
  flush callback. Start an independent log append while sync owns exclusion.
  Record pthread exclusion wait/hold samples, write latency distributions,
  sector write/flush counts and cumulative flush time by JBD2 writer state.
  Fail one delayed flush, assert recovery-required and no second-sync I/O,
  remount/recover, and independently audit both images with Linux.
- `make test-ext4-latency`: builds a probe ISO only in a copied workspace;
  no release image build. BIOS/UEFI x SMP=1/4 x NVMe/USB BOT on sole temporary
  regular-file data disks. The real Ring 3 probe stages 10 MiB in 640 syscalls
  and spawns an ordinary reader/logger child after the first write. Its 64
  iterations perform fstat, retained reads and four-byte appends. A sample
  counts as overlapping only when the observed staging size is greater than
  zero and below 10 MiB. Linux fsck and exact staging/log bytes audit each disk.

Host timing includes sanitizer, pthread scheduling and a deliberately expensive
whole-image memory copy at each modeled flush. It measures the model, not the
latency of NVMe, USB, IRQ exclusion or the FortressOS scheduler. The 100 ms value
is a diagnostic perturbation, not a product latency budget. A concurrent append
also incurs six of those delayed barriers itself. The callback sleeps only in
the host adapter; no sleeping is introduced under a kernel lock.

Guest durations are serialized raw TSC deltas, without converting through
interrupt-driven uptime: IRQ exclusion can delay timer delivery. Host wall time
includes process scheduling, serial collection and harness overhead. Do not
convert these values to physical-device milliseconds or treat SMP=1/4 as
statistical repetitions. Guest syscall durations include copying, lock waiting
and I/O; they do not isolate kernel IRQ-off intervals. JBD2-state timings in
the host report cover flush callbacks only, not the entire commit/checkpoint.
There is no SSH or network load in these disk fixtures.

## Findings and decision

Both host geometries count **3,840 staging flushes**, comprising **2,560 commit
flushes and 1,280 checkpoint flushes**. Each 16 KiB write pays four commit barriers
and two checkpoint barriers. There are 87,040 sector writes at 512 bytes or
10,880 at 4096 bytes: 42.5 MiB of callback writes for 10 MiB of staged payload,
excluding creation, final sync and later logging/cleanup. These are sector
callback counts in this restricted, initially empty fixture, not physical media
write amplification or an arbitrary fragmented-workload bound.

The subsequent clean-orphan final sync performs **one flush and zero writes**.
The delayed sync holds host exclusion for approximately 102 ms; the concurrent
append waits approximately that long and takes approximately 715 ms overall,
including its own six 100 ms barriers. The failed flush returns EIO, publishes
RW|TAINTED|RECOVERY_REQUIRED (35), and another sync performs no I/O. All durations
are individual fixture observations, not guaranteed deadlines.

An fd-scoped syscall can improve mount selection and error attribution, but
cannot remove the 3,840 barriers already paid while staging. This campaign does
not establish that adding SYS_FSYNC is a latency fix, or that current contention
forms a deadlock cycle. No new syscall or performance change is selected on this
evidence alone. Keep current persistence guarantees.

Before changing them, agree on a foreground/SSH latency budget and a reviewed
bounded mechanism. First profile actual directory/allocation/cache work and
controller delays under installed-root package load. Any later batching,
checkpoint scheduling or moving I/O outside IRQ-save exclusion must define
ownership, sequencing, cache publication, freeze and late-error attribution and
repeat the durability/lifetime gates. A descriptor-sync API needs its separate
file/directory/lifetime/error contract even if it retains device barriers.

## Evidence and retained diagnostics

Host results, complete per-profile metrics, source/binary hashes, compiler command
and four Linux filesystem audits: `build/ext4-latency-host/manifest.json` and
adjacent logs. Timing sources are `tests/ext4_latency_host.c` and the explicitly
enabled `EXT4_HOST_LATENCY` branches in the host-only spinlock adapter; ordinary
kernel headers are unchanged.

The first private guest attempt,
`.codex-remote-attachments/ext4-latency-20261020-182429/`, was interrupted after
boot stopped at `ext4 journal startup orphan cleanup`. Its base fixture still
contained `/target.bin`; this precondition failure preceded the latency probe.
The corrected producer removes that seed, verifies it with Linux, and detects
the boot failure immediately. This attempt is not acceptance evidence.

Guest final results and preservation qualifications are recorded below after
the corrected campaign. All private builds freeze unrelated Makefile/TLS and
Watchtower compilation inputs at the recorded committed checkpoint. Shared
sources are not reset or edited by this isolation step.

Corrected guest campaign: **8/8 PASS**, with eight independent Linux fsck and
exact payload audits. Every case observes all 64 peer samples during staging.
Evidence: `.codex-remote-attachments/ext4-latency-20261020-182711/manifest.json`.
Frozen unrelated checkpoint: `f2b5dbc38d695b0549a73389cbb8e43211f84fbb`.
The recorded filesystem/syscall/probe/runner sources and existing shared `bin/`
files remain unchanged during this gate; both change lists are empty. The probe
has a measured maximum compiler stack frame of 112 bytes, with buffers in BSS.
Its build/packaging rules are injected only into the copied Makefile. Ordinary
builds have test runner targets but no probe dependency or packaging rule.

| Firmware | CPUs | Transport | Write p95 (million TSC units) | Peer max (million TSC units) | Final sync (million TSC units) |
| --- | --- | --- | --- | --- | --- |
| BIOS | 1 | NVMe | 135.87 | 193.26 | 0.723 |
| BIOS | 4 | NVMe | 96.64 | 244.45 | 0.400 |
| UEFI | 1 | NVMe | 96.87 | 168.75 | 0.412 |
| UEFI | 4 | NVMe | 87.94 | 233.70 | 0.569 |
| BIOS | 1 | USB BOT | 99.42 | 161.77 | 0.571 |
| BIOS | 4 | USB BOT | 91.16 | 214.20 | 0.696 |
| UEFI | 1 | USB BOT | 138.17 | 184.80 | 0.511 |
| UEFI | 4 | USB BOT | 92.76 | 250.64 | 0.634 |

These are one-run observations under QEMU TCG, not transport rankings. Total
host-observed probe duration is 9.69–11.21 seconds. A peer sample includes fstat,
read and append; its maximum is not an isolated read or log-write latency. Final
sync may overlap the peer's remaining writes; it selects the current global
storage path. The fixture has one mounted data filesystem, so it does not
measure savings from avoiding unrelated mounts. Some host/build/regression work
ran concurrently with this campaign; raw timing variability remains qualified.

Final host campaign: **2/2 PASS**, four Linux filesystem audits. Baseline staging
is 6.51–6.59 seconds in the memory-copy model, with write p95 11.14–11.28 ms and
final sync 1.65–1.72 ms. Those values illustrate model overhead, not device
performance. The EH-3 regression `make test-ext4-health-host` also passes all
six geometries and 42 Linux audits with measurement disabled. No transaction,
ABI or kernel exclusion changes were made, so the full historical interruption
matrix was not rerun for this measurement-only change.
