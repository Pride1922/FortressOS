# Graphics cross-core lifecycle audit

Implemented 2026-10-26. This extends the graphics foundation without adding UI
features. The framebuffer lease belongs to the whole address space; thread
liveness and address-space quiescence are separate predicates.

## Defect and implementation

The real AP stale-write probe faulted correctly, but initially exposed a different
kernel bug: process-wide exit reset thread_count to one and published the exiting
worker's TID. A retained sibling could outlive process metadata, and the parent
could receive the wrong status. The initial failing log is retained in
`build/fb-campaign-1793036632487285353/bios.log`.

The shared process now retains leader metadata until final reaping. Process exit
publishes one group-exit request/status under the process lock; blocked/running
siblings observe it and terminate through ordinary unlocked user-return paths.
The last reaper closes descriptors, destroys the VM and publishes exit accounting
under the canonical PID. SIGKILL also reaches a surviving group after leader
SYS_THREAD_EXIT. Kernel liveness/wait queries recognize the surviving group.
Thread creation cannot resurrect an exiting group. Rank-1 process, scheduler,
join-table and registry locks are acquired separately.

Unjoined completion records now carry CR3 ownership and are reclaimed before that
physical root can be reused. Exhaustion returns EAGAIN with allocation rollback;
it no longer creates a thread without a completion record. No production observer
syscall, placement hook or fault-injection ABI was added.

## Acceptance contract

The disposable campaign pins raw probe threads before publication, observes
actual CPU/affinity, and records the real TLB interrupt's CPL, active CR3 and
preemption count. A second snapshot-only observation at the actual #PF entry
requires the same count at priming, shootdown and fault: no intervening context
switch may clear a stale translation and mask broken revocation. Both accepted
firmware runs record CPU 1 counts 0 / 0 / 0. After release returns, a replacement process fills
the framebuffer. The old AP's stale store must produce CPU-1 #PF/SIGSEGV at
0x6000000000, process status 139, and leave every replacement pixel intact.
Leader-thread exit is tested separately from SYS_EXIT. Final-thread exit without
release and SIGKILL after leader-thread exit must reclaim the root and permit
acquisition by another process.

Accounting includes two unjoined AP workers per child, anonymous RAM and VRAM.
It compares exact allocated-frame bitmaps, free/table/deferred counts, live heap
bytes and committed heap capacity at measured cycles 25/50/75/100. Snapshot-only
observers wait for both physical-root reclamation and final-reaper completion.
The kernel heap retains backing after growth: an initial strict run detected a
158-page capacity increase with identical live heap use, retained in
`build/fb-campaign-1793037711047122032/bios.log`. The test therefore explicitly
allocates/frees 4 MiB reusable heap capacity, then runs five warmup cycles before
its baseline. It does not permit later bitmap or heap drift. A separate negative
boot leaks one PMM frame per child and must fail at cycle 25 with a 25-frame delta.

Contention uses four independent CR3 owners pinned to CPUs 0–3, a common future
start deadline, start skew below 100 ms, ten seconds of repeated acquire/map/write/
release, per-worker one-second progress, EBUSY and successful acquisitions for
each worker, and guest/host watchdogs. It verifies bounded liveness and isolation;
it is not a formal proof of SMP safety.

## Evidence

Acceptance completed on BIOS and UEFI. `make test-fb-campaign` PASS, with
`build/fb-campaign-1793038442568909425/{bios,uefi}.{log,json}`,
`source-manifest.json` and `isolation.json`. The separate negative control using
the exact same frozen workspace PASS (expected rejection) in
`build/fb-campaign-1793038570376070301/`. Reuse command:

```
python3 scripts/test_fb_campaign.py --workspace /mnt/c/Sources/FortressOS/.codex-remote-attachments/ext4-phase9/guest-workspace-3id_j8se --modes negative
```

All eight workers completed ten seconds, each with successful acquisitions and
EBUSY responses:

| Firmware | CPU | Successful acquire/map/release | EBUSY |
| --- | ---: | ---: | ---: |
| BIOS | 0 | 174 | 1,237,833 |
| BIOS | 1 | 176 | 1,318,724 |
| BIOS | 2 | 174 | 1,318,727 |
| BIOS | 3 | 176 | 1,291,585 |
| UEFI | 0 | 168 | 1,256,897 |
| UEFI | 1 | 186 | 1,243,022 |
| UEFI | 2 | 171 | 1,293,025 |
| UEFI | 3 | 168 | 1,305,728 |

Every normal and negative campaign boot records rejected PMM frees 0 -> 0.
Both production preservation guards pass; observer symbols/private probe are
absent from production ELF/initramfs.

Regression evidence:

- `build/fb-final-host-accepted.log`: pthread, crash-dump, process, signal, stop,
  actual concurrent final-reaper, framebuffer/VMM/console, compositor (6/6), ELF
  (16,388 byte/canary cases plus actual-loader rollback), and ABI host targets all
  PASS under their configured sanitizers.
- `build/fb-pthread-final.log`: actual pthread BIOS/UEFI x SMP=1/4, 4/4 PASS.
- `build/fb-crash-final-retry.log`: actual crash-dump BIOS/UEFI x SMP=1/4, 4/4 PASS.
- `build/fb-s8-regression.log` and `build/fb-s8-smp4.log`: process/signal/stop
  BIOS/UEFI x SMP=1/4 PASS. The former retains the earlier target-name mistake
  alongside the successful rerun; the latter is a clean SMP=4 run.
- `build/fb-final-graphics-regression.log`: exclusive compositor ownership 4/4,
  100-cycle teardown 4/4, compositor text/cursor/window pixel acceptance 2/2 PASS.
- `build/fb-teardown-final.log`: final-kernel teardown 4/4 PASS, every intermediate
  checkpoint exact, rejected frees 0 -> 0.

Two old harness assumptions were repaired: the disposable S8 image now replaces
the current /bin/login startup entry as well as /bin/shell; the host signal
permission test uses bound unprivileged credentials and the credential-aware
user API, and separately verifies the trusted kernel API. Production login and
signal permission policy were not changed. A subsequent crash regression boot
once timed out before any serial output (`build/fb-crash-final.log`); the unchanged
rerun completed all four configurations. Its cause is not established.
The runner captures source/observer hashes, kernel/probe/initramfs hashes, serial
logs and actual PMM rejected-free counters via QMP with all vCPUs stopped. It
checks that production artifacts remain byte-identical and that neither observer
symbols nor the private probe are present in production ELF/initramfs. All test
boots attach no data disks. No physical-hardware acceptance is claimed.
