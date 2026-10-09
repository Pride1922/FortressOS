# Spawn and pipe startup diagnostics — 2026-10-09

The user subsequently ran this build on Dell; see the
[physical subphase findings](smpbench-spawn-dell-results.md).
This is opt-in attribution instrumentation, not a performance fix. Existing
`smpbench -p` runs now enable self-owned kernel spawn counters before each
worker's timed workload, and snapshot/disable them afterwards. Plain runs
do not enable counters. Children do not inherit enablement; all TCBs gain
bounded counter storage, so this build must not be compared to older builds
as if instrumentation had no footprint or allocation effect.

## ABI and ownership

`SYS_SPAWN_PROFILE` (55) accepts `(action, output, sizeof(spawn_profile_t))`.
READ=0 snapshots, ENABLE=1 resets/enables, DISABLE=2 snapshots/disables.
The version-one layout is 160 bytes, defined in `spawn_profile_abi.h`, with
a compile-time size check. Bad action or size returns EINVAL before copying;
unmapped, overflowing, kernel or non-writable output returns EFAULT before
changing state. The output is validated writable through the existing VMM
validator. Only the calling task's data is accessible; there is no global
profiling lock or cross-task pointer API. Entry assembly and return frames
are unchanged. There is no allocated profiling buffer or exit cleanup.

`sp_total` measures `process_spawn_from_vfs_group`, including both reaper
calls and cleanup, but excludes syscall argument validation/copying.
`p_spawn - sp_total` therefore includes that work, syscall entry/exit,
measurement overhead and any scheduling between the timestamps.
Counters accumulate elapsed TSC cycles including preemption and lock waits:

- `sp_file`: identity reservation, file lookup/read and target selection.
  Initramfs images use their resident data directly; zero physical file reads
  are possible. This field is not a pure disk-I/O measurement.
- `sp_reap`: both spawn-path dead-task reaper calls.
- `sp_elf`: complete ELF validation and loading.
- `sp_ustack`: argument/environment stack formatting.
- `sp_kstack`: kernel stack allocation, including its VMM/PMM work.
- `sp_tcb`: TCB allocation, initialization, identity and address-space lookup.
- `sp_fds`: descriptor inheritance/actions/CLOEXEC sweep.
- `sp_publish`: cwd/context setup, scheduler reference, process commit,
  runqueue insertion and reschedule notification.
- `sp_cleanup`: outer buffer/file cleanup and identity rollback on failure.
- `sp_other`: unassigned time; failed stages/rollback can land here.

ELF subcounters `se_space`, `se_alloc`, `se_map`, `se_copy` separate root
creation, leaf-frame allocation, mapping and zero/copy operations (including
restorer and stack). They are **inside** `sp_elf`; add neither to `sp_total`
nor to `sp_elf`. Root creation and mapping themselves include their internal
allocation work. The remaining ELF interval includes validation/loop logic
and instrumentation. Counters invalidate on clock reversal or overflow.

## Pipe writer startup

For a child of a profiling caller, record its queue timestamp before releasing
the owning scheduler lock. Record first selection in all four switch paths
under the owning lock, without changing selection, IRQ or CR3 behavior.
The pipe writer samples its user entry before producing the first header.
It reports `writer_queued_to_run` and `writer_run_to_entry` in a versioned
104-byte private trailer, with `writer_startup_valid=1` on valid ordering.
These are elapsed cycles; first selection precedes the actual context/CR3
exchange, so run-to-entry includes that transition and user startup.
Staged tasks are not given an initial queue timestamp by this path.

Reader header waiting and writer startup overlap; they are not additive.
The timestamps require an ordered shared TSC across CPUs. Reversed ordering
fails validation, rather than reporting a negative duration. This is a
diagnostic check, not proof of cross-CPU clock synchronization.

## Validation and evidence

- Build with strict kernel/user warnings passes.
- Actual benchmark/formatter/writer adapters under ASan/UBSan pass, including
  larger trailer partial writes, overflow/reversed clock and disabled helper.
- Evidence rejection tests cover spawn partition, ELF containment, call
  counts/failures and existing phase/writer corruption.
- BIOS and UEFI TCG SMP=8, spawn_wait and pipes, one warmup plus two timed
  repetitions each: complete profiles, spawn partitions, ELF containment,
  birth stamps and readiness validation pass; no panic observed. Guest
  workers additionally check bad action/size, null, overflowing, kernel and
  read-only output rejection before enabling timed profiling.
- UEFI USB SMP=1 signals/pipes profiles pass. A five-repetition profile log
  larger than 4096 bytes saves to `/mnt`; after clean shutdown an independent
  debugfs read validates all records and offline `e2fsck -fn` passes. The
  source image is unchanged. This specifically verifies the capture path
  missed in the original Dell handoff.
- BIOS TCG SMP=8 plain spawn_wait/pipes control (one warmup plus one timed
  repetition each) passes readiness and completion with no diagnostic fields.

Artifacts: `build/spawn-diag-{bios,uefi}-20261009`,
`build/spawn-diag-analysis-20261009.json`, `build/spawn-diag-usb-20261009`.
Plain control: `build/spawn-diag-plain-20261009`.
QEMU is correctness/attribution-format evidence only, not an optimization
effect or a physical acceptance claim. Hardware execution remains pending.
The analyzer preserves these nested subphases and writer startup in JSON.

## Dell procedure

The new frozen bundle is `build/dell-spawn-profile-20261009`, verified against
its embedded kernel, initramfs and benchmark. Preserve the existing Dell logs
on the host before reflashing the disposable test USB. Select Persistent
Storage and verify the selected USB `/mnt` mount. Keep AC power, background
work and capture mode consistent; record model/CPU/firmware/power conditions.
Run the bundle's focused `commands.txt` one prompt at a time. It contains
single-worker references and plain/profile/profile/plain eight-worker runs
for spawn_wait and pipes. Full logs go directly to `/mnt`, not `/tmp`.
Check all summaries for `ok=1 short=0`, then sync/shutdown and export all
`dell-spawn-*.txt` through a Linux filesystem reader to `build/dell-spawn-results`.
USB collection perturbs spacing and parent collect timing; worker timings
exclude parent result printing. No optimization is selected until these
hardware subphase results are available.
