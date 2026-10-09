# SMP panic capture — 2026-10-09

Step 1 is implemented in `scripts/capture_smp_panic.py`. This is diagnostic
capture, not a benchmark or a correctness gate. No kernel behavior was changed
for this step.

## Capture behavior

The runner preserves the input ISO and ELF, records their SHA-256 hashes, and
checks that the ELF exactly matches `/boot/fortress.elf` inside the ISO before
booting. Each fresh BIOS/QEMU TCG run uses only the ISO, with no data disks.
The runner now also accepts `--firmware uefi`, using paired OVMF 4M code and
a disposable variables copy; its argv check permits only those pflash drives.
It retains serial output, QEMU stderr, exact arguments, commands and outcomes.

No debugger client connects while the guest runs; there are no breakpoints,
single steps or guest writes. On `KERNEL PANIC`, `[FATAL]`, a timeout or another
runner failure, QMP stops all CPUs before GDB attaches. Capture includes:

- QEMU registers for every CPU, including DR6/DR7, CR3 and segment state.
- GDB registers, backtraces, instruction bytes/disassembly and stack reads.
- CPU-local, scheduler, current-thread, TLB mailbox and VMM registry state.
- Raw usable kernel stack slots wherever mapped; individual failed reads are
  logged without suppressing the remaining capture.
- With `--dump-ram`, a physical-memory ELF core for offline inspection.

GDB disconnects without resuming the guest. The runner records the paused state
and terminates/reaps QEMU. A full RAM capture requires roughly 2.2 GB of disk
space per failed 2 GiB guest run.

## Usage

From WSL Ubuntu-24.04 at `/mnt/c/Sources/FortressOS`:

```sh
make capture-smp-panic SMP=8
```

This builds the current ISO and runs up to five fresh SMP=8 sessions, seven
repetitions each of `cpu_scale`, `spawn_wait`, `signals` and `pipes`. It stops
after the first failure and retains evidence under a timestamped
`build/panic-capture-*` directory. GDB, QEMU and xorriso must be available.

To investigate the saved instrumented baseline without rebuilding it:

```sh
python3 scripts/capture_smp_panic.py \
  --iso build/smp-perf-baseline/fortress.iso \
  --elf build/smp-perf-baseline/fortress.elf \
  --output build/panic-capture-baseline-next \
  --cpus 8 --runs 5 --reps 7 --dump-ram
```

The output directory must be new. `no-panic-observed` means only that the bounded
attempt completed; it does not establish that the original panic is fixed.

## Verification and retained evidence

| Run | Evidence directory under `build/` | Result |
| --- | --- | --- |
| Healthy capture control, saved baseline, SMP=1, signals x1, `--snapshot-after-run` | `panic-capture-smoke-20261009` | GDB completed; guest remained paused |
| Saved baseline, five fresh SMP=8 runs, all four workloads x7 | `panic-capture-baseline-20261009` | Five `no-panic-observed` outcomes |
| Current binary, two fresh SMP=8 runs, all four workloads x7 | `panic-capture-current-20261009` | Two `no-panic-observed` outcomes |
| Deliberate timeout control, current binary, SMP=8, spawn_wait x31, timeout 10 seconds | `panic-capture-timeout-control-20261009` | Expected timeout; all eight CPU register sets, GDB exit 0, RAM core retained, final paused state, QEMU reaped |

The timeout control's `guest-memory.elf` is 2,164,464,539 bytes and was verified
as an ELF64 x86-64 core. It validates the failure capture path; it is **not a
reproduction of the panic**. Some stack reads can cross an unmapped guard or
refer to unused slots; these are explicitly logged. Python compilation and the
Make target dry run passed. These runs provide no physical-hardware or
performance-improvement claim.

## Original failure remains open

`build/smpbench-baseline-smp8-run1.log` recorded a BSP debug exception during
signals repetition 5 after cpu_scale/spawn_wait. Against the saved ELF,
RIP `0xffffffff8005d216` resolves to `spin_debug_assert_unheld`, immediately
after its call to `cpu_current`. The recorded RFLAGS were `0x3d42`, including
unexpected TF. CPU 4 subsequently reported a TLB acknowledgement timeout after
the BSP halted. That timeout alone does not establish the initiating cause.

The original log lacks the frozen all-CPU register/stack/RAM evidence now
available. Seven fresh SMP=8 attempts did not reproduce it, so no root cause or
fix is claimed.

One source-review lead is the scheduler handoff: `thread_yield` publishes its
outgoing task in the run queue and releases the scheduler lock before
`switch_context` saves its RSP; `sched_steal_work` does not visibly gate stealing
on completion of that save. Benchmark workers are pinned, but the benchmark
parent and spawn_wait noop children can be unbound. This is a hypothesis to
investigate using a captured failure, not a diagnosis of the recorded exception.
No scheduler change was made in this step.
