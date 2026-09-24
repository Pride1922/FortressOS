# SMP Piece 6 — Memory safety implementation and evidence

Status (2026-09-24): **6A implemented, verification pending.**
6B–6D remain planned. This is not acceptance of Piece 6.
Approved design: [implementation plan](smp-piece6-plan.md).

## 6A: Boot memory readiness

- All PMM allocation paths and the bitmap's entire backing are restricted to
  physical addresses below 1 GiB before VMM readiness. Fully contained usable
  pages alone enter the free bitmap; overflowing ranges are excluded.
- VMM constructs RAM mappings with bounded range arithmetic, activates the
  kernel CR3 and reads it back before publishing readiness. PMM verifies that
  publication and the active root before lifting its ceiling under rank 4.
- SMP refuses AP release without readiness; each AP checks readiness and its
  active kernel CR3 before publishing online. AP bring-up remains serialized.
- Managed free pages include high RAM even during boot. The new
  `pmm_get_allocatable_pages()` metric reports only currently eligible free
  pages, so capped OOM cannot be mistaken for exhausted managed RAM.
- Bootloader reclamation is explicitly disabled (returns zero). No callers
  used it previously. Limine handoff/module lifetime must be proven separately.
- No shootdown protocol, context-switch ABI or lifetime registry change in 6A.
  The later `vmm_space_t` kernel record will be static, as the approved plan
  specifies. Existing Phase 5 contention/timeout limitations remain for 6C.

## Verification handoff — user executes

Run in WSL Ubuntu-24.04 at `/mnt/c/Sources/FortressOS`:

```bash
make
make test-pmm-boot-host
make test-smp-memory-boot
python3 scripts/test_smp_memory_boot.py --ram 256M --cpus 1
python3 scripts/test_smp_memory_boot.py --ram 8G --cpus 8
# Optional larger QEMU case; requires sufficient host memory:
python3 scripts/test_smp_memory_boot.py --ram 32G --cpus 8
make test-smp-percpu
make test-smp-ipi
```

The host harness compiles actual PMM code with single-threaded lock and
readiness/CR3 shims under host ASan/UBSan. It checks low-memory exhaustion
despite free high RAM, contiguous allocation crossing the ceiling, premature
unlock rejection, alignment, partial usable-page exclusion, exact allocation
set recovery and bounded test-mode parsing. It does not prove SMP exclusion
or hardware CR3 behavior. Concurrent stress/TSan belongs to 6B.

The QEMU runner defaults to BIOS and UEFI, 1/4/8 CPUs, 2 GiB. It creates a
temporary ISO with `smp_memory_test=boot`; normal `limine.conf` and build ISOs
are unchanged. No data disks are attached. UEFI uses read-only paired OVMF
code and a disposable vars copy. Each boot has a 300-second deadline and
bounded terminate/kill cleanup. Logs are `build/smp-memory-boot-*.log` and
`.stderr`; no prior-run log can satisfy a new run.

Required evidence: early allocator assertions and exact cleanup; kernel CR3
and unlock ordering; full-page HHDM probes at every applicable threshold;
readiness before AP release; all requested APs online; shell prompt.
Unsupported RAM thresholds explicitly skip. On a 32 GiB run, all of 1, 2, 4,
16 and 30 GiB thresholds must pass. Actual allocated addresses may exceed the
threshold when a page is reserved. Every page is checked word by word using
an address-derived pattern and its complement, then returned to the allocator.
Static snapshot storage adds 2 MiB of kernel BSS, retained in each baseline.

For Dell 6A evidence, add `smp_memory_test=boot` to the selected Limine entry's
kernel arguments, preserving its existing PARTUUID/mode arguments. On 8 CPUs
and 32 GiB, require all five high probes, both cleanup markers, all seven APs
online and interactive shell/storage behavior. Save the log using the existing
explicit USB policy. These checks do not claim later concurrent memory stress.

## Results

| Check | Result |
| --- | --- |
| Kernel build | Not run; user executes |
| Host ASan/UBSan | Not run; user executes |
| BIOS/UEFI matrix and regressions | Not run; user executes |
| Dell 5590 | Not run; user executes |

The agent writes code and tooling; the user runs them. Both review supplied
evidence before recording acceptance. Historical Phase 9H hardware evidence
is preserved, but its documented cap/unlock was missing from the Phase 5
checkout. This change supplies that invariant; the available history did not
establish how the discrepancy arose.
