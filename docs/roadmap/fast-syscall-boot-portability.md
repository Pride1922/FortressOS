# Fast-syscall boot self-test portability (2026-10-27)

A user-provided Latitude 5510 photograph ends at Phase 7 checkpoint 5,
`Insufficient runnable switches during fast syscalls! Switches: 1`.
This is an explicit self-test halt, after both worker exit codes and nonzero
preemption counts passed, rather than evidence of a SYSRET exception.

Modes 5/6 previously performed 60 writes separated by fixed 500,000-iteration
compute loops. That workload need not span enough 20 ms scheduler quanta on
every processor. Both embedded workers are pinned to the calling CPU; ordinary
VFS process placement is unrelated to this test.

The workers retain all 60 checked writes and user-stack sentinels, and now
continue CPU work and fast query syscalls until each receives at least eight
CPU ticks. `SYS_PROCINFO(PROC_INFO_SELF)` reads the current worker's ticks.
`CLOCK_MONOTONIC` limits execution to a ten-second seconds-counter difference
(approximately nine to ten seconds). Query errors or deadline expiry exit 17.
The timeout depends on a progressing monotonic clock; it is not a watchdog
for a completely frozen CPU/clock. No voluntary sleep or yield supplies the
required preemption evidence.

The kernel still requires both successful exits, nonzero preemption counts,
and at least two runnable-user switches; it also checks the eight-tick minimum
and reports observed ticks. Existing hostile-return checks, PMM/page-table/stack
reclamation and heap checks remain active. Scheduler behavior, syscall entry,
boot initialization order and storage safety gates are unchanged.

## Verification

Commands run in WSL Ubuntu-24.04:

```
make -j4 bin/fortress.iso
python3 scripts/test_fast_syscall_boot.py --accel kvm
python3 scripts/test_fast_syscall_boot.py
python3 scripts/test_fast_syscall_boot.py --accel kvm --probe fast
python3 scripts/test_fast_syscall_boot.py --accel kvm --probe timeout
make -j4 bin/fortress.img
```

- Normal image: BIOS/UEFI x SMP=1/4 x KVM/TCG, **8/8 PASS**. Each worker
  records at least eight CPU ticks and four preemptions; four runnable-user
  switches recorded in every boot.
- Private fast-workload image: BIOS/UEFI x SMP=1/4, **4/4 PASS**. Each worker's
  compute loop is reduced from 500,000 iterations to one. This directly
  exercises fast workload completion without weakening the production checks.
- Private expired-deadline image: BIOS/UEFI x SMP=1/4, **4/4 PASS**. Both workers
  exit 17 and checkpoint 5 refuses to pass. This injects deadline expiry; it
  does not simulate a failed hardware timer.

`make test-fast-syscall-boot` builds the ISO and runs the four TCG cases.
The runner attaches only the boot ISO and private UEFI variable storage, with
no fixture or physical data disks. Private probes replace only the embedded
worker bytes in a temporary ISO. Logs are in `build/fast-syscall-boot/`.
Assertions cover checkpoint completion, both exits/ticks/preemptions, switch
count and resource checks. These are boot-gate tests, not full login tests.
The combined live/installer USB image build completed its existing GPT,
filesystem and payload audits. Its extracted `boot/fortress.elf` is byte-identical
to the tested kernel. Both `bin/fortress.iso` and `bin/fortress.img` were rebuilt.

The first expired-deadline runner attempt stopped after reading a partially
written UART failure line. The runner now waits for a complete diagnostic line;
the subsequent four-case expired-deadline matrix passed. This was a harness
observation race, not a kernel pass.

No physical acceptance is claimed. Retry the rebuilt image on the Latitude
5510 and other laptops. SATA/AHCI remains a separate missing driver/backend
feature and is not addressed by this boot-test correction.
